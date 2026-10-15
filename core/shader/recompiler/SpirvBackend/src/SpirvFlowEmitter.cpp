#include "SpirvBackend/SpirvFlowEmitter.hpp"
#include "PipelineSpecialization.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvEmitterInstructions.hpp"
#include "SpirvBackend/SpirvMemory/SpirvSubgroup.hpp"
#include <array>
#include <cstdio>
#include <span>
#include <vector>
#include <cstdlib>
#include <string>
#include <spirv/unified1/spirv.hpp>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ShaderRecompiler {
namespace {

void EmitKillIfBoolFalse(SpirvEmitterState& state, std::uint32_t active) {
    const auto killLabel = state.module.AllocateId();
    const auto mergeLabel = state.module.AllocateId();
    const auto inactive = state.module.AllocateId();
    state.module.AddFunction(spv::OpLogicalNot, TypeBool(state), inactive, active);
    state.module.AddFunction(spv::OpSelectionMerge, mergeLabel, spv::SelectionControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, inactive, killLabel, mergeLabel);
    EmitLabel(state, killLabel);
    state.module.AddFunction(spv::OpKill);
    EmitLabel(state, mergeLabel);
}

void EmitKillIfPixelValidMaskInactive(SpirvEmitterState& state) {
    if (state.pixelValidMaskVariable == 0) {
        return;
    }
    const auto maskValue = state.module.AllocateId();
    const auto active = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, TypeU32(state), maskValue, state.pixelValidMaskVariable);
    state.module.AddFunction(spv::OpINotEqual, TypeBool(state), active, maskValue, ConstantU32(state, 0u));
    auto keep = active;
    if (const auto helperVariable = InputVariableForKind(state, StageInputKind::HelperInvocation); helperVariable != 0u) {
        const auto helper = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeBool(state), helper, helperVariable);
        keep = Binary(state, spv::OpLogicalOr, TypeBool(state), active, helper);
    }
    EmitKillIfBoolFalse(state, keep);
}

// Block metadata is paired with BlockOrder by position (as IrProgram validation does); terminator
// targets name control-flow ids, which are not IR block ids.
const IrBlock* TargetBlock(const IrProgram& program, std::uint32_t id) {
    const auto& blocks = program.BlockOrder();
    const auto& blockInfo = program.Metadata().blockInfo;
    if (blocks.size() != blockInfo.size()) {
        throw std::runtime_error("SPIR-V control flow block metadata is inconsistent");
    }
    for (std::size_t index = 0; index < blockInfo.size(); index++) {
        if (blockInfo[index].id == id) {
            if (blocks[index] == nullptr) {
                throw std::runtime_error("SPIR-V control flow target block is null");
            }
            return blocks[index];
        }
    }
    throw std::runtime_error("SPIR-V control flow target block is missing");
}

const BlockInfo* BlockInfoFor(const IrProgram& program, const IrBlock* block) {
    const auto& blocks = program.BlockOrder();
    const auto& blockInfo = program.Metadata().blockInfo;
    if (blocks.size() != blockInfo.size()) {
        throw std::runtime_error("SPIR-V control flow block metadata is inconsistent");
    }
    if (block == nullptr) {
        throw std::runtime_error("SPIR-V control flow block is null");
    }
    for (std::size_t index = 0; index < blocks.size(); index++) {
        if (blocks[index] == block) {
            return &blockInfo[index];
        }
    }
    throw std::runtime_error("SPIR-V control flow block has no metadata");
}

void EmitReturnTerminator(SpirvValueEmitContext& ctx) {
    auto& state = ctx.state;
    if (state.loopGuardLimit != 0 && state.faultBufferVariable != 0) {
        const auto pc = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeU32(state), pc, state.loopGuardPc);
        EmitIfCondition(state, Binary(state, spv::OpINotEqual, TypeBool(state), pc, ConstantU32(state, 0u)), [&] {
            const auto hash = state.program.Resources().shaderHash;
            RecordBdaFaultWords(state, ConstantU32(state, static_cast<std::uint32_t>(hash)), ConstantU32(state, static_cast<std::uint32_t>(hash >> 32u)), ConstantU32(state, state.loopGuardLimit), EmitBinaryU32(state, spv::OpISub, pc, ConstantU32(state, 1u)), BdaAbi::FaultReason::LoopLimit);
        });
    }
    if (OrderedPixelShader(state)) {
        state.module.AddFunction(spv::OpEndInvocationInterlockEXT);
    }
    EmitKillIfPixelValidMaskInactive(state);
    if (state.stencilReplay) {
        const auto value = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeI32(state), value, state.stencilRefVariable);
        const auto reference = Unary(state, spv::OpBitcast, TypeU32(state), value);
        const auto masked = Binary(state, spv::OpBitwiseAnd, TypeU32(state), reference, ConstantU32(state, 0xffu));
        const auto offset = state.module.SpecializationConstant(TypeU32(state), PipelineSpecialization::StencilReferenceOffset, state.program.Metadata().bindings.stencilReferenceDword);
        const auto pointer = state.module.AllocateId();
        state.module.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state), pointer, state.pushConstantVariable, ConstantU32(state, 0u), offset);
        const auto expected = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeU32(state), expected, pointer);
        EmitKillIfBoolFalse(state, Binary(state, spv::OpIEqual, TypeBool(state), masked, expected));
    }
    state.module.AddFunction(spv::OpReturn);
}

// The loop guard (see SpirvEmitterState::loopGuardLimit) at a branch that may leave a loop: counts the
// evaluation and, past the limit, takes the exit and remembers the block's PC (plus one, 0 = none).
std::uint32_t GuardLoopExit(SpirvEmitterState& state, const BlockInfo& info, std::uint32_t condition, bool exitWhenTrue) {
    const auto visits = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, TypeU32(state), visits, state.loopGuardVisits);
    const auto next = EmitBinaryU32(state, spv::OpIAdd, visits, ConstantU32(state, 1u));
    state.module.AddFunction(spv::OpStore, state.loopGuardVisits, next);
    const auto tripped = Binary(state, spv::OpUGreaterThan, TypeBool(state), next, ConstantU32(state, state.loopGuardLimit));
    const auto previous = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, TypeU32(state), previous, state.loopGuardPc);
    const auto first = Binary(state, spv::OpLogicalAnd, TypeBool(state), tripped, Binary(state, spv::OpIEqual, TypeBool(state), previous, ConstantU32(state, 0u)));
    const auto pc = state.module.AllocateId();
    state.module.AddFunction(spv::OpSelect, TypeU32(state), pc, first, ConstantU32(state, info.startPc + 1u), previous);
    state.module.AddFunction(spv::OpStore, state.loopGuardPc, pc);
    if (exitWhenTrue) return Binary(state, spv::OpLogicalOr, TypeBool(state), condition, tripped);
    const auto stay = state.module.AllocateId();
    state.module.AddFunction(spv::OpLogicalNot, TypeBool(state), stay, tripped);
    return Binary(state, spv::OpLogicalAnd, TypeBool(state), condition, stay);
}

bool IsLoopMerge(const IrProgram& program, std::uint32_t block) {
    for (const auto& info : program.Metadata().blockInfo) {
        if (info.terminator.loopHeader && info.terminator.mergeBlock == block) return true;
    }
    return false;
}

bool IsContinueTarget(const IrProgram& program, std::uint32_t block) {
    for (const auto& info : program.Metadata().blockInfo) {
        if (info.terminator.loopHeader && info.terminator.continueBlock == block) return true;
    }
    return false;
}

std::uint32_t EmitWaveAny(SpirvEmitterState& state, std::uint32_t predicate) {
    const auto ballot = state.module.AllocateId();
    EmitSubgroupBallot(state, ballot, predicate);
    const auto wave = EmitWaveBallot(state, ballot);
    const auto low = state.module.AllocateId();
    const auto high = state.module.AllocateId();
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, wave, 0u);
    state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, wave, 1u);
    state.module.AddFunction(spv::OpINotEqual, TypeBool(state), result, EmitBinaryU32(state, spv::OpBitwiseOr, low, high), ConstantU32(state, 0u));
    return result;
}

std::uint32_t EmitWaveMaskBranchCondition(SpirvValueEmitContext& ctx, const BlockInfo& info) {
    auto& state = ctx.state;
    const auto kind = info.terminator.condition;
    if (kind == BranchCondition::ExecNonZero || kind == BranchCondition::VccNonZero) {
        return EmitWaveAny(state, ctx.Def(info.condition));
    }
    const auto clear = state.module.AllocateId();
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpLogicalNot, TypeBool(state), clear, ctx.Def(info.condition));
    state.module.AddFunction(spv::OpLogicalNot, TypeBool(state), result, EmitWaveAny(state, clear));
    return result;
}

std::uint32_t EmitBranchCondition(SpirvValueEmitContext& ctx, const BlockInfo& info) {
    const auto condition = info.terminator.condition;
    if (ctx.otherHalf == nullptr && IsWaveMaskBranch(condition)) {
        return EmitWaveMaskBranchCondition(ctx, info);
    }
    if (ctx.otherHalf == nullptr || condition == BranchCondition::ScalarInstruction || condition == BranchCondition::GotoVariable || condition == BranchCondition::IndirectTarget) {
        return ctx.Def(info.condition);
    }
    auto& state = ctx.state;
    const auto ballot = ctx.Ballot(info.condition);
    const auto low = state.module.AllocateId();
    const auto high = state.module.AllocateId();
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, ballot, 0u);
    state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, ballot, 1u);
    const auto kind = info.terminator.condition;
    const bool zero = kind == BranchCondition::ExecZero || kind == BranchCondition::VccZero || kind == BranchCondition::SccZero;
    const auto combined = EmitBinaryU32(state, zero ? spv::OpBitwiseAnd : spv::OpBitwiseOr, low, high);
    state.module.AddFunction(zero ? spv::OpIEqual : spv::OpINotEqual, TypeBool(state), result, combined, ConstantU32(state, zero ? ~0u : 0u));
    return result;
}

void EmitStructuredTerminator(SpirvValueEmitContext& ctx, const IrProgram& program, const BlockInfo& info) {
    auto& state = ctx.state;
    const Terminator& term = info.terminator;
    const auto emitMerge = [&]() {
        if (term.loopHeader) {
            const IrBlock* merge = TargetBlock(program, term.mergeBlock);
            const IrBlock* cont = TargetBlock(program, term.continueBlock);
            if (merge != nullptr && cont != nullptr) {
                state.module.AddFunction(spv::OpLoopMerge, ctx.Label(merge), ctx.Label(cont), spv::LoopControlMaskNone);
            }
        } else if (term.kind == TerminatorKind::ConditionalBranch && term.mergeBlock != InvalidControlFlowId) {
            if (const IrBlock* merge = TargetBlock(program, term.mergeBlock); merge != nullptr) {
                state.module.AddFunction(spv::OpSelectionMerge, ctx.Label(merge), spv::SelectionControlMaskNone);
            }
        }
    };
    switch (term.kind) {
        case TerminatorKind::Branch: {
            const IrBlock* target = TargetBlock(program, term.trueBlock);
            if (target == nullptr) {
                EmitReturnTerminator(ctx);
                return;
            }
            emitMerge();
            state.module.AddFunction(spv::OpBranch, ctx.Label(target));
            return;
        }
        case TerminatorKind::ConditionalBranch: {
            const IrBlock* trueBlock = TargetBlock(program, term.trueBlock);
            const IrBlock* falseBlock = TargetBlock(program, term.falseBlock);
            if (trueBlock == nullptr || falseBlock == nullptr || info.condition == nullptr) {
                EmitReturnTerminator(ctx);
                return;
            }
            if (const IrValue* resolved = info.condition->Resolve(); !resolved->HasImmediate() && !ctx.definitions.contains(resolved)) {
                const std::string where = resolved->Parent() == nullptr ? std::string("no block") : "block " + std::to_string(resolved->Parent()->Id());
                const std::string message = "branch condition " + std::string(IrOpcodeName(resolved->Opcode())) + " of block " + std::to_string(info.id) + " (defined in " + where + ") was not emitted before the branch";
                ctx.Fail(message.c_str());
            }
            if (ctx.Label(trueBlock) == ctx.Label(falseBlock)) {
                if (term.loopHeader) emitMerge();
                state.module.AddFunction(spv::OpBranch, ctx.Label(trueBlock));
                return;
            }
            auto condition = EmitBranchCondition(ctx, info);
            if (state.loopGuardLimit != 0 && term.trueBlock != term.falseBlock) {
                if (IsLoopMerge(program, term.trueBlock)) condition = GuardLoopExit(state, info, condition, true);
                else if (IsLoopMerge(program, term.falseBlock)) condition = GuardLoopExit(state, info, condition, false);
            }
            static const bool debug = std::getenv("APS5_SRT_DEBUG") != nullptr;
            if (debug) std::fprintf(stderr, "[spirv] block %u branches on %%%u = %s (kind %d)\n", static_cast<unsigned>(info.id), condition, std::string(IrOpcodeName(info.condition->Resolve()->Opcode())).c_str(), static_cast<int>(info.terminator.condition));
            emitMerge();
            state.module.AddFunction(spv::OpBranchConditional, condition, ctx.Label(trueBlock), ctx.Label(falseBlock));
            return;
        }
        default:
            EmitReturnTerminator(ctx);
            return;
    }
}

template<typename TArgument>
decltype(auto) DispatchArgument(SpirvValueEmitContext& ctx, const IrValue& inst, std::size_t index) {
    if constexpr (std::is_same_v<TArgument, const IrValue&>) {
        return inst;
    } else if constexpr (std::is_same_v<TArgument, const IrValue*>) {
        return static_cast<const IrValue*>(inst.Argument(index));
    } else if constexpr (std::is_same_v<TArgument, ScalarReg>) {
        return static_cast<ScalarReg>(inst.Argument(index)->Register().index);
    } else {
        static_assert(std::is_same_v<TArgument, std::uint32_t>);
        return ctx.Def(inst.Argument(index));
    }
}

template<typename TContext, typename TReturn, typename... TArguments>
void Invoke(TReturn (*emit)(TContext&, TArguments...), SpirvValueEmitContext& ctx, const IrValue& inst) {
    static_assert(std::is_same_v<TContext, SpirvValueEmitContext> || std::is_same_v<TContext, SpirvEmitterState>);
    auto& context = [&]() -> TContext& {
        if constexpr (std::is_same_v<TContext, SpirvEmitterState>) {
            return ctx.state;
        } else {
            return ctx;
        }
    }();
    constexpr bool hasInst = (std::is_same_v<TArguments, const IrValue&> || ...);
    [&]<std::size_t... TIndices>(std::index_sequence<TIndices...>) {
        static_assert(((!std::is_same_v<TArguments, const IrValue&> || TIndices == 0) && ...));
        const auto call = [&] {
            return emit(context, DispatchArgument<TArguments>(ctx, inst, TIndices - (hasInst && TIndices != 0 ? 1u : 0u))...);
        };
        if constexpr (std::is_void_v<TReturn>) {
            call();
        } else {
            static_assert(std::is_same_v<TReturn, std::uint32_t>);
            ctx.Define(inst, call());
        }
    }(std::index_sequence_for<TArguments...>{});
}

template<typename... TArguments>
void InvokeOutlined(std::uint32_t (*emit)(SpirvEmitterState&, TArguments...), SpirvValueEmitContext& ctx, const IrValue& inst) {
    static_assert(((std::is_same_v<TArguments, std::uint32_t> || std::is_same_v<TArguments, const IrValue*>) && ...));
    auto& state = ctx.state;
    constexpr std::size_t count = sizeof...(TArguments);
    if (inst.ArgumentCount() < count) ctx.Fail(inst, "has fewer arguments than its emitter takes");
    std::vector<std::uint64_t> key{static_cast<std::uint64_t>(inst.Opcode()), static_cast<std::uint64_t>(inst.Type())};
    std::array<std::uint32_t, count> types{};
    bool outlined = true;
    [&]<std::size_t... TIndices>(std::index_sequence<TIndices...>) {
        const auto describe = [&]<typename TArgument>(std::size_t index) {
            const IrValue* argument = inst.Argument(index);
            if (argument == nullptr) {
                outlined = false;
                return;
            }
            if constexpr (std::is_same_v<TArgument, const IrValue*>) {
                key.push_back(static_cast<std::uint64_t>(argument->Type()));
                if (!argument->HasImmediate()) outlined = false;
                key.push_back(argument->ImmediateU64());
            } else {
                const IrValue* resolved = argument->Resolve();
                key.push_back(static_cast<std::uint64_t>(resolved->Type()));
                types[index] = TypeId(state, resolved->Type());
                if (types[index] == 0u) outlined = false;
            }
        };
        (describe.template operator()<TArguments>(TIndices), ...);
    }(std::index_sequence_for<TArguments...>{});
    const auto resultType = TypeId(state, inst.Type());
    if (!state.sharedFloatFunctions || !outlined || resultType == 0u) return Invoke(emit, ctx, inst);
    auto function = state.outlinedFunctions.find(key);
    if (function == state.outlinedFunctions.end()) {
        std::vector<std::uint32_t> parameterTypes;
        for (std::size_t index = 0; index < count; ++index) {
            if (types[index] != 0u) parameterTypes.push_back(types[index]);
        }
        const auto functionType = state.module.Type(spv::OpTypeFunction, resultType, std::span<const std::uint32_t>(parameterTypes));
        const auto id = state.module.AllocateId();
        const auto label = state.currentLabel;
        const auto block = state.currentBlock;
        state.module.BeginHelperFunction();
        state.module.AddName(id, "outlined_" + std::string(IrOpcodeName(inst.Opcode())));
        state.module.AddFunction(spv::OpFunction, resultType, id, spv::FunctionControlInlineMask, functionType);
        std::array<std::uint32_t, count> parameters{};
        for (std::size_t index = 0; index < count; ++index) {
            if (types[index] == 0u) continue;
            parameters[index] = state.module.AllocateId();
            state.module.AddFunction(spv::OpFunctionParameter, types[index], parameters[index]);
        }
        EmitLabel(state, state.module.AllocateId());
        const auto result = [&]<std::size_t... TIndices>(std::index_sequence<TIndices...>) {
            const auto argument = [&]<typename TArgument>(std::size_t index) -> TArgument {
                if constexpr (std::is_same_v<TArgument, const IrValue*>) {
                    return static_cast<const IrValue*>(inst.Argument(index));
                } else {
                    return parameters[index];
                }
            };
            return emit(state, argument.template operator()<TArguments>(TIndices)...);
        }(std::index_sequence_for<TArguments...>{});
        state.module.AddFunction(spv::OpReturnValue, result);
        state.module.AddFunction(spv::OpFunctionEnd);
        state.module.EndHelperFunction();
        state.currentLabel = label;
        state.currentBlock = block;
        function = state.outlinedFunctions.emplace(std::move(key), id).first;
    }
    std::vector<std::uint32_t> arguments;
    for (std::size_t index = 0; index < count; ++index) {
        if (types[index] != 0u) arguments.push_back(ctx.Def(inst.Argument(index)));
    }
    const auto result = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionCall, resultType, result, function->second, std::span<const std::uint32_t>(arguments));
    ctx.Define(inst, result);
}

void EmitDirectInstruction(SpirvValueEmitContext& ctx, const IrValue& inst) {
    switch (inst.Opcode()) {
        case IrOpcode::Identity: return Invoke(EmitIdentity, ctx, inst);
        case IrOpcode::Phi: return Invoke(EmitPhi, ctx, inst);
        case IrOpcode::IAdd32: return Invoke(EmitIAdd32, ctx, inst);
        case IrOpcode::ISub32: return Invoke(EmitISub32, ctx, inst);
        case IrOpcode::IMul32: return Invoke(EmitIMul32, ctx, inst);
        case IrOpcode::Unreachable: return Invoke(EmitUnreachable, ctx, inst);
        case IrOpcode::Barrier: return Invoke(EmitBarrier, ctx, inst);
        case IrOpcode::ReadFirstLane: return Invoke(EmitReadFirstLane, ctx, inst);
        case IrOpcode::ReadLane: return Invoke(EmitReadLane, ctx, inst);
        case IrOpcode::Ballot: return Invoke(EmitBallot, ctx, inst);
        case IrOpcode::Void: return Invoke(EmitVoid, ctx, inst);
        case IrOpcode::Reference: return Invoke(EmitReference, ctx, inst);
        case IrOpcode::ReferenceU32: return Invoke(EmitReferenceU32, ctx, inst);
        case IrOpcode::UnlinkedCall: return Invoke(EmitUnlinkedCall, ctx, inst);
        case IrOpcode::CheckDenormalFree: return Invoke(EmitCheckDenormalFree, ctx, inst);
        case IrOpcode::CheckEqualU32: return Invoke(EmitCheckEqualU32, ctx, inst);
        case IrOpcode::GetUserData: return Invoke(EmitGetUserData, ctx, inst);
        case IrOpcode::GetShaderBase: return Invoke(EmitGetShaderBase, ctx, inst);
        case IrOpcode::ShaderClock: return Invoke(EmitShaderClock, ctx, inst);
        case IrOpcode::RealtimeClock: return Invoke(EmitRealtimeClock, ctx, inst);
        case IrOpcode::MeshDrawParameter: return Invoke(EmitMeshDrawParameter, ctx, inst);
        case IrOpcode::MeshArgument: return Invoke(EmitMeshArgument, ctx, inst);
        case IrOpcode::MeshRestartStart: return Invoke(EmitMeshRestartStart, ctx, inst);
        case IrOpcode::MeshAllocate: return Invoke(EmitMeshAllocate, ctx, inst);
        case IrOpcode::TessellationBase: return Invoke(EmitTessellationBase, ctx, inst);
        case IrOpcode::GetTessellationAttribute: return Invoke(EmitGetTessellationAttribute, ctx, inst);
        case IrOpcode::SetTessellationAttribute: return Invoke(EmitSetTessellationAttribute, ctx, inst);
        case IrOpcode::GetBuiltin: return Invoke(EmitGetBuiltin, ctx, inst);
        case IrOpcode::GetThreadBitScalarRegister: return Invoke(EmitGetThreadBitScalarRegister, ctx, inst);
        case IrOpcode::SetThreadBitScalarRegister: return Invoke(EmitSetThreadBitScalarRegister, ctx, inst);
        case IrOpcode::GetScalarMaskTag: return Invoke(EmitGetScalarMaskTag, ctx, inst);
        case IrOpcode::SetScalarMaskTag: return Invoke(EmitSetScalarMaskTag, ctx, inst);
        case IrOpcode::GetScalarRegister: return Invoke(EmitGetScalarRegister, ctx, inst);
        case IrOpcode::SetScalarRegister: return Invoke(EmitSetScalarRegister, ctx, inst);
        case IrOpcode::GetVectorRegister: return Invoke(EmitGetVectorRegister, ctx, inst);
        case IrOpcode::SetVectorRegister: return Invoke(EmitSetVectorRegister, ctx, inst);
        case IrOpcode::LoadVectorWindow: return Invoke(EmitLoadVectorWindow, ctx, inst);
        case IrOpcode::StoreVectorWindow: return Invoke(EmitStoreVectorWindow, ctx, inst);
        case IrOpcode::GetGotoVariable: return Invoke(EmitGetGotoVariable, ctx, inst);
        case IrOpcode::SetGotoVariable: return Invoke(EmitSetGotoVariable, ctx, inst);
        case IrOpcode::GetScc: return Invoke(EmitGetScc, ctx, inst);
        case IrOpcode::SetScc: return Invoke(EmitSetScc, ctx, inst);
        case IrOpcode::GetExec: return Invoke(EmitGetExec, ctx, inst);
        case IrOpcode::SetExec: return Invoke(EmitSetExec, ctx, inst);
        case IrOpcode::GetExecLo: return Invoke(EmitGetExecLo, ctx, inst);
        case IrOpcode::SetExecLo: return Invoke(EmitSetExecLo, ctx, inst);
        case IrOpcode::GetExecHi: return Invoke(EmitGetExecHi, ctx, inst);
        case IrOpcode::SetExecHi: return Invoke(EmitSetExecHi, ctx, inst);
        case IrOpcode::GetVcc: return Invoke(EmitGetVcc, ctx, inst);
        case IrOpcode::SetVcc: return Invoke(EmitSetVcc, ctx, inst);
        case IrOpcode::GetVccLo: return Invoke(EmitGetVccLo, ctx, inst);
        case IrOpcode::SetVccLo: return Invoke(EmitSetVccLo, ctx, inst);
        case IrOpcode::GetVccHi: return Invoke(EmitGetVccHi, ctx, inst);
        case IrOpcode::SetVccHi: return Invoke(EmitSetVccHi, ctx, inst);
        case IrOpcode::GetM0: return Invoke(EmitGetM0, ctx, inst);
        case IrOpcode::SetM0: return Invoke(EmitSetM0, ctx, inst);
        case IrOpcode::UndefU1: return Invoke(EmitUndefU1, ctx, inst);
        case IrOpcode::UndefU8: return Invoke(EmitUndefU8, ctx, inst);
        case IrOpcode::UndefU16: return Invoke(EmitUndefU16, ctx, inst);
        case IrOpcode::UndefU32: return Invoke(EmitUndefU32, ctx, inst);
        case IrOpcode::UndefU64: return Invoke(EmitUndefU64, ctx, inst);
        case IrOpcode::BitCastU16F16: return Invoke(EmitBitCastU16F16, ctx, inst);
        case IrOpcode::BitCastF16U16: return Invoke(EmitBitCastF16U16, ctx, inst);
        case IrOpcode::BitCastU32F32: return Invoke(EmitBitCastU32F32, ctx, inst);
        case IrOpcode::BitCastF32U32: return Invoke(EmitBitCastF32U32, ctx, inst);
        case IrOpcode::ConvertU16U32: return Invoke(EmitConvertU16U32, ctx, inst);
        case IrOpcode::ConvertU32U16: return Invoke(EmitConvertU32U16, ctx, inst);
        case IrOpcode::ConvertU8U32: return Invoke(EmitConvertU8U32, ctx, inst);
        case IrOpcode::ConvertU32U8: return Invoke(EmitConvertU32U8, ctx, inst);
        case IrOpcode::ConvertF32F16: return Invoke(EmitConvertF32F16, ctx, inst);
        case IrOpcode::ConvertF16F32: return InvokeOutlined(EmitConvertF16F32, ctx, inst);
        case IrOpcode::ConvertS32F32: return Invoke(EmitConvertS32F32, ctx, inst);
        case IrOpcode::ConvertU32F32: return Invoke(EmitConvertU32F32, ctx, inst);
        case IrOpcode::ConvertF32S32: return Invoke(EmitConvertF32S32, ctx, inst);
        case IrOpcode::ConvertF32U32: return Invoke(EmitConvertF32U32, ctx, inst);
        case IrOpcode::CompositeConstructU64: return Invoke(EmitCompositeConstructU64, ctx, inst);
        case IrOpcode::CompositeConstructU32x2: return Invoke(EmitCompositeConstructU32x2, ctx, inst);
        case IrOpcode::CompositeConstructU32x3: return Invoke(EmitCompositeConstructU32x3, ctx, inst);
        case IrOpcode::CompositeConstructF32x2: return Invoke(EmitCompositeConstructF32x2, ctx, inst);
        case IrOpcode::CompositeConstructU32x4: return Invoke(EmitCompositeConstructU32x4, ctx, inst);
        case IrOpcode::CompositeExtractU64: return Invoke(EmitCompositeExtractU64, ctx, inst);
        case IrOpcode::CompositeExtractU32x2: return Invoke(EmitCompositeExtractU32x2, ctx, inst);
        case IrOpcode::CompositeExtractU32x3: return Invoke(EmitCompositeExtractU32x3, ctx, inst);
        case IrOpcode::CompositeExtractU32x4: return Invoke(EmitCompositeExtractU32x4, ctx, inst);
        case IrOpcode::PackHalf2x16: return InvokeOutlined(EmitPackHalf2x16, ctx, inst);
        case IrOpcode::PackFloat2x16Rtz: return Invoke(EmitPackFloat2x16Rtz, ctx, inst);
        case IrOpcode::FPAdd16x2: return Invoke(EmitFPAdd16x2, ctx, inst);
        case IrOpcode::FPMul16x2: return Invoke(EmitFPMul16x2, ctx, inst);
        case IrOpcode::FPAbs32: return Invoke(EmitFPAbs32, ctx, inst);
        case IrOpcode::FPNeg32: return Invoke(EmitFPNeg32, ctx, inst);
        case IrOpcode::FPSaturate32: return InvokeOutlined(EmitFPSaturate32, ctx, inst);
        case IrOpcode::BitFieldInsert: return Invoke(EmitBitFieldInsert, ctx, inst);
        case IrOpcode::BitFieldUExtract: return Invoke(EmitBitFieldUExtract, ctx, inst);
        case IrOpcode::BitFieldSExtract: return Invoke(EmitBitFieldSExtract, ctx, inst);
        case IrOpcode::DppMoveU32: return Invoke(EmitDppMoveU32, ctx, inst);
        case IrOpcode::DppUpdateU32: return Invoke(EmitDppUpdateU32, ctx, inst);
        case IrOpcode::WqmU64: return Invoke(EmitWqmU64, ctx, inst);
        case IrOpcode::SelectU1: return Invoke(EmitSelectU1, ctx, inst);
        case IrOpcode::SelectF32: return Invoke(EmitSelectF32, ctx, inst);
        case IrOpcode::IAdd64: return Invoke(EmitIAdd64, ctx, inst);
        case IrOpcode::IAddCarry32: return Invoke(EmitIAddCarry32, ctx, inst);
        case IrOpcode::ISub64: return Invoke(EmitISub64, ctx, inst);
        case IrOpcode::IMul64: return Invoke(EmitIMul64, ctx, inst);
        case IrOpcode::UDiv32: return Invoke(EmitUDiv32, ctx, inst);
        case IrOpcode::SMulHi: return Invoke(EmitSMulHi, ctx, inst);
        case IrOpcode::UMulHi: return Invoke(EmitUMulHi, ctx, inst);
        case IrOpcode::IAbs32: return Invoke(EmitIAbs32, ctx, inst);
        case IrOpcode::ShiftLeftLogical32: return Invoke(EmitShiftLeftLogical32, ctx, inst);
        case IrOpcode::ShiftLeftLogical64: return Invoke(EmitShiftLeftLogical64, ctx, inst);
        case IrOpcode::ShiftRightLogical32: return Invoke(EmitShiftRightLogical32, ctx, inst);
        case IrOpcode::ShiftRightLogical64: return Invoke(EmitShiftRightLogical64, ctx, inst);
        case IrOpcode::ShiftRightArithmetic32: return Invoke(EmitShiftRightArithmetic32, ctx, inst);
        case IrOpcode::ShiftRightArithmetic64: return Invoke(EmitShiftRightArithmetic64, ctx, inst);
        case IrOpcode::BitwiseAnd32: return Invoke(EmitBitwiseAnd32, ctx, inst);
        case IrOpcode::BitwiseAnd64: return Invoke(EmitBitwiseAnd64, ctx, inst);
        case IrOpcode::BitwiseOr32: return Invoke(EmitBitwiseOr32, ctx, inst);
        case IrOpcode::BitwiseXor32: return Invoke(EmitBitwiseXor32, ctx, inst);
        case IrOpcode::BitwiseNot32: return Invoke(EmitBitwiseNot32, ctx, inst);
        case IrOpcode::BitReverse32: return Invoke(EmitBitReverse32, ctx, inst);
        case IrOpcode::BitCount32: return Invoke(EmitBitCount32, ctx, inst);
        case IrOpcode::BitCount64: return Invoke(EmitBitCount64, ctx, inst);
        case IrOpcode::FindUMsb32: return Invoke(EmitFindUMsb32, ctx, inst);
        case IrOpcode::FindUMsb64: return Invoke(EmitFindUMsb64, ctx, inst);
        case IrOpcode::FindILsb32: return Invoke(EmitFindILsb32, ctx, inst);
        case IrOpcode::SMin32: return Invoke(EmitSMin32, ctx, inst);
        case IrOpcode::UMin32: return Invoke(EmitUMin32, ctx, inst);
        case IrOpcode::SMax32: return Invoke(EmitSMax32, ctx, inst);
        case IrOpcode::UMax32: return Invoke(EmitUMax32, ctx, inst);
        case IrOpcode::SMinTri32: return Invoke(EmitSMinTri32, ctx, inst);
        case IrOpcode::UMinTri32: return Invoke(EmitUMinTri32, ctx, inst);
        case IrOpcode::SMaxTri32: return Invoke(EmitSMaxTri32, ctx, inst);
        case IrOpcode::UMaxTri32: return Invoke(EmitUMaxTri32, ctx, inst);
        case IrOpcode::SMedTri32: return Invoke(EmitSMedTri32, ctx, inst);
        case IrOpcode::UMedTri32: return Invoke(EmitUMedTri32, ctx, inst);
        case IrOpcode::SLessThan32: return Invoke(EmitSLessThan32, ctx, inst);
        case IrOpcode::SLessThan64: return Invoke(EmitSLessThan64, ctx, inst);
        case IrOpcode::ULessThan32: return Invoke(EmitULessThan32, ctx, inst);
        case IrOpcode::ULessThan64: return Invoke(EmitULessThan64, ctx, inst);
        case IrOpcode::IEqual32: return Invoke(EmitIEqual32, ctx, inst);
        case IrOpcode::IEqual64: return Invoke(EmitIEqual64, ctx, inst);
        case IrOpcode::SLessThanEqual32: return Invoke(EmitSLessThanEqual32, ctx, inst);
        case IrOpcode::ULessThanEqual32: return Invoke(EmitULessThanEqual32, ctx, inst);
        case IrOpcode::SGreaterThan32: return Invoke(EmitSGreaterThan32, ctx, inst);
        case IrOpcode::UGreaterThan32: return Invoke(EmitUGreaterThan32, ctx, inst);
        case IrOpcode::UGreaterThan64: return Invoke(EmitUGreaterThan64, ctx, inst);
        case IrOpcode::INotEqual32: return Invoke(EmitINotEqual32, ctx, inst);
        case IrOpcode::INotEqual64: return Invoke(EmitINotEqual64, ctx, inst);
        case IrOpcode::SGreaterThanEqual32: return Invoke(EmitSGreaterThanEqual32, ctx, inst);
        case IrOpcode::UGreaterThanEqual32: return Invoke(EmitUGreaterThanEqual32, ctx, inst);
        case IrOpcode::LogicalOr: return Invoke(EmitLogicalOr, ctx, inst);
        case IrOpcode::LogicalAnd: return Invoke(EmitLogicalAnd, ctx, inst);
        case IrOpcode::LogicalXor: return Invoke(EmitLogicalXor, ctx, inst);
        case IrOpcode::LogicalNot: return Invoke(EmitLogicalNot, ctx, inst);
        case IrOpcode::FPOrdEqual32: return Invoke(EmitFPOrdEqual32, ctx, inst);
        case IrOpcode::FPUnordEqual32: return Invoke(EmitFPUnordEqual32, ctx, inst);
        case IrOpcode::FPOrdNotEqual32: return Invoke(EmitFPOrdNotEqual32, ctx, inst);
        case IrOpcode::FPUnordNotEqual32: return Invoke(EmitFPUnordNotEqual32, ctx, inst);
        case IrOpcode::FPOrdLessThan32: return InvokeOutlined(EmitFPOrdLessThan32, ctx, inst);
        case IrOpcode::FPUnordLessThan32: return Invoke(EmitFPUnordLessThan32, ctx, inst);
        case IrOpcode::FPOrdGreaterThan32: return Invoke(EmitFPOrdGreaterThan32, ctx, inst);
        case IrOpcode::FPUnordGreaterThan32: return Invoke(EmitFPUnordGreaterThan32, ctx, inst);
        case IrOpcode::FPOrdLessThanEqual32: return Invoke(EmitFPOrdLessThanEqual32, ctx, inst);
        case IrOpcode::FPUnordLessThanEqual32: return Invoke(EmitFPUnordLessThanEqual32, ctx, inst);
        case IrOpcode::FPOrdGreaterThanEqual32: return Invoke(EmitFPOrdGreaterThanEqual32, ctx, inst);
        case IrOpcode::FPUnordGreaterThanEqual32: return Invoke(EmitFPUnordGreaterThanEqual32, ctx, inst);
        case IrOpcode::FPIsNan32: return Invoke(EmitFPIsNan32, ctx, inst);
        case IrOpcode::FPCmpClass32: return InvokeOutlined(EmitFPCmpClass32, ctx, inst);
        case IrOpcode::FPAdd32: return Invoke(EmitFPAdd32, ctx, inst);
        case IrOpcode::FPSub32: return Invoke(EmitFPSub32, ctx, inst);
        case IrOpcode::FPFma32: return Invoke(EmitFPFma32, ctx, inst);
        case IrOpcode::FPMad32: return Invoke(EmitFPMad32, ctx, inst);
        case IrOpcode::FPMul32: return Invoke(EmitFPMul32, ctx, inst);
        case IrOpcode::FPMin32: return InvokeOutlined(EmitFPMin32, ctx, inst);
        case IrOpcode::FPMax32: return InvokeOutlined(EmitFPMax32, ctx, inst);
        case IrOpcode::FPMinTri32: return Invoke(EmitFPMinTri32, ctx, inst);
        case IrOpcode::FPMaxTri32: return Invoke(EmitFPMaxTri32, ctx, inst);
        case IrOpcode::FPMedTri32: return InvokeOutlined(EmitFPMedTri32, ctx, inst);
        case IrOpcode::FPRecip32: return Invoke(EmitFPRecip32, ctx, inst);
        case IrOpcode::FPRecipIFlag32: return Invoke(EmitFPRecipIFlag32, ctx, inst);
        case IrOpcode::FPRecipSqrt32: return Invoke(EmitFPRecipSqrt32, ctx, inst);
        case IrOpcode::FPSqrt: return Invoke(EmitFPSqrt, ctx, inst);
        case IrOpcode::FPSin: return InvokeOutlined(EmitFPSin, ctx, inst);
        case IrOpcode::FPCos: return InvokeOutlined(EmitFPCos, ctx, inst);
        case IrOpcode::FPExp2: return Invoke(EmitFPExp2, ctx, inst);
        case IrOpcode::FPLog2: return InvokeOutlined(EmitFPLog2, ctx, inst);
        case IrOpcode::FPRoundEven32: return Invoke(EmitFPRoundEven32, ctx, inst);
        case IrOpcode::FPFloor32: return Invoke(EmitFPFloor32, ctx, inst);
        case IrOpcode::FPCeil32: return Invoke(EmitFPCeil32, ctx, inst);
        case IrOpcode::FPTrunc32: return Invoke(EmitFPTrunc32, ctx, inst);
        case IrOpcode::FPFract32: return InvokeOutlined(EmitFPFract32, ctx, inst);
        case IrOpcode::FPAdd64: return Invoke(EmitFPAdd64, ctx, inst);
        case IrOpcode::FPMul64: return Invoke(EmitFPMul64, ctx, inst);
        case IrOpcode::FPFma64: return Invoke(EmitFPFma64, ctx, inst);
        case IrOpcode::FPFmaScale64: return Invoke(EmitFPFmaScale64, ctx, inst);
        case IrOpcode::FPMin64: return Invoke(EmitFPMin64, ctx, inst);
        case IrOpcode::FPMax64: return Invoke(EmitFPMax64, ctx, inst);
        case IrOpcode::FPSaturate64: return Invoke(EmitFPSaturate64, ctx, inst);
        case IrOpcode::FPLdexp64: return Invoke(EmitFPLdexp64, ctx, inst);
        case IrOpcode::FPRoundEven64: return Invoke(EmitFPRoundEven64, ctx, inst);
        case IrOpcode::FPFloor64: return Invoke(EmitFPFloor64, ctx, inst);
        case IrOpcode::FPCeil64: return Invoke(EmitFPCeil64, ctx, inst);
        case IrOpcode::FPTrunc64: return Invoke(EmitFPTrunc64, ctx, inst);
        case IrOpcode::FPFract64: return Invoke(EmitFPFract64, ctx, inst);
        case IrOpcode::FPFrexpMant64: return Invoke(EmitFPFrexpMant64, ctx, inst);
        case IrOpcode::FPFrexpExp64: return Invoke(EmitFPFrexpExp64, ctx, inst);
        case IrOpcode::FPRcp64: return Invoke(EmitFPRcp64, ctx, inst);
        case IrOpcode::FPRsq64: return Invoke(EmitFPRsq64, ctx, inst);
        case IrOpcode::FPSqrt64: return Invoke(EmitFPSqrt64, ctx, inst);
        case IrOpcode::FPTrigPreop64: return Invoke(EmitFPTrigPreop64, ctx, inst);
        case IrOpcode::FPDot2F32F16: return Invoke(EmitFPDot2F32F16, ctx, inst);
        case IrOpcode::FPInterpolateF32: return Invoke(EmitFPInterpolateF32, ctx, inst);
        case IrOpcode::FPInterpolateF16: return Invoke(EmitFPInterpolateF16, ctx, inst);
        case IrOpcode::ConvertF32F64: return Invoke(EmitConvertF32F64, ctx, inst);
        case IrOpcode::ConvertF64F32: return Invoke(EmitConvertF64F32, ctx, inst);
        case IrOpcode::ConvertF64S32: return Invoke(EmitConvertF64S32, ctx, inst);
        case IrOpcode::ConvertF64U32: return Invoke(EmitConvertF64U32, ctx, inst);
        case IrOpcode::ConvertS32F64: return Invoke(EmitConvertS32F64, ctx, inst);
        case IrOpcode::ConvertU32F64: return Invoke(EmitConvertU32F64, ctx, inst);
        case IrOpcode::LaneId: return Invoke(EmitLaneId, ctx, inst);
        case IrOpcode::WriteLane: return Invoke(EmitWriteLane, ctx, inst);
        case IrOpcode::Permlane16U32: return Invoke(EmitPermlane16U32, ctx, inst);
        case IrOpcode::BpermuteU32: return Invoke(EmitBpermuteU32, ctx, inst);
        case IrOpcode::PermuteU32: return Invoke(EmitPermuteU32, ctx, inst);
        case IrOpcode::GetSrtResource: return Invoke(EmitGetSrtResource, ctx, inst);
        case IrOpcode::GetBufferResource: return Invoke(EmitGetBufferResource, ctx, inst);
        case IrOpcode::GetAddressResource: return Invoke(EmitGetAddressResource, ctx, inst);
        case IrOpcode::GetScratchResource: return Invoke(EmitGetScratchResource, ctx, inst);
        case IrOpcode::GetImageResource: return Invoke(EmitGetImageResource, ctx, inst);
        case IrOpcode::GetSamplerResource: return Invoke(EmitGetSamplerResource, ctx, inst);
        case IrOpcode::MakeImageAddress: return Invoke(EmitMakeImageAddress, ctx, inst);
        case IrOpcode::ReadConst: return Invoke(EmitReadConst, ctx, inst);
        case IrOpcode::ReadConstBuffer: return Invoke(EmitReadConstBuffer, ctx, inst);
        case IrOpcode::LoadAddressU8: return Invoke(EmitLoadAddressU8, ctx, inst);
        case IrOpcode::LoadAddressU16: return Invoke(EmitLoadAddressU16, ctx, inst);
        case IrOpcode::LoadAddressU32: return Invoke(EmitLoadAddressU32, ctx, inst);
        case IrOpcode::LoadAddressU32x2: return Invoke(EmitLoadAddressU32x2, ctx, inst);
        case IrOpcode::LoadAddressU32x3: return Invoke(EmitLoadAddressU32x3, ctx, inst);
        case IrOpcode::LoadAddressU32x4: return Invoke(EmitLoadAddressU32x4, ctx, inst);
        case IrOpcode::StoreAddressU8: return Invoke(EmitStoreAddressU8, ctx, inst);
        case IrOpcode::StoreAddressU16: return Invoke(EmitStoreAddressU16, ctx, inst);
        case IrOpcode::StoreAddressU32: return Invoke(EmitStoreAddressU32, ctx, inst);
        case IrOpcode::StoreAddressU32x2: return Invoke(EmitStoreAddressU32x2, ctx, inst);
        case IrOpcode::StoreAddressU32x3: return Invoke(EmitStoreAddressU32x3, ctx, inst);
        case IrOpcode::StoreAddressU32x4: return Invoke(EmitStoreAddressU32x4, ctx, inst);
        case IrOpcode::AddressAtomicSwap32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicCmpSwap32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicIAdd32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicISub32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicSMin32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicUMin32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicSMax32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicUMax32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicAnd32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicOr32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicXor32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicInc32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicDec32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicUSubSat32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicSwap64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicCmpSwap64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicIAdd64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicISub64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicSMin64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicUMin64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicSMax64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicUMax64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicAnd64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicOr64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicXor64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicFCmpSwap32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicFMin32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicFMax32: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicFCmpSwap64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicFMin64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicFMax64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicInc64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::AddressAtomicDec64: return Invoke(EmitAddressAtomic, ctx, inst);
        case IrOpcode::LoadBufferU8: return Invoke(EmitLoadBufferU8, ctx, inst);
        case IrOpcode::LoadBufferU16: return Invoke(EmitLoadBufferU16, ctx, inst);
        case IrOpcode::LoadBufferU32: return Invoke(EmitLoadBufferU32, ctx, inst);
        case IrOpcode::LoadBufferU32x2: return Invoke(EmitLoadBufferU32x2, ctx, inst);
        case IrOpcode::LoadBufferU32x3: return Invoke(EmitLoadBufferU32x3, ctx, inst);
        case IrOpcode::LoadBufferU32x4: return Invoke(EmitLoadBufferU32x4, ctx, inst);
        case IrOpcode::StoreBufferU8: return Invoke(EmitStoreBufferU8, ctx, inst);
        case IrOpcode::StoreBufferU16: return Invoke(EmitStoreBufferU16, ctx, inst);
        case IrOpcode::StoreBufferU32: return Invoke(EmitStoreBufferU32, ctx, inst);
        case IrOpcode::StoreBufferU32x2: return Invoke(EmitStoreBufferU32x2, ctx, inst);
        case IrOpcode::StoreBufferU32x3: return Invoke(EmitStoreBufferU32x3, ctx, inst);
        case IrOpcode::StoreBufferU32x4: return Invoke(EmitStoreBufferU32x4, ctx, inst);
        case IrOpcode::BufferAtomicSwap32: return Invoke(EmitBufferAtomicSwap32, ctx, inst);
        case IrOpcode::BufferAtomicCmpSwap32: return Invoke(EmitBufferAtomicCmpSwap32, ctx, inst);
        case IrOpcode::BufferAtomicSwap64: return Invoke(EmitBufferAtomicSwap64, ctx, inst);
        case IrOpcode::BufferAtomicIAdd32: return Invoke(EmitBufferAtomicIAdd32, ctx, inst);
        case IrOpcode::BufferAtomicISub32: return Invoke(EmitBufferAtomicISub32, ctx, inst);
        case IrOpcode::BufferAtomicSMin32: return Invoke(EmitBufferAtomicSMin32, ctx, inst);
        case IrOpcode::BufferAtomicUMin32: return Invoke(EmitBufferAtomicUMin32, ctx, inst);
        case IrOpcode::BufferAtomicSMax32: return Invoke(EmitBufferAtomicSMax32, ctx, inst);
        case IrOpcode::BufferAtomicUMax32: return Invoke(EmitBufferAtomicUMax32, ctx, inst);
        case IrOpcode::BufferAtomicAnd32: return Invoke(EmitBufferAtomicAnd32, ctx, inst);
        case IrOpcode::BufferAtomicOr32: return Invoke(EmitBufferAtomicOr32, ctx, inst);
        case IrOpcode::BufferAtomicOr64: return Invoke(EmitBufferAtomicOr64, ctx, inst);
        case IrOpcode::BufferAtomicInc32: return Invoke(EmitBufferAtomicInc32, ctx, inst);
        case IrOpcode::BufferAtomicDec32: return Invoke(EmitBufferAtomicDec32, ctx, inst);
        case IrOpcode::BufferAtomicUSubSat32: return Invoke(EmitBufferAtomicUSubSat32, ctx, inst);
        case IrOpcode::BufferAtomicIAdd64: return Invoke(EmitBufferAtomicIAdd64, ctx, inst);
        case IrOpcode::BufferAtomicISub64: return Invoke(EmitBufferAtomicISub64, ctx, inst);
        case IrOpcode::BufferAtomicSMin64: return Invoke(EmitBufferAtomicSMin64, ctx, inst);
        case IrOpcode::BufferAtomicUMin64: return Invoke(EmitBufferAtomicUMin64, ctx, inst);
        case IrOpcode::BufferAtomicSMax64: return Invoke(EmitBufferAtomicSMax64, ctx, inst);
        case IrOpcode::BufferAtomicUMax64: return Invoke(EmitBufferAtomicUMax64, ctx, inst);
        case IrOpcode::BufferAtomicAnd64: return Invoke(EmitBufferAtomicAnd64, ctx, inst);
        case IrOpcode::BufferAtomicXor64: return Invoke(EmitBufferAtomicXor64, ctx, inst);
        case IrOpcode::BufferAtomicCmpSwap64: return Invoke(EmitBufferAtomicCmpSwap64, ctx, inst);
        case IrOpcode::BufferAtomicFCmpSwap32: return Invoke(EmitBufferAtomicFCmpSwap32, ctx, inst);
        case IrOpcode::BufferAtomicFCmpSwap64: return Invoke(EmitBufferAtomicFCmpSwap64, ctx, inst);
        case IrOpcode::BufferAtomicFMin64: return Invoke(EmitBufferAtomicFMin64, ctx, inst);
        case IrOpcode::BufferAtomicFMax64: return Invoke(EmitBufferAtomicFMax64, ctx, inst);
        case IrOpcode::BufferAtomicInc64: return Invoke(EmitBufferAtomicInc64, ctx, inst);
        case IrOpcode::BufferAtomicDec64: return Invoke(EmitBufferAtomicDec64, ctx, inst);
        case IrOpcode::BufferAtomicXor32: return Invoke(EmitBufferAtomicXor32, ctx, inst);
        case IrOpcode::BufferAtomicFMin32: return Invoke(EmitBufferAtomicFMin32, ctx, inst);
        case IrOpcode::BufferAtomicFMax32: return Invoke(EmitBufferAtomicFMax32, ctx, inst);
        case IrOpcode::LoadSharedU8: return Invoke(EmitLoadSharedU8, ctx, inst);
        case IrOpcode::LoadSharedU16: return Invoke(EmitLoadSharedU16, ctx, inst);
        case IrOpcode::LoadSharedU32: return Invoke(EmitLoadSharedU32, ctx, inst);
        case IrOpcode::LoadSharedU32x2: return Invoke(EmitLoadSharedU32x2, ctx, inst);
        case IrOpcode::LoadSharedU32x3: return Invoke(EmitLoadSharedU32x3, ctx, inst);
        case IrOpcode::LoadSharedU32x4: return Invoke(EmitLoadSharedU32x4, ctx, inst);
        case IrOpcode::WriteSharedU8: return Invoke(EmitWriteSharedU8, ctx, inst);
        case IrOpcode::WriteSharedU16: return Invoke(EmitWriteSharedU16, ctx, inst);
        case IrOpcode::WriteSharedU32: return Invoke(EmitWriteSharedU32, ctx, inst);
        case IrOpcode::WriteSharedU32x2: return Invoke(EmitWriteSharedU32x2, ctx, inst);
        case IrOpcode::WriteSharedU32x3: return Invoke(EmitWriteSharedU32x3, ctx, inst);
        case IrOpcode::WriteSharedU32x4: return Invoke(EmitWriteSharedU32x4, ctx, inst);
        case IrOpcode::SharedAtomicFMin32: return Invoke(EmitSharedAtomicFMin32, ctx, inst);
        case IrOpcode::SharedAtomicFMax32: return Invoke(EmitSharedAtomicFMax32, ctx, inst);
        case IrOpcode::SharedAtomicSwap32: return Invoke(EmitSharedAtomicSwap32, ctx, inst);
        case IrOpcode::SharedAtomicIAdd32: return Invoke(EmitSharedAtomicIAdd32, ctx, inst);
        case IrOpcode::SharedAtomicISub32: return Invoke(EmitSharedAtomicISub32, ctx, inst);
        case IrOpcode::SharedAtomicInc32: return Invoke(EmitSharedAtomicInc32, ctx, inst);
        case IrOpcode::SharedAtomicDec32: return Invoke(EmitSharedAtomicDec32, ctx, inst);
        case IrOpcode::SharedAtomicSMin32: return Invoke(EmitSharedAtomicSMin32, ctx, inst);
        case IrOpcode::SharedAtomicUMin32: return Invoke(EmitSharedAtomicUMin32, ctx, inst);
        case IrOpcode::SharedAtomicSMax32: return Invoke(EmitSharedAtomicSMax32, ctx, inst);
        case IrOpcode::SharedAtomicUMax32: return Invoke(EmitSharedAtomicUMax32, ctx, inst);
        case IrOpcode::SharedAtomicAnd32: return Invoke(EmitSharedAtomicAnd32, ctx, inst);
        case IrOpcode::SharedAtomicOr32: return Invoke(EmitSharedAtomicOr32, ctx, inst);
        case IrOpcode::SharedAtomicXor32: return Invoke(EmitSharedAtomicXor32, ctx, inst);
        case IrOpcode::SharedAtomicRsub32: return Invoke(EmitSharedAtomicRsub32, ctx, inst);
        case IrOpcode::SharedAtomicFAdd32: return Invoke(EmitSharedAtomicFAdd32, ctx, inst);
        case IrOpcode::SharedAtomicCmpst32: return Invoke(EmitSharedAtomicCmpst32, ctx, inst);
        case IrOpcode::SharedAtomicCmpstF32: return Invoke(EmitSharedAtomicCmpstF32, ctx, inst);
        case IrOpcode::SharedAtomicMskor32: return Invoke(EmitSharedAtomicMskor32, ctx, inst);
        case IrOpcode::SharedAtomicWrap32: return Invoke(EmitSharedAtomicWrap32, ctx, inst);
        case IrOpcode::SharedAtomicSwap64: return Invoke(EmitSharedAtomicSwap64, ctx, inst);
        case IrOpcode::SharedAtomicIAdd64: return Invoke(EmitSharedAtomicIAdd64, ctx, inst);
        case IrOpcode::SharedAtomicISub64: return Invoke(EmitSharedAtomicISub64, ctx, inst);
        case IrOpcode::SharedAtomicRsub64: return Invoke(EmitSharedAtomicRsub64, ctx, inst);
        case IrOpcode::SharedAtomicInc64: return Invoke(EmitSharedAtomicInc64, ctx, inst);
        case IrOpcode::SharedAtomicDec64: return Invoke(EmitSharedAtomicDec64, ctx, inst);
        case IrOpcode::SharedAtomicSMin64: return Invoke(EmitSharedAtomicSMin64, ctx, inst);
        case IrOpcode::SharedAtomicUMin64: return Invoke(EmitSharedAtomicUMin64, ctx, inst);
        case IrOpcode::SharedAtomicSMax64: return Invoke(EmitSharedAtomicSMax64, ctx, inst);
        case IrOpcode::SharedAtomicUMax64: return Invoke(EmitSharedAtomicUMax64, ctx, inst);
        case IrOpcode::SharedAtomicAnd64: return Invoke(EmitSharedAtomicAnd64, ctx, inst);
        case IrOpcode::SharedAtomicOr64: return Invoke(EmitSharedAtomicOr64, ctx, inst);
        case IrOpcode::SharedAtomicXor64: return Invoke(EmitSharedAtomicXor64, ctx, inst);
        case IrOpcode::SharedAtomicFMin64: return Invoke(EmitSharedAtomicFMin64, ctx, inst);
        case IrOpcode::SharedAtomicFMax64: return Invoke(EmitSharedAtomicFMax64, ctx, inst);
        case IrOpcode::SharedAtomicCmpst64: return Invoke(EmitSharedAtomicCmpst64, ctx, inst);
        case IrOpcode::SharedAtomicCmpstF64: return Invoke(EmitSharedAtomicCmpstF64, ctx, inst);
        case IrOpcode::SharedAtomicMskor64: return Invoke(EmitSharedAtomicMskor64, ctx, inst);
        case IrOpcode::DataAppend: return Invoke(EmitDataAppend, ctx, inst);
        case IrOpcode::DataConsume: return Invoke(EmitDataConsume, ctx, inst);
        case IrOpcode::DataOrderedCount: return Invoke(EmitDataOrderedCount, ctx, inst);
        case IrOpcode::SwizzleU32: return Invoke(EmitSwizzleU32, ctx, inst);
        case IrOpcode::ImageQueryDimensions: return Invoke(EmitImageQueryDimensions, ctx, inst);
        case IrOpcode::ImageQueryLod: return Invoke(EmitImageQueryLod, ctx, inst);
        case IrOpcode::ImageRead: return Invoke(EmitImageRead, ctx, inst);
        case IrOpcode::ImageBvhIntersectRay: return Invoke(EmitImageBvhIntersectRay, ctx, inst);
        case IrOpcode::ImageWrite: return Invoke(EmitImageWrite, ctx, inst);
        case IrOpcode::ImageSampleRaw: return Invoke(EmitImageSampleRaw, ctx, inst);
        case IrOpcode::ImageGatherRaw: return Invoke(EmitImageGatherRaw, ctx, inst);
        case IrOpcode::ImageAtomicSwap32: return Invoke(EmitImageAtomicSwap32, ctx, inst);
        case IrOpcode::ImageAtomicIAdd32: return Invoke(EmitImageAtomicIAdd32, ctx, inst);
        case IrOpcode::ImageAtomicUMin32: return Invoke(EmitImageAtomicUMin32, ctx, inst);
        case IrOpcode::ImageAtomicUMax32: return Invoke(EmitImageAtomicUMax32, ctx, inst);
        case IrOpcode::ImageAtomicAnd32: return Invoke(EmitImageAtomicAnd32, ctx, inst);
        case IrOpcode::ImageAtomicOr32: return Invoke(EmitImageAtomicOr32, ctx, inst);
        case IrOpcode::ImageAtomicXor32: return Invoke(EmitImageAtomicXor32, ctx, inst);
        case IrOpcode::ImageAtomicCmpSwap32: return Invoke(EmitImageAtomicCmpSwap32, ctx, inst);
        case IrOpcode::ImageAtomicISub32: return Invoke(EmitImageAtomicISub32, ctx, inst);
        case IrOpcode::ImageAtomicSMin32: return Invoke(EmitImageAtomicSMin32, ctx, inst);
        case IrOpcode::ImageAtomicSMax32: return Invoke(EmitImageAtomicSMax32, ctx, inst);
        case IrOpcode::ImageAtomicInc32: return Invoke(EmitImageAtomicInc32, ctx, inst);
        case IrOpcode::ImageAtomicDec32: return Invoke(EmitImageAtomicDec32, ctx, inst);
        case IrOpcode::ImageAtomicFCmpSwap32: return Invoke(EmitImageAtomicFCmpSwap32, ctx, inst);
        case IrOpcode::ImageAtomicFMin32: return Invoke(EmitImageAtomicFMin32, ctx, inst);
        case IrOpcode::ImageAtomicFMax32: return Invoke(EmitImageAtomicFMax32, ctx, inst);
        case IrOpcode::ImageAtomicSwap64: return Invoke(EmitImageAtomicSwap64, ctx, inst);
        case IrOpcode::ImageAtomicIAdd64: return Invoke(EmitImageAtomicIAdd64, ctx, inst);
        case IrOpcode::ImageAtomicISub64: return Invoke(EmitImageAtomicISub64, ctx, inst);
        case IrOpcode::ImageAtomicUMin64: return Invoke(EmitImageAtomicUMin64, ctx, inst);
        case IrOpcode::ImageAtomicUMax64: return Invoke(EmitImageAtomicUMax64, ctx, inst);
        case IrOpcode::ImageAtomicSMin64: return Invoke(EmitImageAtomicSMin64, ctx, inst);
        case IrOpcode::ImageAtomicSMax64: return Invoke(EmitImageAtomicSMax64, ctx, inst);
        case IrOpcode::ImageAtomicAnd64: return Invoke(EmitImageAtomicAnd64, ctx, inst);
        case IrOpcode::ImageAtomicOr64: return Invoke(EmitImageAtomicOr64, ctx, inst);
        case IrOpcode::ImageAtomicXor64: return Invoke(EmitImageAtomicXor64, ctx, inst);
        case IrOpcode::ImageAtomicCmpSwap64: return Invoke(EmitImageAtomicCmpSwap64, ctx, inst);
        case IrOpcode::GetAttribute: return Invoke(EmitGetAttribute, ctx, inst);
        case IrOpcode::GetInterpolationParameter: return Invoke(EmitGetInterpolationParameter, ctx, inst);
        case IrOpcode::GetInterpolationParameterF16: return Invoke(EmitGetInterpolationParameterF16, ctx, inst);
        case IrOpcode::SetAttribute: return Invoke(EmitSetAttribute, ctx, inst);
        case IrOpcode::ControlNop: return Invoke(EmitControlNop, ctx, inst);
        case IrOpcode::Waitcnt: return Invoke(EmitWaitcnt, ctx, inst);
        case IrOpcode::Sendmsg: return Invoke(EmitSendmsg, ctx, inst);
        case IrOpcode::TtraceData: return Invoke(EmitTtraceData, ctx, inst);
        case IrOpcode::InstPrefetch: return Invoke(EmitInstPrefetch, ctx, inst);
        case IrOpcode::SelectU32: return Invoke(EmitSelectU32, ctx, inst);
        default: ctx.Fail(inst, "has no direct SPIR-V emitter");
    }
}

namespace {

enum FloatIdiom : std::uint8_t {
    NoIdiom,
    FlushIdiom,
    IsNanIdiom,
    FoldedIntoIdiom,
};

bool immediateU32(const IrValue* value, std::uint32_t bits) {
    return value != nullptr && value->HasImmediate() && value->Type() == IrType::U32 && value->ImmediateU32() == bits;
}

const IrValue* maskedBy(const IrValue* value, std::uint32_t mask) {
    if (value == nullptr || value->Opcode() != IrOpcode::BitwiseAnd32 || value->ArgumentCount() != 2u || !immediateU32(value->Argument(1), mask)) return nullptr;
    return value->Argument(0);
}

const IrValue* flushedSource(const IrValue& value) {
    if (value.Opcode() != IrOpcode::SelectU32 || value.ArgumentCount() != 3u) return nullptr;
    const IrValue* source = value.Argument(2);
    const IrValue* denormal = value.Argument(0);
    if (source == nullptr || denormal == nullptr || denormal->Opcode() != IrOpcode::IEqual32 || denormal->ArgumentCount() != 2u || !immediateU32(denormal->Argument(1), 0u)) return nullptr;
    if (maskedBy(denormal->Argument(0), 0x7f800000u) != source || maskedBy(value.Argument(1), 0x80000000u) != source) return nullptr;
    return source;
}

const IrValue* nanTestSource(const IrValue& value) {
    if (value.Opcode() != IrOpcode::UGreaterThan32 || value.ArgumentCount() != 2u || !immediateU32(value.Argument(1), 0x7f800000u)) return nullptr;
    return maskedBy(value.Argument(0), 0x7fffffffu);
}

std::uint8_t idiomOf(const std::vector<std::uint8_t>& idioms, const IrValue* value) {
    return value->Id() < idioms.size() ? idioms[value->Id()] : static_cast<std::uint8_t>(NoIdiom);
}

bool foldable(const std::vector<std::uint8_t>& idioms, const std::vector<std::uint8_t>& pinned, const IrValue* value, std::uint8_t user) {
    if (value->Parent() == nullptr || value->Id() >= idioms.size() || pinned[value->Id()] != 0u || idioms[value->Id()] != NoIdiom || value->Uses().empty()) return false;
    return std::ranges::all_of(value->Uses(), [&](const IrValue* consumer) { return idiomOf(idioms, consumer) == user; });
}

bool EmitFloatIdiom(SpirvValueEmitContext& ctx, const IrValue& inst) {
    auto& state = ctx.state;
    switch (idiomOf(state.floatIdioms, &inst)) {
    case FlushIdiom: {
        const auto result = state.module.AllocateId();
        state.module.AddFunction(spv::OpFunctionCall, TypeU32(state), result, state.flushF32DenormalFunction, ctx.Def(inst.Argument(2)));
        ctx.Define(inst, result);
        return true;
    }
    case IsNanIdiom: {
        const auto result = state.module.AllocateId();
        state.module.AddFunction(spv::OpFunctionCall, TypeBool(state), result, state.isNanF32Function, ctx.Def(nanTestSource(inst)));
        ctx.Define(inst, result);
        return true;
    }
    case FoldedIntoIdiom:
        return true;
    default:
        return false;
    }
}

}

void EmitStructuredInstruction(SpirvValueEmitContext& ctx, StructuredFunctionState& functionState, const IrValue& inst) {
    if (inst.Opcode() == IrOpcode::Phi) {
        const auto type = TypeId(ctx.state, inst.Type());
        if (type == 0 || inst.ArgumentCount() == 0) {
            ctx.Fail(inst, "has no native SPIR-V representation");
        }
        for (std::size_t index = 0; index < inst.ArgumentCount(); index++) {
            const IrBlock* predecessor = inst.PhiBlock(index);
            if (predecessor == nullptr || !ctx.state.labels.contains(predecessor)) {
                ctx.Fail(inst, "has a predecessor outside the structured function");
            }
        }
        functionState.deferredPhis.push_back({ctx.state.module.AddDeferredPhi(type, ctx.Result(inst), inst.ArgumentCount()), &inst, ctx.half});
        return;
    }
    if (EmitFloatIdiom(ctx, inst)) return;
    EmitDirectInstruction(ctx, inst);
}

void EmitStructuredBlock(SpirvValueEmitContext& ctx, StructuredFunctionState& functionState, const IrBlock* block) {
    auto& state = ctx.state;
    state.currentBlock = block;
    EmitLabel(state, ctx.Label(block));
    bool emittedNonPhi = false;
    // Wave LDS ordering (see WaveLdsScope): a barrier separates an LDS write from the next LDS access
    // and a read from the next write. The block may be entered right after a write.
    bool ldsWritten = true;
    bool ldsRead = false;
    bool firstNonPhi = true;
    for (const IrValue* inst : block->Instructions()) {
        if (inst->Opcode() == IrOpcode::Phi) {
            if (emittedNonPhi) {
                ctx.Fail(*inst, "appears after a non-Phi instruction");
            }
        } else {
            if (!emittedNonPhi && state.guestLineFile != 0u) {
                if (const BlockInfo* info = BlockInfoFor(state.program, block); info != nullptr) state.module.AddFunction(spv::OpLine, state.guestLineFile, info->startPc, 0u);
            }
            emittedNonPhi = true;
            if (state.guestLineFile != 0u && AddressOpcodeInfoOf(inst->Opcode()).access != AddressAccess::None) state.module.AddFunction(spv::OpLine, state.guestLineFile, inst->Flags<MemoryFlags>().pc, 0u);
        }
        if (state.waveLdsScope != 0) {
            const auto access = SharedAccessOf(inst->Opcode());
            if (inst->Opcode() == IrOpcode::Barrier) {
                ldsWritten = false;
                ldsRead = false;
            } else if (access != SharedAccess::None) {
                const bool writes = access != SharedAccess::Read;
                if (ldsWritten || (writes && ldsRead)) {
                    state.module.AddFunction(spv::OpControlBarrier, ConstantU32(state, state.waveLdsScope), ConstantU32(state, spv::ScopeWorkgroup), ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask | LdsMemorySemantics(state)));
                    ldsWritten = false;
                    ldsRead = false;
                }
                ldsWritten |= writes;
                ldsRead |= access != SharedAccess::Write;
            }
        }
        const bool shared = state.laneCount == 2u && inst->Type() != IrType::Void && !IrOpcodeHasSideEffects(inst->Opcode()) && state.sharedLaneValues.contains(inst);
        for (std::uint32_t half = 0; half < state.laneCount; half++) {
            if (half != 0 && ctx.otherHalf == nullptr) {
                ctx.Fail(*inst, "requires a second lane context");
            }
            if (half != 0 && shared) {
                if (const auto found = ctx.definitions.find(inst); found != ctx.definitions.end()) {
                    ctx.otherHalf->Define(*inst, found->second);
                    continue;
                }
            }
            SpirvValueEmitContext& lane = half == 0 ? ctx : *ctx.otherHalf;
            state.laneHalf = half;
            if (half == 0 || (inst->Opcode() != IrOpcode::Barrier && inst->Opcode() != IrOpcode::MeshAllocate)) {
                if (inst->Opcode() != IrOpcode::Phi) EmitBdaRootBefore(lane, *inst, firstNonPhi);
                EmitStructuredInstruction(lane, functionState, *inst);
                if (inst->Opcode() != IrOpcode::Phi) EmitBdaRootAfter(lane, *inst);
            }
        }
        state.laneHalf = 0;
        if (inst->Opcode() != IrOpcode::Phi) firstNonPhi = false;
    }
}

void PatchStructuredPhis(SpirvValueEmitContext& ctx, StructuredFunctionState& functionState) {
    for (const SpirvDeferredPhiPatch& deferred : functionState.deferredPhis) {
        SpirvValueEmitContext& lane = deferred.half == 0 ? ctx : *ctx.otherHalf;
        for (std::size_t index = 0; index < deferred.instruction->ArgumentCount(); index++) {
            const IrBlock* predecessor = deferred.instruction->PhiBlock(index);
            const auto found = functionState.blockExitLabels.find(predecessor);
            if (found == functionState.blockExitLabels.end()) {
                ctx.Fail(*deferred.instruction, "has a predecessor that was not emitted");
            }
            ctx.state.module.PatchDeferredPhi(deferred.phi, index, lane.Def(deferred.instruction->Argument(index)), found->second);
        }
    }
}

bool CommunicatesThroughMemory(IrOpcode opcode) {
    const auto buffer = BufferAccessOf(opcode);
    const auto address = AddressOpcodeInfoOf(opcode).access;
    const auto image = ImageOpcodeInfoOf(opcode).access;
    return SharedAccessOf(opcode) != SharedAccess::None || buffer == BufferAccess::Write || buffer == BufferAccess::Atomic || address == AddressAccess::Write || address == AddressAccess::Atomic || image == ImageAccess::Write || image == ImageAccess::Atomic;
}

}

void DefineFloatIdiomFunctions(SpirvEmitterState& state) {
    if (!state.sharedFloatFunctions) return;
    const auto& program = state.program;
    std::uint32_t count = 0;
    for (const IrBlock* block : program.BlockOrder()) {
        for (const IrValue* inst : block->Instructions()) count = std::max(count, inst->Id() + 1u);
    }
    auto& idioms = state.floatIdioms;
    idioms.assign(count, NoIdiom);
    std::vector<std::uint8_t> pinned(count, 0u);
    const auto pin = [&](const IrValue* value) {
        if (value != nullptr && value->Id() < count) pinned[value->Id()] = 1u;
    };
    for (const auto& info : program.Metadata().blockInfo) {
        pin(info.condition);
        pin(info.indirectTarget);
    }
    for (const IrValue* value : program.Metadata().dynamicReads) pin(value);
    bool flush = false;
    bool nan = false;
    for (const IrBlock* block : program.BlockOrder()) {
        for (const IrValue* inst : block->Instructions()) {
            if (pinned[inst->Id()] != 0u) continue;
            if (flushedSource(*inst) != nullptr) {
                idioms[inst->Id()] = FlushIdiom;
                flush = true;
            } else if (nanTestSource(*inst) != nullptr) {
                idioms[inst->Id()] = IsNanIdiom;
                nan = true;
            }
        }
    }
    if (!flush && !nan) return;
    std::vector<const IrValue*> denormalTests;
    for (const IrBlock* block : program.BlockOrder()) {
        for (const IrValue* inst : block->Instructions()) {
            const auto idiom = idioms[inst->Id()];
            if (idiom == FlushIdiom) {
                if (foldable(idioms, pinned, inst->Argument(0), FlushIdiom)) denormalTests.push_back(inst->Argument(0));
                if (foldable(idioms, pinned, inst->Argument(1), FlushIdiom)) idioms[inst->Argument(1)->Id()] = FoldedIntoIdiom;
            } else if (idiom == IsNanIdiom && foldable(idioms, pinned, inst->Argument(0), IsNanIdiom)) {
                idioms[inst->Argument(0)->Id()] = FoldedIntoIdiom;
            }
        }
    }
    for (const IrValue* test : denormalTests) {
        if (idioms[test->Id()] != NoIdiom) continue;
        idioms[test->Id()] = FoldedIntoIdiom;
    }
    for (const IrValue* test : denormalTests) {
        const IrValue* exponent = test->Argument(0);
        if (exponent->Parent() == nullptr || exponent->Id() >= count || pinned[exponent->Id()] != 0u || idioms[exponent->Id()] != NoIdiom || exponent->Uses().empty()) continue;
        if (std::ranges::all_of(exponent->Uses(), [&](const IrValue* consumer) { return consumer->Opcode() == IrOpcode::IEqual32 && idiomOf(idioms, consumer) == FoldedIntoIdiom; })) idioms[exponent->Id()] = FoldedIntoIdiom;
    }
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    if (flush) {
        state.flushF32DenormalFunction = state.module.AllocateId();
        state.module.AddName(state.flushF32DenormalFunction, "flush_f32_denormal");
        state.module.AddFunction(spv::OpFunction, u32, state.flushF32DenormalFunction, spv::FunctionControlInlineMask, state.module.Type(spv::OpTypeFunction, u32, u32));
        const auto bits = state.module.AllocateId();
        state.module.AddFunction(spv::OpFunctionParameter, u32, bits);
        EmitLabel(state, state.module.AllocateId());
        const auto exponent = EmitBitwiseAnd32(state, bits, ConstantU32(state, 0x7f800000u));
        const auto denormal = EmitIEqual32(state, exponent, ConstantU32(state, 0u));
        const auto sign = EmitBitwiseAnd32(state, bits, ConstantU32(state, 0x80000000u));
        state.module.AddFunction(spv::OpReturnValue, EmitSelectU32(state, denormal, sign, bits));
        state.module.AddFunction(spv::OpFunctionEnd);
    }
    if (nan) {
        state.isNanF32Function = state.module.AllocateId();
        state.module.AddName(state.isNanF32Function, "is_nan_f32_bits");
        state.module.AddFunction(spv::OpFunction, boolean, state.isNanF32Function, spv::FunctionControlInlineMask, state.module.Type(spv::OpTypeFunction, boolean, u32));
        const auto bits = state.module.AllocateId();
        state.module.AddFunction(spv::OpFunctionParameter, u32, bits);
        EmitLabel(state, state.module.AllocateId());
        const auto magnitude = EmitBitwiseAnd32(state, bits, ConstantU32(state, 0x7fffffffu));
        state.module.AddFunction(spv::OpReturnValue, EmitUGreaterThan32(state, magnitude, ConstantU32(state, 0x7f800000u)));
        state.module.AddFunction(spv::OpFunctionEnd);
    }
}

void EmitControlFlow(SpirvModule& module, const IrProgram& program) {
    throw std::runtime_error("EmitControlFlow not implemented");
}

void EmitControlFlow(SpirvValueEmitContext& context, StructuredFunctionState& functionState, const IrProgram& program) {
    auto& state = context.state;
    const std::vector<IrBlock*>& blocks = program.BlockOrder();
    if (blocks.empty() || blocks.front() == nullptr) {
        context.Fail("structured control flow requires at least one block");
    }
    if (state.loopGuardLimit != 0) {
        const auto pointer = TypePointer(state, spv::StorageClassPrivate, TypeU32(state));
        if (state.loopGuardVisits == 0) state.loopGuardVisits = state.module.DefineGlobalVariable(pointer, spv::StorageClassPrivate);
        if (state.loopGuardPc == 0) state.loopGuardPc = state.module.DefineGlobalVariable(pointer, spv::StorageClassPrivate);
        state.module.AddFunction(spv::OpStore, state.loopGuardVisits, ConstantU32(state, 0u));
        state.module.AddFunction(spv::OpStore, state.loopGuardPc, ConstantU32(state, 0u));
    }
    state.module.AddFunction(spv::OpBranch, context.Label(blocks.front()));
    static const bool debug = std::getenv("APS5_SRT_DEBUG") != nullptr;
    if (debug) {
        const auto& infos = program.Metadata().blockInfo;
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const auto* byId = BlockInfoFor(program, blocks[index]);
            std::fprintf(stderr, "[spirv] order %zu: IR block %u, positional info id %u, id-matched info %s (kind %d true %u false %u)\n", index, blocks[index]->Id(),
                         index < infos.size() ? infos[index].id : 0xffffffffu, byId ? std::to_string(byId->id).c_str() : "none",
                         byId ? static_cast<int>(byId->terminator.kind) : -1, byId ? byId->terminator.trueBlock : 0u, byId ? byId->terminator.falseBlock : 0u);
        }
    }
    for (const IrBlock* block : blocks) {
        if (block == nullptr) {
            context.Fail("structured control flow contains a null block");
        }
        const BlockInfo* info = BlockInfoFor(program, block);
        if (info == nullptr) {
            context.Fail("structured control flow block has no terminator metadata");
        }
        const bool stops = state.bdaStopsInvocations;
        state.continueTarget = IsContinueTarget(program, info->id);
        state.bdaStopsInvocations = stops && !state.continueTarget;
        EmitStructuredBlock(context, functionState, block);
        functionState.blockExitLabels.emplace(block, state.currentLabel);
        EmitStructuredTerminator(context, program, *info);
        state.bdaStopsInvocations = stops;
        state.continueTarget = false;
    }
    PatchStructuredPhis(context, functionState);
}

void EmitVoid(SpirvValueEmitContext&) {
}

void EmitBarrier(SpirvValueEmitContext& ctx, const IrValue& inst) {
    auto& state = ctx.state;
    if (state.program.Resources().stage == IrShaderStage::Vertex) {
        for (const auto* block : state.program.BlockOrder()) {
            for (const auto* instruction : block->Instructions()) {
                if (CommunicatesThroughMemory(instruction->Opcode())) ctx.Fail(inst, "s_barrier in a vertex program that accesses LDS or GDS or writes memory is not implemented");
            }
        }
        return;
    }
    const bool tessellation = state.program.Resources().stage == IrShaderStage::TessellationControl;
    const auto memoryScope = tessellation ? spv::ScopeInvocation : spv::ScopeWorkgroup;
    const std::uint32_t semantics = tessellation ? static_cast<std::uint32_t>(spv::MemorySemanticsMaskNone) : spv::MemorySemanticsAcquireReleaseMask | LdsMemorySemantics(state);
    state.module.AddFunction(spv::OpControlBarrier, ConstantU32(state, spv::ScopeWorkgroup), ConstantU32(state, memoryScope), ConstantU32(state, semantics));
}

void EmitUnreachable(SpirvValueEmitContext& ctx, const IrValue& inst) {
    ctx.Fail(inst, "must be lowered before SPIR-V emission");
}

void EmitReference(SpirvValueEmitContext& context) {
    EmitVoid(context);
}

void EmitReferenceU32(SpirvValueEmitContext& context) {
    EmitVoid(context);
}

void EmitCheckDenormalFree(SpirvValueEmitContext& ctx, const IrValue& inst, std::uint32_t value, const IrValue* reason) {
    auto& state = ctx.state;
    if (state.faultBufferVariable == 0) ctx.Fail(inst, "checks a denormal-free proof without a fault buffer");
    const auto faultReason = static_cast<BdaAbi::FaultReason>(reason->Resolve()->ImmediateU32());
    if (faultReason != BdaAbi::FaultReason::FlushProofMismatch && faultReason != BdaAbi::FaultReason::InterpolationFactMismatch) ctx.Fail(inst, "checks a denormal-free proof with an unknown fault reason");
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    const auto exponentZero = Binary(state, spv::OpIEqual, boolean, Binary(state, spv::OpBitwiseAnd, u32, value, ConstantU32(state, 0x7f800000u)), ConstantU32(state, 0u));
    const auto nonZero = Binary(state, spv::OpINotEqual, boolean, Binary(state, spv::OpBitwiseAnd, u32, value, ConstantU32(state, 0x7fffffffu)), ConstantU32(state, 0u));
    EmitIfCondition(state, Binary(state, spv::OpLogicalAnd, boolean, exponentZero, nonZero), [&] {
        RecordBdaFaultWords(state, value, ConstantU32(state, 0u), ConstantU32(state, 0u), ConstantU32(state, 0u), faultReason);
    });
}

void EmitCheckEqualU32(SpirvValueEmitContext& ctx, const IrValue& inst, std::uint32_t expected, std::uint32_t actual, const IrValue* reason) {
    auto& state = ctx.state;
    if (state.faultBufferVariable == 0) ctx.Fail(inst, "checks an equality without a fault buffer");
    const auto faultReason = static_cast<BdaAbi::FaultReason>(reason->Resolve()->ImmediateU32());
    if (faultReason != BdaAbi::FaultReason::RegisterWindowMismatch) ctx.Fail(inst, "checks an equality with an unknown fault reason");
    EmitIfCondition(state, Binary(state, spv::OpINotEqual, TypeBool(state), expected, actual), [&] {
        RecordBdaFaultWords(state, expected, ConstantU32(state, 0u), actual, ConstantU32(state, 0u), faultReason);
    });
}

void EmitUnlinkedCall(SpirvValueEmitContext& ctx, const IrValue& inst, std::uint32_t low, std::uint32_t high, std::uint32_t programCounter) {
    auto& state = ctx.state;
    if (state.faultBufferVariable == 0) ctx.Fail(inst, "calls an unlinked target without a fault buffer");
    RecordBdaFaultWords(state, low, high, ConstantU32(state, 0u), programCounter, BdaAbi::FaultReason::UnlinkedCall);
    StopBdaInvocationIf(state, ConstantBool(state, true));
}

void EmitControlNop(SpirvValueEmitContext& context) {
    EmitVoid(context);
}

void EmitWaitcnt(SpirvValueEmitContext& context) {
    EmitVoid(context);
}

void EmitSendmsg(SpirvValueEmitContext& context) {
    EmitVoid(context);
}

void EmitTtraceData(SpirvValueEmitContext& context) {
    EmitVoid(context);
}

void EmitInstPrefetch(SpirvValueEmitContext& context) {
    EmitVoid(context);
}

void EmitPhi(SpirvValueEmitContext& ctx, const IrValue& inst) {
    EmitUnreachable(ctx, inst);
}

}

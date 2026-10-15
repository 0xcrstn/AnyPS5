#include "BdaShader.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvMemory/SpirvTypes.hpp"
#include "SpirvBackend/SpirvMemory/SpirvConstants.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include <functional>
#include <stdexcept>
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "SpirvBackend/SpirvOptimizer.hpp"
#endif

namespace {

ShaderRecompiler::BdaTable::Translation testTranslation = ShaderRecompiler::BdaTable::Translation::Legacy;
ShaderRecompiler::BdaTable::Relative testRelative = ShaderRecompiler::BdaTable::Relative::Off;

template<typename TEmit>
std::vector<std::uint32_t> MakeShader(std::uint64_t address, bool coherent, bool stops, const TEmit& emit, std::uint32_t extracted = 0, bool writes = false, const std::function<void(ShaderRecompiler::SpirvEmitterState&)>& prologue = {}) {
    using namespace ShaderRecompiler;
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.Info().usesDma = true;
    program.Info().bdaWrites = writes;
    if (coherent) {
        MemoryInfo memory;
        memory.coherent = true;
        program.Resources().memoryInfo = {memory};
    }
    SpirvEmitterState state(program, {});
    EmitBaseHeader(state.module, program);
    const auto define = [&](std::uint32_t binding) {
        const auto variable = state.module.DefineGlobalVariable(TypeStorageBufferPointer(state), spv::StorageClassStorageBuffer);
        state.module.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, 0u);
        state.module.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding, binding);
        return variable;
    };
    state.bdaPagetableVariable = define(0);
    state.faultBufferVariable = define(1);
    const auto output = define(2);
    std::vector<std::uint32_t> interfaceVariables;
    if (!stops) {
        interfaceVariables.push_back(state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassInput, TypeU32(state)), spv::StorageClassInput));
        state.module.AddAnnotation(spv::OpDecorate, interfaceVariables[0], spv::DecorationBuiltIn, spv::BuiltInLocalInvocationIndex);
    }
    state.bdaStopsInvocations = stops;
    state.bdaTranslation = testTranslation;
    state.bdaRelative = testRelative;
    DefineGetBdaPointer(state);
    const auto main = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunction, TypeVoid(state), main, spv::FunctionControlMaskNone, TypeFunction(state));
    EmitLabel(state, state.module.AllocateId());
    if (prologue) prologue(state);
    SpirvValueEmitContext ctx(state);
    auto& instruction = writes ? program.CreateValue(IrOpcode::StoreAddressU32, IrType::Void) : extracted != 0u ? program.CreateValue(IrOpcode::LoadAddressU32x4, IrType::U32x4) : program.CreateValue(IrOpcode::LoadAddressU32, IrType::U32);
    IrBuilder ir(program);
    for (std::uint32_t dword = 0; dword < 4u; ++dword) {
        if ((extracted & (1u << dword)) == 0u) continue;
        auto& extract = program.CreateValue(IrOpcode::CompositeExtractU32x4, IrType::U32);
        extract.AddArgument(&instruction);
        extract.AddArgument(&ir.Constant(dword));
    }
    MemoryFlags flags{};
    flags.pc = 0x1234;
    instruction.SetFlags(flags);
    auto base = BdaConstant(state, address);
    if (!stops) {
        const auto index = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, TypeU32(state), index, interfaceVariables[0]);
        base = Binary(state, spv::OpIAdd, TypeScalarU64(state), base, Unary(state, spv::OpUConvert, TypeScalarU64(state), index));
    }
    const auto values = emit(ctx, instruction, base);
    for (std::uint32_t index = 0; index < values.size(); ++index) state.module.AddFunction(spv::OpStore, BdaWord(state, output, ConstantU32(state, index)), values[index]);
    state.module.AddFunction(spv::OpReturn);
    state.module.AddFunction(spv::OpFunctionEnd);
    state.module.AddExecutionMode(main, spv::ExecutionModeLocalSize, 1u, 1u, 1u);
    state.module.EmitEntryPoint(spv::ExecutionModelGLCompute, main, "main", interfaceVariables);
    return state.module.Finalize();
}

}

void SetBdaTestTranslation(ShaderRecompiler::BdaTable::Translation translation) {
    testTranslation = translation;
}

ShaderRecompiler::BdaTable::Translation BdaTestTranslation() {
    return testTranslation;
}

void SetBdaTestRelative(ShaderRecompiler::BdaTable::Relative relative) {
    testRelative = relative;
}

ShaderRecompiler::BdaTable::Relative BdaTestRelative() {
    return testRelative;
}

std::vector<std::uint32_t> MakeBdaCapabilityTestShader(std::uint64_t address, std::uint64_t capabilityBase) {
    using namespace ShaderRecompiler;
    return MakeShader(address, false, true, [&](SpirvValueEmitContext& ctx, const IrValue& instruction, std::uint32_t base) {
        return std::vector<std::uint32_t>{EmitBdaRead(ctx, instruction, base, 8u)};
    }, 0u, false, [&](SpirvEmitterState& state) {
        if (state.bdaCapability[0] == 0) throw std::runtime_error("the capability test shader needs APS5_BDA_RELATIVE");
        state.module.AddFunction(spv::OpStore, state.bdaCapability[0], BdaConstant(state, address));
        state.module.AddFunction(spv::OpStore, state.bdaCapability[1], BdaConstant(state, address + 1u));
        state.module.AddFunction(spv::OpStore, state.bdaCapability[2], BdaConstant(state, capabilityBase));
        state.module.AddFunction(spv::OpStore, state.bdaCapability[3], ConstantU32(state, BdaAbi::Read));
    });
}

std::vector<std::uint32_t> MakeBdaResolveTestShader(std::uint32_t permission) {
    using namespace ShaderRecompiler;
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.Info().usesDma = true;
    SpirvEmitterState state(program, {});
    EmitBaseHeader(state.module, program);
    const auto define = [&](std::uint32_t binding) {
        const auto variable = state.module.DefineGlobalVariable(TypeStorageBufferPointer(state), spv::StorageClassStorageBuffer);
        state.module.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, 0u);
        state.module.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding, binding);
        return variable;
    };
    state.bdaPagetableVariable = define(0);
    const auto io = define(1);
    const std::array<std::uint32_t, 2> resolves{DefineBdaResolveFunction(state, false, permission), DefineBdaResolveFunction(state, true, permission)};
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto invocation = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassInput, TypeU32Vector(state, 3u)), spv::StorageClassInput);
    state.module.AddAnnotation(spv::OpDecorate, invocation, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId);
    const auto main = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunction, TypeVoid(state), main, spv::FunctionControlMaskNone, TypeFunction(state));
    EmitLabel(state, state.module.AllocateId());
    const auto id = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, TypeU32Vector(state, 3u), id, invocation);
    const auto index = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, u32, index, id, 0u);
    const auto base = Binary(state, spv::OpIMul, u32, index, ConstantU32(state, 12u));
    const auto word = [&](std::uint32_t offset) { return BdaWord(state, io, Binary(state, spv::OpIAdd, u32, base, ConstantU32(state, offset))); };
    const auto load = [&](std::uint32_t offset) {
        const auto value = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, u32, value, word(offset));
        return value;
    };
    const auto low = Unary(state, spv::OpUConvert, u64, load(0u));
    const auto high = Binary(state, spv::OpShiftLeftLogical, u64, Unary(state, spv::OpUConvert, u64, load(1u)), BdaConstant(state, 32u));
    const auto address = Binary(state, spv::OpBitwiseOr, u64, low, high);
    const auto bytes = load(2u);
    const auto resolvedType = state.module.Type(spv::OpTypeVector, u64, 2u);
    for (std::uint32_t kind = 0; kind < resolves.size(); ++kind) {
        const auto resolved = state.module.AllocateId();
        state.module.AddFunction(spv::OpFunctionCall, resolvedType, resolved, resolves[kind], address, bytes);
        const auto result = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeExtract, u64, result, resolved, 0u);
        const auto reason = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeExtract, u64, reason, resolved, 1u);
        state.module.AddFunction(spv::OpStore, word(4u + kind * 4u), Unary(state, spv::OpUConvert, u32, result));
        state.module.AddFunction(spv::OpStore, word(5u + kind * 4u), Unary(state, spv::OpUConvert, u32, Binary(state, spv::OpShiftRightLogical, u64, result, BdaConstant(state, 32u))));
        state.module.AddFunction(spv::OpStore, word(6u + kind * 4u), Unary(state, spv::OpUConvert, u32, reason));
    }
    state.module.AddFunction(spv::OpReturn);
    state.module.AddFunction(spv::OpFunctionEnd);
    state.module.AddExecutionMode(main, spv::ExecutionModeLocalSize, 64u, 1u, 1u);
    state.module.EmitEntryPoint(spv::ExecutionModelGLCompute, main, "main", {invocation});
    return state.module.Finalize();
}

std::vector<std::uint32_t> MakeBdaTestShader(std::uint64_t address, std::uint32_t bits, std::int64_t offset) {
    using namespace ShaderRecompiler;
    return MakeShader(address, false, true, [&](SpirvValueEmitContext& ctx, const IrValue& instruction, std::uint32_t base) {
        if (offset != 0) base = AddBdaAddress(ctx, instruction, base, BdaConstant(ctx.state, offset < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(offset) : static_cast<std::uint64_t>(offset)), offset < 0);
        return std::vector<std::uint32_t>{EmitBdaRead(ctx, instruction, base, bits)};
    });
}

std::vector<std::uint32_t> MakeBdaSpanReadTestShader(std::uint64_t address, std::uint32_t offset, std::uint32_t extracted, bool coherent, bool stops) {
    using namespace ShaderRecompiler;
    return MakeShader(address, coherent, stops, [&](SpirvValueEmitContext& ctx, const IrValue& instruction, std::uint32_t base) {
        const auto values = EmitBdaDwordReads(ctx, instruction, base, offset, 4u);
        return std::vector<std::uint32_t>(values.begin(), values.end());
    }, extracted);
}

std::vector<std::uint32_t> MakeBdaDwordWriteTestShader(std::uint64_t address, std::uint32_t dwords, const std::uint32_t* values) {
    using namespace ShaderRecompiler;
    return MakeShader(address, false, true, [&](SpirvValueEmitContext& ctx, const IrValue& instruction, std::uint32_t base) {
        auto& state = ctx.state;
        std::vector<std::uint32_t> parts;
        for (std::uint32_t dword = 0; dword < dwords; ++dword) parts.push_back(ConstantU32(state, values[dword]));
        const auto composite = state.module.AllocateId();
        if (dwords == 2u) state.module.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 2u), composite, parts[0], parts[1]);
        else if (dwords == 3u) state.module.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3u), composite, parts[0], parts[1], parts[2]);
        else state.module.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 4u), composite, parts[0], parts[1], parts[2], parts[3]);
        EmitBdaDwordWrites(ctx, instruction, base, 0u, dwords, composite);
        return std::vector<std::uint32_t>{ConstantU32(state, 1u)};
    }, 0u, true);
}

std::vector<std::uint32_t> MakeBdaDwordReadTestShader(std::uint64_t address, std::uint32_t dwords, bool coherent, bool stops) {
    using namespace ShaderRecompiler;
    return MakeShader(address, coherent, stops, [&](SpirvValueEmitContext& ctx, const IrValue& instruction, std::uint32_t base) {
        const auto values = EmitBdaDwordReads(ctx, instruction, base, 0u, dwords, true);
        return std::vector<std::uint32_t>(values.begin(), values.begin() + dwords);
    });
}

std::vector<std::uint32_t> MakeBdaStoreTestShader(std::uint64_t address, std::uint32_t value, std::uint32_t bits, bool coherent) {
    using namespace ShaderRecompiler;
    auto shader = MakeShader(address, coherent, true, [&](SpirvValueEmitContext& ctx, const IrValue& instruction, std::uint32_t base) {
        EmitBdaStore(ctx, instruction, base, ConstantU32(ctx.state, value), bits);
        return std::vector<std::uint32_t>{ConstantU32(ctx.state, 1u)};
    }, 0u, true);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    shader = ValidateAndOptimizeSpirv(shader, VK_API_VERSION_1_1, shader[1], false, false);
#endif
    return shader;
}

std::vector<std::uint32_t> MakeBdaWriteTestShader(std::uint64_t address, std::uint32_t value, std::uint32_t bits, bool coherent) {
    using namespace ShaderRecompiler;
    auto shader = MakeShader(address, coherent, true, [&](SpirvValueEmitContext& ctx, const IrValue& instruction, std::uint32_t base) {
        EmitBdaWrite(ctx, instruction, base, ConstantU32(ctx.state, value), bits);
        return std::vector<std::uint32_t>{ConstantU32(ctx.state, 1u)};
    }, 0u, true);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    shader = ValidateAndOptimizeSpirv(shader, VK_API_VERSION_1_1, shader[1], false, false);
#endif
    return shader;
}

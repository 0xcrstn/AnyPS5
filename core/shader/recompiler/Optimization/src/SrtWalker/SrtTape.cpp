#include "Optimization/SrtWalker/SrtTape.hpp"
#include "Optimization/SrtWalker/SrtAddressArithmetic.hpp"
#include "Optimization/SrtWalker/SrtEvaluator.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>

namespace ShaderRecompiler::Detail {

namespace {

class TapeCompiler {
public:
    TapeCompiler(const IrResourcePlan& plan, std::vector<SrtTape::Instruction>& instructions, std::size_t& reads) : plan(plan), instructions(instructions), reads(reads) {}

    std::optional<SrtTape::Operand> Compile(IrValue* raw) {
        if (raw == nullptr) return std::nullopt;
        IrValue* value = raw->Resolve();
        if (value->HasImmediate()) return immediate(*value);
        if (value->Opcode() == IrOpcode::Void) return std::nullopt;
        if (const auto found = compiled.find(value); found != compiled.end()) return found->second;
        if (std::find(visiting.begin(), visiting.end(), value) != visiting.end()) return std::nullopt;
        visiting.push_back(value);
        const auto operand = compileValue(*value);
        visiting.pop_back();
        if (operand) compiled.emplace(value, *operand);
        return operand;
    }

private:
    static std::optional<SrtTape::Operand> immediate(const IrValue& value) {
        SrtTape::Operand operand;
        operand.immediate = true;
        switch (value.Type()) {
            case IrType::Bool: operand.value = value.ImmediateBool() ? 1u : 0u; return operand;
            case IrType::U8: operand.value = value.ImmediateU8(); return operand;
            case IrType::U16: operand.value = value.ImmediateU16(); return operand;
            case IrType::U32: operand.value = value.ImmediateU32(); return operand;
            case IrType::U64: operand.value = value.ImmediateU64(); return operand;
            case IrType::F32: operand.value = std::bit_cast<std::uint32_t>(value.ImmediateF32()); return operand;
            default: return std::nullopt;
        }
    }

    SrtTape::Operand emit(SrtTape::Instruction instruction) {
        instructions.push_back(instruction);
        return SrtTape::Operand{static_cast<std::uint32_t>(instructions.size() - 1), false, 0};
    }

    std::optional<std::uint32_t> component(const IrValue& value) {
        if (value.ArgumentCount() < 2 || value.Argument(1) == nullptr) return std::nullopt;
        const IrValue* index = value.Argument(1)->Resolve();
        if (!index->HasImmediate() || index->Type() != IrType::U32 || index->ImmediateU32() >= 2u) return std::nullopt;
        return index->ImmediateU32();
    }

    std::optional<SrtTape::Operand> compileValue(IrValue& value) {
        switch (value.Opcode()) {
            case IrOpcode::GetUserData: {
                const auto reg = RegIndex(static_cast<ScalarReg>(value.Argument(0)->Register().index));
                if (reg < plan.userDataBase) return std::nullopt;
                SrtTape::Instruction instruction;
                instruction.step = SrtTape::Step::UserData;
                instruction.index = reg - plan.userDataBase;
                return emit(instruction);
            }
            case IrOpcode::GetShaderBase: {
                SrtTape::Instruction instruction;
                instruction.step = SrtTape::Step::ShaderBase;
                return emit(instruction);
            }
            case IrOpcode::Phi: {
                IrValue* resolved = ResolveInvariantPhi(plan, &value);
                if (resolved == nullptr) return std::nullopt;
                return Compile(resolved);
            }
            case IrOpcode::BitCastU32F32:
            case IrOpcode::BitCastF32U32: return Compile(value.Argument(0));
            case IrOpcode::CompositeExtractU64: {
                const auto index = component(value);
                if (!index) return std::nullopt;
                const auto packed = Compile(value.Argument(0));
                if (!packed) return std::nullopt;
                SrtTape::Instruction instruction;
                instruction.step = SrtTape::Step::Extract64;
                instruction.index = *index;
                instruction.count = 1;
                instruction.arguments[0] = *packed;
                return emit(instruction);
            }
            case IrOpcode::CompositeExtractU32x2: {
                const auto index = component(value);
                if (!index) return std::nullopt;
                IrValue* source = value.Argument(0)->Resolve();
                if (source->Opcode() == IrOpcode::CompositeConstructU32x2) return Compile(source->Argument(*index));
                if (source->Opcode() != IrOpcode::IAddCarry32) return std::nullopt;
                const auto lhs = Compile(source->Argument(0));
                if (!lhs) return std::nullopt;
                const auto rhs = Compile(source->Argument(1));
                if (!rhs) return std::nullopt;
                SrtTape::Instruction instruction;
                instruction.step = SrtTape::Step::AddCarry;
                instruction.index = *index;
                instruction.count = 2;
                instruction.arguments[0] = *lhs;
                instruction.arguments[1] = *rhs;
                return emit(instruction);
            }
            case IrOpcode::ReadConst: {
                if (value.ArgumentCount() < 2 || value.Argument(1) == nullptr) return std::nullopt;
                const IrValue* slot = value.Argument(1)->Resolve();
                if (!slot->HasImmediate() || slot->Type() != IrType::U32 || slot->ImmediateU32() >= plan.srtReads.size()) return std::nullopt;
                return Compile(plan.srtReads[slot->ImmediateU32()].value);
            }
            case IrOpcode::LoadAddressU32:
            case IrOpcode::ReadConstBuffer:
                if (!IsRawRead(plan, value)) return std::nullopt;
                return compileRead(value);
            default: break;
        }
        const auto count = SrtOperationArguments(value.Opcode());
        if (!count || value.ArgumentCount() < *count) return std::nullopt;
        SrtTape::Instruction instruction;
        instruction.step = SrtTape::Step::Operation;
        instruction.opcode = value.Opcode();
        instruction.count = static_cast<std::uint8_t>(*count);
        for (std::size_t index = 0; index < *count; ++index) {
            const auto argument = Compile(value.Argument(index));
            if (!argument) return std::nullopt;
            instruction.arguments[index] = *argument;
        }
        return emit(instruction);
    }

    std::optional<SrtTape::Operand> compileRead(IrValue& value) {
        const auto flags = value.Flags<MemoryFlags>();
        if (flags.index >= plan.memoryInfo.size()) return std::nullopt;
        const auto& memory = plan.memoryInfo[flags.index];
        IrValue* handle = value.Argument(0)->Resolve();
        if (handle->Opcode() == IrOpcode::Void || handle->ArgumentCount() < 2) return std::nullopt;
        const bool buffer = value.Opcode() == IrOpcode::ReadConstBuffer;
        SrtTape::Instruction instruction;
        instruction.step = buffer ? SrtTape::Step::RawBuffer : SrtTape::Step::RawAddress;
        instruction.immediate = static_cast<std::int64_t>(static_cast<std::int32_t>(memory.offset));
        if (buffer && (handle->ArgumentCount() != 4u || instruction.immediate < 0)) return std::nullopt;
        const auto low = Compile(handle->Argument(0));
        if (!low) return std::nullopt;
        const auto high = Compile(handle->Argument(1));
        if (!high) return std::nullopt;
        const auto offset = Compile(value.Argument(1));
        if (!offset) return std::nullopt;
        instruction.arguments[0] = *low;
        instruction.arguments[1] = *high;
        instruction.arguments[2] = *offset;
        instruction.count = 3;
        if (buffer) {
            const auto records = Compile(handle->Argument(2));
            if (!records) return std::nullopt;
            const auto word3 = Compile(handle->Argument(3));
            if (!word3) return std::nullopt;
            instruction.arguments[3] = *records;
            instruction.arguments[4] = *word3;
            instruction.count = 5;
        }
        instruction.loaded = LoadedFromMemory(*handle);
        ++reads;
        return emit(instruction);
    }

    const IrResourcePlan& plan;
    std::vector<SrtTape::Instruction>& instructions;
    std::size_t& reads;
    std::unordered_map<const IrValue*, SrtTape::Operand> compiled;
    std::vector<const IrValue*> visiting;
};

struct TapeScratchTag {};

}

bool CompiledWalkEnabled() {
    static const bool enabled = std::getenv("APS5_NO_COMPILED_WALK") == nullptr;
    return enabled;
}

std::shared_ptr<const SrtTape> SrtTape::Compile(const IrResourcePlan& plan, std::span<const std::uint32_t> sourceIndices) {
    if (!plan.srtPlanComplete || !plan.controlFlow.empty()) return nullptr;
    auto tape = std::make_shared<SrtTape>();
    tape->userDataBase = plan.userDataBase;
    TapeCompiler compiler(plan, tape->instructions, tape->reads);
    for (const auto sourceIndex : sourceIndices) {
        if (sourceIndex >= plan.descriptorSources.size()) return nullptr;
        const auto& source = plan.descriptorSources[sourceIndex];
        if (source.dwordCount > 8u) return nullptr;
        Source compiledSource;
        compiledSource.dwordCount = source.dwordCount;
        for (std::uint32_t index = 0; index < source.dwordCount; ++index) {
            const auto operand = compiler.Compile(source.dwords[index]);
            if (!operand) return nullptr;
            compiledSource.dwords[index] = *operand;
        }
        tape->sources.push_back(compiledSource);
    }
    tape->flatSize = plan.srtReads.size();
    for (const auto& read : plan.srtReads) {
        if (read.flatOffset >= tape->flatSize) return nullptr;
        const auto operand = compiler.Compile(read.value);
        if (!operand) return nullptr;
        tape->flatRoots.push_back({read.flatOffset, *operand});
    }
    tape->activeSize = plan.descriptorSources.size() + plan.info.images.size();
    return tape;
}

std::shared_ptr<const SrtTape> SrtTape::For(const IrResourcePlan& plan) {
    auto& cell = *plan.compiledWalk;
    std::call_once(cell.once, [&] { cell.walk = Compile(plan, plan.materializationSources); });
    return std::static_pointer_cast<const SrtTape>(cell.walk);
}

bool SrtTape::Run(const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::vector<std::uint8_t>& activeSources, std::uint32_t& nullRootReads) const {
    auto& values = HostThreadLocal<std::vector<std::uint64_t>, TapeScratchTag>();
    values.resize(instructions.size());
    const auto get = [&](const Operand& operand) { return operand.immediate ? operand.value : values[operand.slot]; };
    std::uint32_t nullRoots = 0;
    for (std::size_t position = 0; position < instructions.size(); ++position) {
        const auto& instruction = instructions[position];
        std::uint64_t result = 0;
        switch (instruction.step) {
            case Step::UserData:
                if (instruction.index >= runtime.userData.size()) return false;
                result = runtime.userData[instruction.index];
                break;
            case Step::ShaderBase: result = runtime.shaderBase; break;
            case Step::Operation: {
                std::array<std::uint64_t, 4> arguments{};
                for (std::size_t index = 0; index < instruction.count; ++index) arguments[index] = get(instruction.arguments[index]);
                if (!ApplySrtOperation(instruction.opcode, arguments, result)) return false;
                break;
            }
            case Step::Extract64: result = static_cast<std::uint32_t>(get(instruction.arguments[0]) >> (instruction.index * 32u)); break;
            case Step::AddCarry: {
                const auto sum = static_cast<std::uint64_t>(static_cast<std::uint32_t>(get(instruction.arguments[0]))) + static_cast<std::uint32_t>(get(instruction.arguments[1]));
                result = instruction.index == 0u ? static_cast<std::uint32_t>(sum) : static_cast<std::uint32_t>(sum >> 32u);
                break;
            }
            case Step::RawAddress:
            case Step::RawBuffer: {
                const auto low = get(instruction.arguments[0]);
                const auto high = get(instruction.arguments[1]);
                const auto offset = get(instruction.arguments[2]);
                const auto base = ((high << 32u) | static_cast<std::uint32_t>(low)) & AddressMask;
                std::uint64_t address = 0;
                if (instruction.step == Step::RawBuffer) {
                    const auto records = get(instruction.arguments[3]);
                    const auto byteOffset = static_cast<std::uint64_t>(instruction.immediate) + static_cast<std::uint32_t>(offset);
                    const auto aligned = byteOffset & ~std::uint64_t {3};
                    const auto stride = (static_cast<std::uint32_t>(high) >> 16u) & 0x3fffu;
                    const auto size = stride == 0u ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(records)) : static_cast<std::uint64_t>(stride) * static_cast<std::uint32_t>(records);
                    if (aligned > size || size - aligned < sizeof(std::uint32_t)) {
                        values[position] = 0;
                        continue;
                    }
                    address = ((base & ~std::uint64_t {3}) + byteOffset) & ~std::uint64_t {3};
                } else {
                    const auto relative = (instruction.immediate & ~std::int64_t {3}) + static_cast<std::int64_t>(static_cast<std::uint32_t>(offset) & ~3u);
                    if (!AddSignedAddress(base & ~std::uint64_t {3}, relative, address)) return false;
                }
                if (base == 0 && !instruction.loaded) {
                    ++nullRoots;
                    values[position] = 0;
                    continue;
                }
                if (runtime.accessible != nullptr && !runtime.accessible(runtime.userContext, address, sizeof(std::uint32_t)) && instruction.loaded) return false;
                std::uint32_t word = 0;
                if (runtime.readMemory != nullptr) {
                    if (!runtime.readMemory(runtime.userContext, address, &word)) return false;
                } else {
                    std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
                }
                result = word;
                break;
            }
        }
        values[position] = result;
    }
    std::vector<DescriptorValue> evaluated;
    evaluated.reserve(sources.size());
    for (const auto& source : sources) {
        DescriptorValue value;
        value.dwordCount = source.dwordCount;
        for (std::uint32_t index = 0; index < source.dwordCount; ++index) value.dwords[index] = static_cast<std::uint32_t>(get(source.dwords[index]));
        evaluated.push_back(value);
    }
    std::vector<std::uint32_t> flattened(flatSize);
    for (const auto& root : flatRoots) flattened[root.flatOffset] = static_cast<std::uint32_t>(get(root.value));
    results = std::move(evaluated);
    flat = std::move(flattened);
    activeSources.assign(activeSize, 1u);
    nullRootReads = nullRoots;
    return true;
}

}

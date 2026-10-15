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
#include <map>
#include <optional>
#include <utility>

namespace ShaderRecompiler::Detail {

namespace {

constexpr std::uint32_t MainDomain = 0;
constexpr std::uint32_t CleanDomain = 1;
constexpr int MaxDepth = 512;

struct Domain {
    bool clean = false;
    const IrValue* mask = nullptr;
};

class TapeCompiler {
public:
    TapeCompiler(const IrResourcePlan& plan, std::vector<SrtTape::Instruction>& instructions, std::size_t& reads) : plan(plan), instructions(instructions), reads(reads) {
        domains.push_back({false, nullptr});
        domains.push_back({true, nullptr});
    }

    std::optional<SrtTape::Operand> Compile(IrValue* raw, std::uint32_t domain) {
        if (raw == nullptr) return refuse(SrtCompiledWalkRefusal::VoidHandle);
        IrValue* value = raw->Resolve();
        if (value->HasImmediate()) return immediate(*value);
        if (value->Opcode() == IrOpcode::Void) return refuse(SrtCompiledWalkRefusal::Opcode);
        const auto key = std::pair(domain, static_cast<const IrValue*>(value));
        if (const auto found = compiled.find(key); found != compiled.end()) return found->second;
        if (visiting.size() >= MaxDepth || std::find(visiting.begin(), visiting.end(), key) != visiting.end()) return refuse(SrtCompiledWalkRefusal::Cycle);
        visiting.push_back(key);
        const auto operand = compileValue(*value, domain);
        visiting.pop_back();
        if (operand) compiled.emplace(key, *operand);
        return operand;
    }

    SrtCompiledWalkRefusal Refusal() const { return refusal; }
    SrtCompiledWalkRefusal TakeRefusal() { return std::exchange(refusal, SrtCompiledWalkRefusal::None); }

private:
    std::optional<SrtTape::Operand> refuse(SrtCompiledWalkRefusal reason) {
        if (refusal == SrtCompiledWalkRefusal::None) refusal = reason;
        return std::nullopt;
    }

    std::optional<SrtTape::Operand> immediate(const IrValue& value) {
        SrtTape::Operand operand;
        operand.immediate = true;
        switch (value.Type()) {
            case IrType::Bool: operand.value = value.ImmediateBool() ? 1u : 0u; return operand;
            case IrType::U8: operand.value = value.ImmediateU8(); return operand;
            case IrType::U16: operand.value = value.ImmediateU16(); return operand;
            case IrType::U32: operand.value = value.ImmediateU32(); return operand;
            case IrType::U64: operand.value = value.ImmediateU64(); return operand;
            case IrType::F32: operand.value = std::bit_cast<std::uint32_t>(value.ImmediateF32()); return operand;
            default: return refuse(SrtCompiledWalkRefusal::Opcode);
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

    std::uint32_t laneDomain(std::uint32_t parent, const IrValue* mask) {
        const auto key = std::pair(parent, mask);
        if (const auto found = laneDomains.find(key); found != laneDomains.end()) return found->second;
        domains.push_back({domains[parent].clean, mask});
        const auto id = static_cast<std::uint32_t>(domains.size() - 1);
        laneDomains.emplace(key, id);
        return id;
    }

    std::optional<SrtTape::Operand> compileValue(IrValue& value, std::uint32_t domain) {
        const auto scope = domains[domain];
        if (scope.mask != nullptr && IsRuntimeSelect(value.Opcode()) && value.ArgumentCount() == 3 && value.Argument(0) != nullptr && value.Argument(0)->Resolve() == scope.mask) return Compile(value.Argument(1), domain);
        switch (value.Opcode()) {
            case IrOpcode::GetUserData: {
                const auto reg = RegIndex(static_cast<ScalarReg>(value.Argument(0)->Register().index));
                if (reg < plan.userDataBase) return refuse(SrtCompiledWalkRefusal::UserData);
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
                if (resolved == nullptr) return refuse(SrtCompiledWalkRefusal::Phi);
                return Compile(resolved, domain);
            }
            case IrOpcode::ReadFirstLane: {
                if (value.ArgumentCount() < 2 || value.Argument(0) == nullptr || value.Argument(1) == nullptr) return refuse(SrtCompiledWalkRefusal::Opcode);
                return Compile(value.Argument(0), laneDomain(domain, value.Argument(1)->Resolve()));
            }
            case IrOpcode::BitCastU32F32:
            case IrOpcode::BitCastF32U32: return Compile(value.Argument(0), domain);
            case IrOpcode::CompositeExtractU64: {
                const auto index = component(value);
                if (!index) return refuse(SrtCompiledWalkRefusal::Opcode);
                const auto packed = Compile(value.Argument(0), domain);
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
                if (!index) return refuse(SrtCompiledWalkRefusal::Opcode);
                IrValue* source = value.Argument(0)->Resolve();
                if (source->Opcode() == IrOpcode::Void) return refuse(SrtCompiledWalkRefusal::Opcode);
                if (source->Opcode() == IrOpcode::CompositeConstructU32x2) return Compile(source->Argument(*index), domain);
                if (source->Opcode() != IrOpcode::IAddCarry32) return refuse(SrtCompiledWalkRefusal::Opcode);
                const auto lhs = Compile(source->Argument(0), domain);
                if (!lhs) return std::nullopt;
                const auto rhs = Compile(source->Argument(1), domain);
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
                if (value.ArgumentCount() < 2 || value.Argument(1) == nullptr) return refuse(SrtCompiledWalkRefusal::Opcode);
                const IrValue* slot = value.Argument(1)->Resolve();
                if (!slot->HasImmediate() || slot->Type() != IrType::U32 || slot->ImmediateU32() >= plan.srtReads.size()) return refuse(SrtCompiledWalkRefusal::FlatOffset);
                const auto index = slot->ImmediateU32();
                const bool clean = !scope.clean && index < plan.cleanFlatSlots.size() && plan.cleanFlatSlots[index] != 0u;
                return Compile(plan.srtReads[index].value, clean ? CleanDomain : domain);
            }
            case IrOpcode::LoadAddressU32:
            case IrOpcode::ReadConstBuffer:
                if (!IsRawRead(plan, value)) return refuse(SrtCompiledWalkRefusal::Opcode);
                return compileRead(value, domain);
            default: break;
        }
        const auto count = SrtOperationArguments(value.Opcode());
        if (!count || value.ArgumentCount() < *count) return refuse(SrtCompiledWalkRefusal::Opcode);
        SrtTape::Instruction instruction;
        instruction.step = SrtTape::Step::Operation;
        instruction.opcode = value.Opcode();
        instruction.count = static_cast<std::uint8_t>(*count);
        for (std::size_t index = 0; index < *count; ++index) {
            const auto argument = Compile(value.Argument(index), domain);
            if (!argument) return std::nullopt;
            instruction.arguments[index] = *argument;
        }
        return emit(instruction);
    }

    std::optional<SrtTape::Operand> compileRead(IrValue& value, std::uint32_t domain) {
        const auto flags = value.Flags<MemoryFlags>();
        if (flags.index >= plan.memoryInfo.size()) return refuse(SrtCompiledWalkRefusal::Opcode);
        const auto& memory = plan.memoryInfo[flags.index];
        if (value.ArgumentCount() < 2 || value.Argument(0) == nullptr || value.Argument(1) == nullptr) return refuse(SrtCompiledWalkRefusal::VoidHandle);
        IrValue* handle = value.Argument(0)->Resolve();
        if (handle->Opcode() == IrOpcode::Void || handle->ArgumentCount() < 2) return refuse(SrtCompiledWalkRefusal::VoidHandle);
        const bool buffer = value.Opcode() == IrOpcode::ReadConstBuffer;
        SrtTape::Instruction instruction;
        instruction.step = buffer ? SrtTape::Step::RawBuffer : SrtTape::Step::RawAddress;
        instruction.immediate = static_cast<std::int64_t>(static_cast<std::int32_t>(memory.offset));
        instruction.clean = domains[domain].clean;
        if (buffer && (handle->ArgumentCount() != 4u || instruction.immediate < 0)) return refuse(SrtCompiledWalkRefusal::VoidHandle);
        const auto low = Compile(handle->Argument(0), domain);
        if (!low) return std::nullopt;
        const auto high = Compile(handle->Argument(1), domain);
        if (!high) return std::nullopt;
        const auto offset = Compile(value.Argument(1), domain);
        if (!offset) return std::nullopt;
        instruction.arguments[0] = *low;
        instruction.arguments[1] = *high;
        instruction.arguments[2] = *offset;
        instruction.count = 3;
        if (buffer) {
            const auto records = Compile(handle->Argument(2), domain);
            if (!records) return std::nullopt;
            const auto word3 = Compile(handle->Argument(3), domain);
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
    std::vector<Domain> domains;
    std::map<std::pair<std::uint32_t, const IrValue*>, SrtTape::Operand> compiled;
    std::map<std::pair<std::uint32_t, const IrValue*>, std::uint32_t> laneDomains;
    std::vector<std::pair<std::uint32_t, const IrValue*>> visiting;
    SrtCompiledWalkRefusal refusal = SrtCompiledWalkRefusal::None;
};

struct TapeValuesTag {};
struct TapeStatesTag {};

}

bool CompiledWalkEnabled() {
    static const bool enabled = std::getenv("APS5_NO_COMPILED_WALK") == nullptr;
    return enabled;
}

class SrtTape::Runner {
public:
    Runner(const SrtTape& tape, const SrtRuntime& runtime, std::vector<std::uint64_t>& values, std::vector<std::uint8_t>& states) : tape(tape), runtime(runtime), values(values), states(states) {}

    bool Evaluate(const Operand& operand, std::uint64_t& result, int depth = 0) {
        if (operand.immediate) {
            result = operand.value;
            return true;
        }
        if (!evaluate(operand.slot, depth)) return false;
        result = values[operand.slot];
        return true;
    }

    bool inaccessible = false;
    std::uint32_t nullRoots = 0;

private:
    bool evaluate(std::uint32_t slot, int depth) {
        if (states[slot] != 0u) return true;
        if (depth > MaxDepth) return false;
        const auto& instruction = tape.instructions[slot];
        std::uint64_t result = 0;
        switch (instruction.step) {
            case Step::UserData:
                if (instruction.index >= runtime.userData.size()) return false;
                result = runtime.userData[instruction.index];
                break;
            case Step::ShaderBase: result = runtime.shaderBase; break;
            case Step::Operation: {
                std::array<std::uint64_t, 4> arguments{};
                for (std::size_t index = 0; index < instruction.count; ++index) {
                    if (!Evaluate(instruction.arguments[index], arguments[index], depth + 1)) return false;
                }
                if (!ApplySrtOperation(instruction.opcode, arguments, result)) return false;
                break;
            }
            case Step::Extract64: {
                std::uint64_t packed = 0;
                if (!Evaluate(instruction.arguments[0], packed, depth + 1)) return false;
                result = static_cast<std::uint32_t>(packed >> (instruction.index * 32u));
                break;
            }
            case Step::AddCarry: {
                std::uint64_t lhs = 0;
                std::uint64_t rhs = 0;
                if (!Evaluate(instruction.arguments[0], lhs, depth + 1) || !Evaluate(instruction.arguments[1], rhs, depth + 1)) return false;
                const auto sum = static_cast<std::uint64_t>(static_cast<std::uint32_t>(lhs)) + static_cast<std::uint32_t>(rhs);
                result = instruction.index == 0u ? static_cast<std::uint32_t>(sum) : static_cast<std::uint32_t>(sum >> 32u);
                break;
            }
            case Step::RawAddress:
            case Step::RawBuffer: {
                std::uint64_t low = 0;
                std::uint64_t high = 0;
                std::uint64_t offset = 0;
                if (!Evaluate(instruction.arguments[0], low, depth + 1) || !Evaluate(instruction.arguments[1], high, depth + 1) || !Evaluate(instruction.arguments[2], offset, depth + 1)) return false;
                const auto base = ((high << 32u) | static_cast<std::uint32_t>(low)) & AddressMask;
                std::uint64_t address = 0;
                if (instruction.step == Step::RawBuffer) {
                    std::uint64_t records = 0;
                    std::uint64_t word3 = 0;
                    if (!Evaluate(instruction.arguments[3], records, depth + 1) || !Evaluate(instruction.arguments[4], word3, depth + 1)) return false;
                    const auto byteOffset = static_cast<std::uint64_t>(instruction.immediate) + static_cast<std::uint32_t>(offset);
                    const auto aligned = byteOffset & ~std::uint64_t {3};
                    const auto stride = (static_cast<std::uint32_t>(high) >> 16u) & 0x3fffu;
                    const auto size = stride == 0u ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(records)) : static_cast<std::uint64_t>(stride) * static_cast<std::uint32_t>(records);
                    if (aligned > size || size - aligned < sizeof(std::uint32_t)) {
                        values[slot] = 0;
                        states[slot] = 1u;
                        return true;
                    }
                    address = ((base & ~std::uint64_t {3}) + byteOffset) & ~std::uint64_t {3};
                } else {
                    const auto relative = (instruction.immediate & ~std::int64_t {3}) + static_cast<std::int64_t>(static_cast<std::uint32_t>(offset) & ~3u);
                    if (!AddSignedAddress(base & ~std::uint64_t {3}, relative, address)) return false;
                }
                if (base == 0 && !instruction.loaded) {
                    ++nullRoots;
                    values[slot] = 0;
                    states[slot] = 1u;
                    return true;
                }
                if (runtime.accessible != nullptr && instruction.loaded && !runtime.accessible(runtime.userContext, address, sizeof(std::uint32_t))) {
                    inaccessible = true;
                    return false;
                }
                std::uint32_t word = 0;
                const auto reader = instruction.clean ? runtime.readSpecializationMemory : runtime.readMemory;
                if (reader != nullptr) {
                    if (!reader(runtime.userContext, address, &word)) return false;
                } else {
                    std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
                }
                result = word;
                break;
            }
        }
        values[slot] = result;
        states[slot] = 1u;
        return true;
    }

    const SrtTape& tape;
    const SrtRuntime& runtime;
    std::vector<std::uint64_t>& values;
    std::vector<std::uint8_t>& states;
};

std::shared_ptr<const SrtTape> SrtTape::Compile(const IrResourcePlan& plan, std::span<const std::uint32_t> sourceIndices) {
    auto tape = std::make_shared<SrtTape>();
    const auto refuse = [&](SrtCompiledWalkRefusal reason) {
        tape->refusal = reason;
        tape->instructions.clear();
        tape->sources.clear();
        tape->flatRoots.clear();
        tape->blocks.clear();
        return tape;
    };
    if (!plan.srtPlanComplete) return refuse(SrtCompiledWalkRefusal::IncompletePlan);
    if (plan.requiresSpecializationMemory || std::ranges::any_of(plan.descriptorSources, [](const DescriptorSource& source) { return source.tableColumn.has_value(); })) return refuse(SrtCompiledWalkRefusal::IndirectImage);
    tape->userDataBase = plan.userDataBase;
    tape->sourceCount = plan.descriptorSources.size();
    tape->imageCount = plan.info.images.size();
    TapeCompiler compiler(plan, tape->instructions, tape->reads);
    const auto failed = [&] { return compiler.Refusal() != SrtCompiledWalkRefusal::None ? compiler.Refusal() : SrtCompiledWalkRefusal::Opcode; };
    for (const auto& block : plan.controlFlow) {
        Block compiledBlock;
        compiledBlock.successors = block.successors;
        compiledBlock.sources = block.sources;
        compiledBlock.images = block.images;
        for (const auto successor : block.successors) {
            if (successor >= plan.controlFlow.size()) return refuse(SrtCompiledWalkRefusal::IncompletePlan);
        }
        for (const auto source : block.sources) {
            if (source >= tape->sourceCount) return refuse(SrtCompiledWalkRefusal::IncompletePlan);
        }
        for (const auto image : block.images) {
            if (image >= tape->imageCount) return refuse(SrtCompiledWalkRefusal::IncompletePlan);
        }
        if (block.condition != nullptr) {
            if (const auto condition = compiler.Compile(block.condition, CleanDomain)) {
                compiledBlock.conditional = true;
                compiledBlock.condition = *condition;
            } else {
                static_cast<void>(compiler.TakeRefusal());
            }
        }
        tape->blocks.push_back(std::move(compiledBlock));
    }
    for (const auto sourceIndex : sourceIndices) {
        if (sourceIndex >= plan.descriptorSources.size()) return refuse(SrtCompiledWalkRefusal::IncompletePlan);
        const auto& source = plan.descriptorSources[sourceIndex];
        if (source.dwordCount > 8u) return refuse(SrtCompiledWalkRefusal::WideSource);
        Source compiledSource;
        compiledSource.index = sourceIndex;
        compiledSource.dwordCount = source.dwordCount;
        for (std::uint32_t index = 0; index < source.dwordCount; ++index) {
            const auto operand = compiler.Compile(source.dwords[index], MainDomain);
            if (!operand) return refuse(failed());
            compiledSource.dwords[index] = *operand;
        }
        tape->sources.push_back(compiledSource);
    }
    tape->flatSize = plan.srtReads.size();
    tape->requiresCleanMemory = std::ranges::any_of(plan.cleanFlatSlots, [](std::uint8_t clean) { return clean != 0u; });
    for (const auto& read : plan.srtReads) {
        if (read.flatOffset >= tape->flatSize) return refuse(SrtCompiledWalkRefusal::FlatOffset);
        const bool clean = read.flatOffset < plan.cleanFlatSlots.size() && plan.cleanFlatSlots[read.flatOffset] != 0u;
        const auto operand = compiler.Compile(read.value, clean ? CleanDomain : MainDomain);
        if (!operand) return refuse(failed());
        tape->flatRoots.push_back({read.flatOffset, *operand});
    }
    return tape;
}

std::shared_ptr<const SrtTape> SrtTape::For(const IrResourcePlan& plan) {
    auto& cell = *plan.compiledWalk;
    std::call_once(cell.once, [&] { cell.walk = Compile(plan, plan.materializationSources); });
    return std::static_pointer_cast<const SrtTape>(cell.walk);
}

SrtTape::Outcome SrtTape::Run(const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::vector<std::uint8_t>& activeSources, std::uint32_t& nullRootReads) const {
    if (!Compiled() || (requiresCleanMemory && runtime.readSpecializationMemory == nullptr)) return Outcome::Failed;
    auto& values = HostThreadLocal<std::vector<std::uint64_t>, TapeValuesTag>();
    auto& states = HostThreadLocal<std::vector<std::uint8_t>, TapeStatesTag>();
    values.resize(instructions.size());
    states.assign(instructions.size(), 0u);
    Runner runner(*this, runtime, values, states);
    std::vector<std::uint8_t> active(sourceCount + imageCount, 1u);
    if (!blocks.empty()) {
        for (const auto& block : blocks) {
            for (const auto source : block.sources) active[source] = 0u;
            for (const auto image : block.images) active[sourceCount + image] = 0u;
        }
        std::vector<std::uint8_t> visited(blocks.size(), 0u);
        std::vector<std::uint32_t> pending{0};
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (visited[index] != 0u) continue;
            visited[index] = 1u;
            const auto& block = blocks[index];
            for (const auto source : block.sources) active[source] = 1u;
            for (const auto image : block.images) active[sourceCount + image] = 1u;
            bool evaluable = false;
            std::uint64_t condition = 0;
            if (block.conditional && runtime.readSpecializationMemory != nullptr && block.successors.size() >= 2) {
                const auto nullRootsBefore = runner.nullRoots;
                evaluable = runner.Evaluate(block.condition, condition) && runner.nullRoots == nullRootsBefore;
                runner.inaccessible = false;
            }
            if (evaluable) pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
            else pending.insert(pending.end(), block.successors.begin(), block.successors.end());
        }
    }
    const auto conditionNullRoots = runner.nullRoots;
    std::vector<DescriptorValue> evaluated;
    evaluated.reserve(sources.size());
    const auto outcome = [&] { return runner.inaccessible ? Outcome::Inaccessible : Outcome::Failed; };
    for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        const auto& source = sources[sourceIndex];
        DescriptorValue value;
        value.dwordCount = source.dwordCount;
        if (source.index < active.size() && active[source.index] != 0u) {
            for (std::uint32_t index = 0; index < source.dwordCount; ++index) {
                std::uint64_t word = 0;
                if (!runner.Evaluate(source.dwords[index], word)) return outcome();
                value.dwords[index] = static_cast<std::uint32_t>(word);
            }
        }
        evaluated.push_back(value);
    }
    std::vector<std::uint32_t> flattened(flatSize);
    for (const auto& root : flatRoots) {
        std::uint64_t word = 0;
        if (!runner.Evaluate(root.value, word)) return outcome();
        flattened[root.flatOffset] = static_cast<std::uint32_t>(word);
    }
    results = std::move(evaluated);
    flat = std::move(flattened);
    activeSources = std::move(active);
    nullRootReads = runner.nullRoots - conditionNullRoots;
    return Outcome::Served;
}

}

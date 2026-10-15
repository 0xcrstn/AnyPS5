#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_SRTWALKER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_SRTWALKER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "UserDataUse.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

using SrtMemoryReader = bool (*)(void* userData, std::uint64_t address, std::uint32_t* value);
using SrtMemoryProbe = bool (*)(void* userData, std::uint64_t address, std::uint64_t bytes);
using SrtMemoryBulkReader = bool (*)(void* userData, std::uint64_t address, std::uint32_t* values, std::uint64_t count);
using SrtStableWord = bool (*)(void* userData, std::uint64_t address, std::uint32_t word);

namespace Detail {
class SrtTape;
}

struct SrtTapeInstance {
    const Detail::SrtTape* tape = nullptr;
    std::vector<std::uint64_t> values;
    std::vector<std::uint64_t> changedAt;
    std::vector<std::uint64_t> computedAt;
    std::uint64_t runs = 0;
    std::uint64_t keptReads = 0;
    std::uint64_t freshReads = 0;
    std::uint64_t keptValues = 0;
};

// The guest addresses a walk dereferenced (Detail::Evaluator::EvaluateRawRead): the leaf read of
// each pure flat slot (IrResourcePlan::pureFlatSlots) as (flat offset, address), set by the
// flattened loop through `leaf`/`leafSlot` around that slot's evaluation, and every other raw read
// address.
struct SrtReadTrace {
    const IrValue* leaf = nullptr;
    std::uint32_t leafSlot = 0;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> leaves;
    std::vector<std::uint64_t> otherReads;
};

struct SrtRuntime {
    std::span<const std::uint32_t> userData;
    std::uint64_t shaderBase = 0;
    SrtMemoryReader readMemory = nullptr;
    void* userContext = nullptr;
    SrtMemoryReader readSpecializationMemory = nullptr;
    SrtReadTrace* readTrace = nullptr;
    SrtMemoryProbe accessible = nullptr;
    SrtUserDataUse* userDataUse = nullptr;
    bool compiledWalk = false;
    SrtMemoryProbe pendingWrite = nullptr;
    SrtMemoryBulkReader readWords = nullptr;
    SrtStableWord stableWord = nullptr;
    SrtTapeInstance* tapeInstance = nullptr;
};

enum class SrtCompiledWalkRefusal : std::uint8_t { None, IncompletePlan, Opcode, Phi, VoidHandle, WideSource, Cycle, FlatOffset, UserData, IndirectImage, Count };
inline constexpr std::size_t SrtCompiledWalkRefusals = static_cast<std::size_t>(SrtCompiledWalkRefusal::Count);
inline constexpr const char* SrtCompiledWalkRefusalNames[SrtCompiledWalkRefusals] = {"none", "incomplete plan", "unsupported opcode", "variant phi", "void handle", "wide source", "cycle", "flat offset", "user data register", "indirect image"};

struct SrtCompiledWalkCounts {
    std::uint64_t runs = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t fallbacksInaccessible = 0;
    std::uint64_t uncompiled = 0;
    std::uint64_t indirectImages = 0;
    std::array<std::uint64_t, SrtCompiledWalkRefusals> refusals{};
};
[[nodiscard]] SrtCompiledWalkCounts CompiledWalkCounts();

struct SrtEvaluationCounts {
    std::uint64_t values = 0;
    std::uint64_t laneEvaluators = 0;
    std::uint64_t phis = 0;
};
[[nodiscard]] SrtEvaluationCounts ThreadSrtEvaluationCounts();

enum class RuntimeValueType {
    Any,
    Integer
};

class SrtWalker {
public:
    void BuildPlan(IrProgram& program) const;
    [[nodiscard]] bool ValidateRuntimeValue(const IrResourcePlan& program, const IrValue* value, RuntimeValueType type = RuntimeValueType::Any) const;
    void EvaluateUniformValues(const IrResourcePlan& program, std::span<IrValue* const> values, const SrtRuntime& runtime, std::span<std::uint32_t> results) const;
    void EvaluateDescriptorSource(const IrResourcePlan& program, std::uint32_t source, const SrtRuntime& runtime, DescriptorValue& result) const;
    void EvaluateDescriptorSources(const IrResourcePlan& program, std::span<const std::uint32_t> sources, const SrtRuntime& runtime, std::vector<DescriptorValue>& results) const;
    void EvaluateRuntimeSources(const IrResourcePlan& program, std::span<const std::uint32_t> sources, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::span<const std::uint8_t> cleanFlatSlots, std::vector<std::uint8_t>& activeSources, std::vector<SrtReadPoison>* poison = nullptr, std::uint32_t* nullRootReads = nullptr) const;
    void Walk(const IrResourcePlan& program, const SrtRuntime& runtime, std::vector<std::uint32_t>& flat) const;

};

}

#endif

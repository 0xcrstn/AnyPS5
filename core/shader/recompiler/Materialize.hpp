#ifndef CORE_SHADER_RECOMPILER_MATERIALIZE_HPP
#define CORE_SHADER_RECOMPILER_MATERIALIZE_HPP

#include "Recompiler.hpp"
#include "CompiledVariant.hpp"
#include "RuntimeAbi.hpp"
#include "Optimization/include/Optimization/DescriptorBindingBuilder.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <vector>

namespace ShaderRecompiler::Materialization {

struct RetainedCounters {
    std::atomic<std::uint64_t> sources{0}, plans{0}, planValues{0}, planBytes{0}, spilledPlans{0}, spilledPlanBytes{0}, planReloads{0}, programs{0}, variants{0}, variantSpirvBytes{0}, variantMetadataBytes{0}, irPrograms{0}, irBytes{0}, irPeakBytes{0}, spilledVariants{0}, spilledSpirvBytes{0}, spirvReloads{0}, spirvReloadBytes{0}, bindingPlans{0}, specializedModules{0}, specializedSpirvBytes{0}, registrationModules{0};
};

inline RetainedCounters& retainedCounters() {
    static RetainedCounters counters;
    return counters;
}

inline std::array<std::uint32_t, 3> partialThreads(const RecompileRequest& request) {
    return request.context.compute ? request.context.compute->partialThreads : std::array<std::uint32_t, 3>{};
}

inline std::uint64_t nextVariantId() {
    static std::atomic<std::uint64_t> variants{0};
    return variants.fetch_add(1, std::memory_order_relaxed) + 1;
}

class PrimedSharedMutex : public std::shared_mutex {
public:
    PrimedSharedMutex() {
        lock();
        unlock();
    }
};

struct PreparedBindingPlan;
struct PreparedModuleEntry;

struct MaterializedParts {
    std::shared_ptr<PreparedBindingPlan> plan;
    std::shared_ptr<PreparedModuleEntry> module;
    const DescriptorBindingPlan* bindings = nullptr;
    std::array<std::uint32_t, ShaderVertexStageInfo::MaxResources / RuntimeAbi::VertexSelectorsPerDword> runtimeSelectors{};
};

void bindingPlanKeyWords(const CompiledVariant& variant, const ResourceSnapshot& snapshot, std::span<const std::uint8_t> exports, std::vector<std::uint64_t>& key);
RecompileResult materializeSkeleton(const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot, bool registrationDefaults, MaterializedParts& parts);
void fillMaterializedValues(RecompileResult& result, const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot, const MaterializedParts& parts);
RecompileResult materializeResult(const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot, bool registrationDefaults = false);

}

#endif

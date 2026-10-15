#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTEPIPELINEAHEADREQUEST_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTEPIPELINEAHEADREQUEST_HPP

#include "Recompiler.hpp"
#include "CompileTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/PipelineContentIndex.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace AgcDriver {

inline std::vector<std::uint64_t> ComputePipelineRecipeFields(const ShaderRecompiler::RecompileResult& shader, bool pushes, std::span<const std::uint32_t> layoutKey) {
    std::vector<std::uint64_t> fields;
    const auto words = shader.spirv.Words();
    fields.push_back(words.size());
    fields.push_back(ShaderCompileTrace::HashOf(words));
    fields.push_back(shader.specialization.size());
    for (const auto& constant : shader.specialization) fields.push_back((static_cast<std::uint64_t>(constant.id) << 32u) | constant.value);
    fields.push_back(shader.bindings.size());
    for (const auto& binding : shader.bindings) {
        fields.push_back(static_cast<std::uint64_t>(binding.kind));
        fields.push_back(static_cast<std::uint64_t>(binding.role));
        fields.push_back((static_cast<std::uint64_t>(binding.descriptorSet) << 32u) | binding.binding);
        fields.push_back(binding.count);
    }
    fields.push_back(shader.hostSubgroupSize);
    fields.push_back(shader.workgroupMemoryDwords);
    fields.push_back(shader.runtimeAbiVersion);
    fields.push_back(shader.bdaAbiVersion);
    fields.push_back(pushes ? 1u : 0u);
    fields.push_back(layoutKey.size());
    fields.push_back(ShaderCompileTrace::HashOf(layoutKey));
    std::uint64_t mixed = 0x9e3779b97f4a7c15ull;
    for (const auto word : words) {
        mixed = (mixed ^ word) * 0xff51afd7ed558ccdull;
        mixed ^= mixed >> 29u;
    }
    fields.push_back(mixed);
    for (const auto word : layoutKey) fields.push_back(word);
    return fields;
}

inline std::uint64_t ComputePipelineRecipeHash(const ShaderRecompiler::RecompileResult& shader, bool pushes, std::span<const std::uint32_t> layoutKey) {
    const auto fields = ComputePipelineRecipeFields(shader, pushes, layoutKey);
    return ShaderCompileTrace::HashOf(std::span<const std::uint64_t>(fields));
}

inline PipelineContentKey ComputePipelineContentKey(const ShaderRecompiler::RecompileResult& shader, bool pushes, std::span<const std::uint32_t> layoutKey) {
    const auto fields = ComputePipelineRecipeFields(shader, pushes, layoutKey);
    return HashPipelineContent(fields);
}

inline void RequireSameComputeRecipe(std::uint64_t built, std::uint64_t demanded, std::uint64_t programAddress) {
    if (built == demanded) return;
    char text[160];
    std::snprintf(text, sizeof(text), "APS5_VERIFY_COMPILE_JOBS: compute pipeline for program 0x%llx was built from a different recipe than the dispatch demands", static_cast<unsigned long long>(programAddress));
    throw std::runtime_error(text);
}

struct ComputePipelineAheadRequest {
    ShaderRecompiler::RecompileResult shader;
    bool pushes = false;
    std::size_t retainedBytes = 0;

    static std::size_t EstimateBytes(const ShaderRecompiler::RecompileResult& source) {
        constexpr std::size_t bound = 64u * 1024u * 1024u;
        const auto count = source.spirv.WordCount();
        if (count > bound / sizeof(std::uint32_t) || source.bindings.size() > bound / sizeof(ShaderRecompiler::DescriptorBinding) || source.specialization.size() > bound / sizeof(ShaderRecompiler::PipelineSpecializationConstant)) return bound + 1;
        return sizeof(ComputePipelineAheadRequest) + 256u + count * sizeof(std::uint32_t) + 2u * source.bindings.size() * sizeof(ShaderRecompiler::DescriptorBinding) + 2u * source.specialization.size() * sizeof(ShaderRecompiler::PipelineSpecializationConstant);
    }

    static std::shared_ptr<const ComputePipelineAheadRequest> Capture(const ShaderRecompiler::RecompileResult& source, std::size_t maxBytes) {
        const auto count = source.spirv.WordCount();
        if (count < 5 || count > maxBytes / sizeof(std::uint32_t) || source.bindings.size() > maxBytes / sizeof(ShaderRecompiler::DescriptorBinding) || source.specialization.size() > maxBytes / sizeof(ShaderRecompiler::PipelineSpecializationConstant)) return nullptr;
        const auto minimum = sizeof(ComputePipelineAheadRequest) + 256u + count * sizeof(std::uint32_t) + source.bindings.size() * sizeof(ShaderRecompiler::DescriptorBinding) + source.specialization.size() * sizeof(ShaderRecompiler::PipelineSpecializationConstant);
        if (minimum > maxBytes) return nullptr;
        auto request = std::make_shared<ComputePipelineAheadRequest>();
        auto& shader = request->shader;
        shader.variantId = source.variantId;
        shader.specializationId = source.specializationId;
        shader.hostSubgroupSize = source.hostSubgroupSize;
        shader.workgroupMemoryDwords = source.workgroupMemoryDwords;
        shader.runtimeAbiVersion = source.runtimeAbiVersion;
        shader.bdaAbiVersion = source.bdaAbiVersion;
        shader.specialization = source.specialization;
        shader.bindings.resize(source.bindings.size());
        for (std::size_t index = 0; index < source.bindings.size(); ++index) {
            const auto& from = source.bindings[index];
            auto& to = shader.bindings[index];
            to.kind = from.kind;
            to.role = from.role;
            to.descriptorSet = from.descriptorSet;
            to.binding = from.binding;
            to.count = from.count;
        }
        request->pushes = !source.pushConstants.empty();
        request->retainedBytes = sizeof(ComputePipelineAheadRequest) + 256u + count * sizeof(std::uint32_t) + shader.bindings.capacity() * sizeof(ShaderRecompiler::DescriptorBinding) + shader.specialization.capacity() * sizeof(ShaderRecompiler::PipelineSpecializationConstant);
        if (request->retainedBytes > maxBytes) return nullptr;
        const auto resolved = source.spirv.Resolved();
        const auto code = resolved.Words();
        if (code.size() != count) throw std::runtime_error("pipeline compile request changed module size");
        auto words = std::shared_ptr<std::uint32_t[]>(new std::uint32_t[count]);
        std::copy(code.begin(), code.end(), words.get());
        std::shared_ptr<const void> owner(words, words.get());
        shader.spirv = ShaderRecompiler::SharedSpirv(std::move(owner), {words.get(), count});
        return request;
    }
};

}

#endif

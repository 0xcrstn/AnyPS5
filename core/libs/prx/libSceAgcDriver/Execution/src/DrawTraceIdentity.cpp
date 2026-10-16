#include "prx/libSceAgcDriver/Execution/include/DrawTraceIdentity.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "TraceIdentity.hpp"
#include <algorithm>
#include <bit>
#include <optional>
#include <stdexcept>
#include <utility>

namespace AgcDriver::DrawTrace {
namespace {

using Builder = ShaderRecompiler::TraceIdentityBuilder;
using Shader = ShaderRecompiler::RecompileResult;

const char* stageName(ShaderRecompiler::ShaderStage stage) {
    switch (stage) {
        case ShaderRecompiler::ShaderStage::Compute: return "compute";
        case ShaderRecompiler::ShaderStage::Vertex: return "vertex";
        case ShaderRecompiler::ShaderStage::TessellationControl: return "tessellation_control";
        case ShaderRecompiler::ShaderStage::TessellationEvaluation: return "tessellation_evaluation";
        case ShaderRecompiler::ShaderStage::Geometry: return "geometry";
        case ShaderRecompiler::ShaderStage::Fragment: return "fragment";
        case ShaderRecompiler::ShaderStage::Local: return "local";
        case ShaderRecompiler::ShaderStage::Mesh: return "mesh";
    }
    throw std::invalid_argument("unknown draw trace shader stage");
}

std::vector<std::uint64_t> shaderFields(const Shader& shader) {
    return ShaderRecompiler::ShaderTraceVariantFields(shader);
}

std::string variantIdentity(ShaderRecompiler::ShaderStage stage, const Shader& shader, std::span<const std::uint64_t> fields) {
    Builder digest;
    digest.Text("anyps5.shader-variant.v1");
    digest.Integer(static_cast<std::uint32_t>(stage));
    digest.Text(shader.spirv.TraceIdentity());
    digest.Text(shader.traceSourceIdentity != nullptr ? shader.traceSourceIdentity->recompileKeyIdentity : "unknown");
    for (const auto value : fields) digest.Integer(value);
    return digest.Finish();
}

void stencil(Builder& digest, VkStencilOpState state) {
    digest.Integer(state.failOp); digest.Integer(state.passOp); digest.Integer(state.depthFailOp);
    digest.Integer(state.compareOp); digest.Integer(state.compareMask); digest.Integer(state.writeMask); digest.Integer(state.reference);
}

void blend(Builder& digest, const VkPipelineColorBlendAttachmentState& state) {
    digest.Integer(state.blendEnable); digest.Integer(state.srcColorBlendFactor); digest.Integer(state.dstColorBlendFactor);
    digest.Integer(state.colorBlendOp); digest.Integer(state.srcAlphaBlendFactor); digest.Integer(state.dstAlphaBlendFactor);
    digest.Integer(state.alphaBlendOp); digest.Integer(state.colorWriteMask);
}

}

Id RegisterBuiltinComputePipelineIdentity(std::string_view kind, std::span<const std::uint32_t> spirv, const BuiltinComputePipelineState& state) {
    if (!Enabled()) return 0;
    if (kind.empty() || spirv.empty() || state.entryPoint.empty()) throw std::invalid_argument("incomplete builtin pipeline identity");
    Builder module;
    module.Text("anyps5.final-spirv.v1");
    module.Words(spirv);
    const auto moduleDigest = module.Finish();
    const auto moduleId = RegisterMetadata("spirv", moduleDigest,
        {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.final-spirv.v1"},
         {"digest_scope", "domain_length_and_little_endian_spirv_words_not_raw_file"}, {"words", std::to_string(spirv.size())}});
    if (moduleId == 0) return 0;
    Builder variant;
    variant.Text("anyps5.builtin-shader-variant.v1");
    variant.Text(kind);
    variant.Text("compute");
    variant.Text(moduleDigest);
    variant.Text(state.entryPoint);
    variant.Integer(state.stageFlags);
    variant.Integer(state.requiredSubgroupSize);
    std::vector<VkSpecializationMapEntry> entries(state.specializationEntries.begin(), state.specializationEntries.end());
    std::ranges::sort(entries, {}, &VkSpecializationMapEntry::constantID);
    variant.Integer(entries.size());
    std::optional<std::uint32_t> previous;
    for (const auto& entry : entries) {
        if ((previous && *previous == entry.constantID) || entry.offset > state.specializationData.size()
            || entry.size > state.specializationData.size() - entry.offset) throw std::invalid_argument("invalid builtin specialization identity");
        previous = entry.constantID;
        variant.Integer(entry.constantID);
        variant.Integer(entry.size);
        variant.Bytes(state.specializationData.subspan(entry.offset, entry.size));
    }
    const auto variantDigest = variant.Finish();
    const auto variantId = RegisterMetadata("shader_variant", variantDigest,
        {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.builtin-shader-variant.v1"}, {"stage", "compute"},
         {"source_kind", "builtin"}, {"builtin_kind", std::string(kind)}, {"entry_point", std::string(state.entryPoint)},
         {"guest_identity", "not_applicable"}, {"compiler_invocation", "not_recorded"}, {"stage_timing", "not_recorded"}}, {moduleId});
    if (variantId == 0) return 0;
    Builder pipeline;
    pipeline.Text("anyps5.builtin-compute-pipeline.v1");
    pipeline.Text(variantDigest);
    pipeline.Integer(state.layoutFlags);
    pipeline.Words(state.descriptorLayout);
    pipeline.Integer(state.pushes.size());
    for (const auto& push : state.pushes) {
        pipeline.Integer(push.stageFlags);
        pipeline.Integer(push.offset);
        pipeline.Integer(push.size);
    }
    pipeline.Integer(state.pipelineFlags);
    return RegisterMetadata("pipeline", pipeline.Finish(),
        {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.builtin-compute-pipeline.v1"}, {"kind", "compute"},
         {"source_kind", "builtin"}, {"builtin_kind", std::string(kind)},
         {"scope", "selected_spirv_entry_specialization_layout_pushes_and_flags_excludes_handles"}}, {variantId});
}

Id RegisterGuestShaderIdentity(const ShaderRecompiler::ShaderBinary& shader) {
    if (!Enabled()) return 0;
    Builder digest;
    digest.Text("anyps5.guest-shader.v1");
    digest.Integer(static_cast<std::uint32_t>(shader.stage));
    digest.Words(shader.code);
    return RegisterMetadata("guest_shader", digest.Finish(), {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.guest-shader.v1"},
                            {"digest_scope", "domain_length_stage_and_little_endian_code_words_not_raw_file"},
                            {"stage", stageName(shader.stage)}, {"code_words", std::to_string(shader.code.size())}, {"owner", "unknown"}});
}

Id RegisterShaderIdentity(ShaderRecompiler::ShaderStage stage, const Shader& shader) {
    if (!Enabled()) return 0;
    const auto index = static_cast<std::uint32_t>(stage);
    if (index >= 8u) throw std::invalid_argument("invalid shader metadata stage");
    if (Current().operation != 0) {
        Note("shader_process_variant_id", shader.variantId);
        Note("shader_process_specialization_id", shader.specializationId);
    }
    auto cache = shader.traceMetadata;
    std::unique_lock<std::mutex> lock;
    std::vector<std::uint64_t> uncachedFields;
    const std::vector<std::uint64_t>* fields = nullptr;
    if (cache != nullptr) {
        lock = std::unique_lock(cache->mutex);
        if (cache->epoch != MetadataEpoch()) { cache->stages.fill(0); cache->epoch = MetadataEpoch(); }
        if (!cache->finalized || cache->module != shader.spirv.TraceCache() || cache->source != shader.traceSourceIdentity) {
            cache->module = shader.spirv.TraceCache(); cache->source = shader.traceSourceIdentity;
            cache->fields = shaderFields(shader); cache->stages.fill(0); cache->finalized = true;
        }
        if (cache->stages[index] != 0) return cache->stages[index];
        fields = &cache->fields;
    } else {
        uncachedFields = shaderFields(shader);
        fields = &uncachedFields;
    }
    std::vector<Id> references;
    const auto source = shader.traceSourceIdentity;
    if (source != nullptr) {
        references.push_back(RegisterMetadata("guest_shader", source->guestIdentity,
            {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.guest-shader.v1"}, {"stage", stageName(static_cast<ShaderRecompiler::ShaderStage>(source->stage))},
             {"digest_scope", "domain_length_stage_and_little_endian_code_words_not_raw_file"},
             {"code_words", std::to_string(source->codeWords)}, {"owner", "unknown"}}));
        references.push_back(RegisterMetadata("compiler_key", source->recompileKeyIdentity,
            {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.recompile-key.v1"}, {"compiler_source_version", std::to_string(source->compilerSourceVersion)},
             {"scope", "recompile_cache_key_not_exact_invocation"}, {"source_binary_binding", "unverified"}}));
    }
    references.push_back(RegisterMetadata("spirv", shader.spirv.TraceIdentity(),
        {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.final-spirv.v1"},
         {"digest_scope", "domain_length_and_little_endian_spirv_words_not_raw_file"}, {"words", std::to_string(shader.spirv.size())}}));
    if (std::ranges::find(references, Id{0}) != references.end()) return 0;
    const auto identity = variantIdentity(stage, shader, *fields);
    const auto id = RegisterMetadata("shader_variant", identity,
        {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.shader-variant.v1"}, {"stage", stageName(stage)},
         {"source_kind", source != nullptr ? "guest" : "generated_or_unknown"}, {"runtime_abi", std::to_string(shader.runtimeAbiVersion)},
         {"bda_abi", std::to_string(shader.bdaAbiVersion)}, {"host_subgroup_size", std::to_string(shader.hostSubgroupSize)},
         {"invocation_data", "not_recorded"}, {"replay_snapshot_relation", "unknown"}}, std::move(references));
    if (cache != nullptr && id != 0) cache->stages[index] = id;
    return id;
}

std::vector<Id> RegisterShaderStages(std::span<const Graphics::CompiledShader> shaders) {
    if (!Enabled()) return {};
    std::vector<Id> result;
    result.reserve(shaders.size());
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) throw std::invalid_argument("missing traced shader stage");
        result.push_back(RegisterShaderIdentity(shader.stage, *shader.program));
    }
    return result;
}

Id RegisterPipelineIdentity(std::string digest, std::span<const Id> stages, bool compute) {
    if (!Enabled()) return 0;
    if (std::ranges::find(stages, Id{0}) != stages.end()) return 0;
    return RegisterMetadata("pipeline", std::move(digest), {{"digest_algorithm", "sha256"},
        {"encoding", compute ? "anyps5.compute-pipeline.v1" : "anyps5.graphics-pipeline.v1"},
        {"kind", compute ? "compute" : "graphics"}, {"scope", "construction_state_excludes_handles_and_guest_payloads"}},
        std::vector<Id>(stages.begin(), stages.end()));
}

std::string ComputePipelineIdentity(const Shader& shader, bool pushes, std::span<const std::uint32_t> layoutKey, std::uint32_t requiredSubgroupSize, std::uint32_t pipelineFlags, std::uint32_t stageFlags) {
    if (!Enabled()) return {};
    Builder digest;
    digest.Text("anyps5.compute-pipeline.v1");
    digest.Text(variantIdentity(ShaderRecompiler::ShaderStage::Compute, shader, shaderFields(shader)));
    digest.Integer(pushes);
    digest.Words(layoutKey);
    digest.Integer(requiredSubgroupSize); digest.Integer(pipelineFlags); digest.Integer(stageFlags);
    return digest.Finish();
}

std::string GraphicsPipelineIdentity(const Graphics::Context& context, const Graphics::State& state, const Graphics::VertexInputLayout& input, const Graphics::PipelineSetLayouts& resources, std::span<const Graphics::CompiledShader> shaders, VkImageLayout attachmentLayout) {
    if (!Enabled()) return {};
    Builder digest;
    digest.Text("anyps5.graphics-pipeline.v1");
    digest.Integer(attachmentLayout);
    digest.Integer(shaders.size());
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) throw std::invalid_argument("missing pipeline shader identity");
        digest.Integer(static_cast<std::uint32_t>(shader.stage));
        digest.Integer(shader.pushConstantOffset);
        digest.Text(variantIdentity(shader.stage, *shader.program, shaderFields(*shader.program)));
    }
    digest.Integer(Graphics::PushConstantStages(shaders));
    digest.Integer(input.bindings.size());
    for (const auto& binding : input.bindings) { digest.Integer(binding.binding); digest.Integer(binding.stride); digest.Integer(binding.inputRate); }
    digest.Integer(input.attributes.size());
    for (const auto& attribute : input.attributes) { digest.Integer(attribute.location); digest.Integer(attribute.binding); digest.Integer(attribute.format); digest.Integer(attribute.offset); }
    digest.Integer(resources.split); digest.Words(resources.mainKey); digest.Words(resources.fragmentKey);
    digest.Integer(state.hasColorTarget); digest.Integer(state.rectList); digest.Integer(state.topology); digest.Integer(state.primitiveRestart);
    digest.Integer(state.cullMode); digest.Integer(state.frontFace); digest.Integer(state.provokingVertexMode); digest.Integer(state.negativeOneToOne);
    digest.Integer(state.depthClamp && context.depthClamp); digest.Integer(state.conservativeRasterization);
    digest.Integer(state.blends.size());
    for (const auto& item : state.blends) blend(digest, item);
    for (const auto item : state.blendConstants) digest.Integer(std::bit_cast<std::uint32_t>(item));
    digest.Integer(state.samples); digest.Integer(state.perSampleShading); digest.Integer(state.customSampleLocations);
    if (state.customSampleLocations) { digest.Integer(state.sampleLocationGrid); for (const auto item : state.sampleLocations) digest.Integer(item); }
    digest.Integer(state.colors.size());
    for (const auto& color : state.colors) { digest.Integer(color.format); digest.Integer(color.exportIndex); digest.Integer(color.componentMapping); }
    digest.Integer(state.depth.has_value());
    if (state.depth) {
        digest.Integer(Graphics::HostDepthFormat(context, state.depth->format)); digest.Integer(state.depthTest); digest.Integer(state.depthWrite); digest.Integer(state.depthCompare);
        digest.Integer(state.depthBoundsTest); digest.Integer(state.depthBias); digest.Integer(state.depthBiasPerFace); digest.Integer(state.stencilTest);
        auto front = state.stencilFront, back = state.stencilBack;
        if (state.stencilReplayReference) front.reference = back.reference = 0;
        digest.Integer(state.stencilReplayReference.has_value()); stencil(digest, front); stencil(digest, back);
    }
    digest.Integer(state.stages.mesh.has_value());
    if (state.stages.mesh) {
        const auto& mesh = *state.stages.mesh;
        digest.Integer(mesh.inputPrimitive); digest.Integer(mesh.primitivesPerGroup); digest.Integer(mesh.verticesPerGroup);
        digest.Integer(mesh.maxVertices); digest.Integer(mesh.maxPrimitives); digest.Integer(mesh.threadsPerGroup);
        digest.Integer(mesh.ldsSizeDwords); digest.Integer(mesh.provokingVertex);
    }
    digest.Integer(state.stages.tessellation.has_value());
    if (state.stages.tessellation) {
        const auto& item = *state.stages.tessellation;
        digest.Integer(item.inputControlPoints); digest.Integer(item.outputControlPoints); digest.Integer(item.domain); digest.Integer(item.partitioning);
        digest.Integer(item.outputTopology); digest.Integer(item.patchesPerGroup); digest.Integer(item.primitiveGroupSize); digest.Integer(item.breakAtInstance);
    }
    digest.Integer(Graphics::HostSampleMask(state));
    digest.Integer(Graphics::ValidateSampleLocations(context, state));
    digest.Integer(context.primitiveListRestart); digest.Integer(context.provokingVertexLast); digest.Integer(context.meshWave32);
    digest.Integer(Graphics::PreRasterizationPushStages(context.meshShader, context.tessellationShader, context.geometryShader));
    digest.Integer(context.graphicsPipelineLibrary); digest.Integer(context.pipelineLinkTimeOptimization); digest.Integer(context.pipelineExecutableInfo);
    return digest.Finish();
}

}

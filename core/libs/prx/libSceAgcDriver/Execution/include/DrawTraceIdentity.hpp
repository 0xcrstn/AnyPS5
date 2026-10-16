#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRAWTRACEIDENTITY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRAWTRACEIDENTITY_HPP

#include "prx/libSceAgcDriver/Execution/include/DrawTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver::Graphics {
struct State;
struct VertexInputLayout;
struct PipelineSetLayouts;
}

namespace AgcDriver::DrawTrace {

struct BuiltinComputePipelineState {
    std::string_view entryPoint = "main";
    std::span<const VkPushConstantRange> pushes;
    std::span<const std::uint32_t> descriptorLayout;
    std::span<const VkSpecializationMapEntry> specializationEntries;
    std::span<const std::byte> specializationData;
    VkPipelineLayoutCreateFlags layoutFlags = 0;
    VkPipelineCreateFlags pipelineFlags = 0;
    VkPipelineShaderStageCreateFlags stageFlags = 0;
    std::uint32_t requiredSubgroupSize = 0;
};

[[nodiscard]] Id RegisterBuiltinComputePipelineIdentity(std::string_view kind, std::span<const std::uint32_t> spirv, const BuiltinComputePipelineState& state);

[[nodiscard]] Id RegisterGuestShaderIdentity(const ShaderRecompiler::ShaderBinary& shader);
[[nodiscard]] Id RegisterShaderIdentity(ShaderRecompiler::ShaderStage stage, const ShaderRecompiler::RecompileResult& shader);
[[nodiscard]] std::vector<Id> RegisterShaderStages(std::span<const Graphics::CompiledShader> shaders);
[[nodiscard]] Id RegisterPipelineIdentity(std::string digest, std::span<const Id> stages, bool compute = false);
[[nodiscard]] std::string ComputePipelineIdentity(const ShaderRecompiler::RecompileResult& shader, bool pushes, std::span<const std::uint32_t> layoutKey, std::uint32_t requiredSubgroupSize = 0, std::uint32_t pipelineFlags = 0, std::uint32_t stageFlags = 0);
[[nodiscard]] std::string GraphicsPipelineIdentity(const Graphics::Context& context, const Graphics::State& state, const Graphics::VertexInputLayout& input, const Graphics::PipelineSetLayouts& resources, std::span<const Graphics::CompiledShader> shaders, VkImageLayout attachmentLayout);

}

#endif

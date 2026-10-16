#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINERECIPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINERECIPE_HPP

#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::Graphics {

inline constexpr std::uint32_t PipelineRecipeVersion = 2;
inline constexpr std::size_t PipelineRecipeMaxBytes = 16u * 1024u * 1024u;

struct PipelineRecipe {
    struct Stage {
        ShaderRecompiler::ShaderStage stage;
        std::uint32_t pushConstantOffset = 0;
        std::shared_ptr<const ShaderRecompiler::RecompileResult> program;
        std::string inputIdentity;
    };
    VkImageLayout attachmentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    std::vector<Stage> stages;
    VertexInputLayout vertexInput;
    std::vector<std::uint32_t> mainLayoutKey;
    std::vector<std::uint32_t> fragmentLayoutKey;
    bool splitSets = false;
    State state{};
};

[[nodiscard]] State CanonicalPipelineState(const State& state);
[[nodiscard]] PipelineRecipe MakePipelineRecipe(const State& state, const VertexInputLayout& vertexInput, const PipelineSetLayouts& resources, std::span<const CompiledShader> shaders, VkImageLayout attachmentLayout);
[[nodiscard]] std::vector<CompiledShader> PipelineRecipeShaders(const PipelineRecipe& recipe);
[[nodiscard]] std::size_t MinimumPipelineRecipeBytes(std::span<const CompiledShader> shaders);
[[nodiscard]] std::size_t MinimumPipelineRecipeBytes(const PipelineRecipe& recipe);
[[nodiscard]] std::vector<std::byte> PipelineShaderIdentity(const CompiledShader& shader);
[[nodiscard]] std::vector<std::byte> PipelineRecipeStateIdentity(const State& state, const VertexInputLayout& vertexInput, const PipelineSetLayouts& resources, VkImageLayout attachmentLayout);
[[nodiscard]] std::vector<std::byte> EncodePipelineRecipe(const PipelineRecipe& recipe);
[[nodiscard]] bool DecodePipelineRecipe(std::span<const std::byte> bytes, PipelineRecipe& recipe);
[[nodiscard]] bool DecodePipelineRecipeInputs(std::span<const std::byte> bytes, PipelineRecipe& recipe);
[[nodiscard]] std::string PipelineRecipeInputIdentity(const PipelineRecipe& recipe);
[[nodiscard]] std::string PipelineRecipeIdentity(std::span<const std::byte> encoded);
[[nodiscard]] std::string PipelineRecipeIdentity(const PipelineRecipe& recipe);

}

#endif

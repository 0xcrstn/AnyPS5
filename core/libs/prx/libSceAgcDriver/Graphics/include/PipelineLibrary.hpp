#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINELIBRARY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINELIBRARY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibraryNotes.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

struct PipelineLibraryKeys {
    std::vector<std::byte> vertexInput;
    std::vector<std::byte> preRasterization;
    std::vector<std::byte> fragmentShader;
    std::vector<std::byte> fragmentOutput;
    std::vector<std::byte> renderPass;
    std::vector<std::byte> layout;
    std::vector<std::byte> preRasterizationLayout;
    std::vector<std::byte> fragmentLayout;
    std::vector<std::byte> preRasterizationShaders;
    std::vector<std::byte> fragmentShaders;
    std::array<std::uint64_t, 6> preRasterizationFields{};
    std::array<std::uint64_t, 3> fragmentFields{};
    std::uint32_t preRasterizationSets = 0;
    std::uint32_t fragmentSets = 0;
};

std::span<const VkDynamicState> PipelineLibraryDynamicStates();

struct OptimizedPipeline {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyPipeline destroy = nullptr;
    std::atomic<VkPipeline> handle{VK_NULL_HANDLE};
    std::atomic<bool> released{false};
    void Release() noexcept;
};

VkPipeline LinkPipelineFromLibraries(const Context& context, const VkGraphicsPipelineCreateInfo& info, const VkPipelineRenderingCreateInfoKHR& rendering, const VkPipelineLayoutCreateInfo& layout, const PipelineLibraryKeys& keys, std::shared_ptr<OptimizedPipeline>* optimized = nullptr, bool partsOnly = false, std::span<VkShaderModule> modules = {});

std::array<std::uint64_t, 4>& ThreadBuiltLibraryParts();

std::vector<LibraryBuildNote>& ThreadLibraryBuildNotes();

void WaitForOptimizedPipelines(VkDevice device);

void ClearPipelineLibraries(VkDevice device);

struct PipelineLibraryCounters {
    std::array<std::uint64_t, 4> built{};
    std::uint64_t linked = 0;
    std::uint64_t optimized = 0;
    std::array<std::uint64_t, 4> shaders{};
    std::array<std::uint64_t, 4> rebuiltForState{};
    std::array<std::uint64_t, 4> rebuiltForLayout{};
    std::array<std::uint64_t, 4> rebuiltForRendering{};
    std::array<std::uint64_t, 6> preRasterizationRebuiltFor{};
    std::array<std::uint64_t, 3> fragmentRebuiltFor{};
};

PipelineLibraryCounters PipelineLibraryCountersOf(VkDevice device);

struct ShaderModuleCounters {
    std::uint64_t modules = 0;
    std::uint64_t spirvBytes = 0;
};

void NoteShaderModule(std::size_t spirvBytes);
ShaderModuleCounters CreatedShaderModules();

}

#endif

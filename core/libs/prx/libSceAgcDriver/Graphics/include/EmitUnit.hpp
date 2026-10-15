#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EMITUNIT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EMITUNIT_HPP

#include "prx/libSceAgcDriver/Graphics/include/CommandSink.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AgcDriver::Graphics {

struct EmitPassBegin {
    bool rendering = false;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRect2D area{};
    std::uint32_t layers = 1;
    std::vector<VkImageView> colorViews;
    VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageView depthView = VK_NULL_HANDLE;
    bool stencil = false;
    bool sampleLocations = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkExtent2D sampleGrid{};
    std::vector<VkSampleLocationEXT> locations;
    std::uint32_t depthAttachment = 0;
    bool depthInitialLocations = false;
};

void RecordPassBegin(const Context& context, VkCommandBuffer commands, const EmitPassBegin& begin, bool secondary);

struct EmitPass {
    EmitPassBegin begin;
    CommandList commands;
    std::vector<std::size_t> drawOffsets;
    std::vector<ChunkDraw> draws;
};

struct EmitSlot {
    std::uint64_t batch = 0;
    std::uint32_t index = 0;
};

struct EmitUnit {
    EmitSlot slot;
    std::vector<EmitPass> passes;
};

}

#endif

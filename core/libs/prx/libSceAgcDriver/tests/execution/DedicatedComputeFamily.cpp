#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueFamilyDispatch.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <cstdio>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
std::string Failure(TAction action) {
    try {
        action();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

struct AsQueue {
    std::uint32_t previous = AgcDriver::GuestMemory::GpuLockThreadTag();
    explicit AsQueue(std::uint32_t queue) { AgcDriver::GuestMemory::TagGpuLockThread(queue); }
    ~AsQueue() { AgcDriver::GuestMemory::TagGpuLockThread(previous); }
    AsQueue(const AsQueue&) = delete;
    AsQueue& operator=(const AsQueue&) = delete;
};

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        auto& graphics = device->RecorderFor(0);
        auto& compute = device->RecorderFor(0x20);
        if (&compute == &graphics || compute.GraphicsCapable()) {
            std::puts("skipped, the device runs no dedicated compute queue family");
            return VulkanTestSkipped;
        }
        Require(graphics.GraphicsCapable(), "the graphics recorder lost its graphics queue");
        const auto& context = compute.GetContext();
        const auto commands = compute.Commands();
        Require(AgcDriver::ComputeFamilyCommands(commands) && !AgcDriver::ComputeFamilyCommands(graphics.Commands()), "the dispatch does not tell the compute family's command buffers from the graphics family's");
        const auto graphicsOnly = [](const std::string& failure) { return failure.find("APS5_QUEUE_TOPOLOGY=dedicated") != std::string::npos && failure.find("compute queue family") != std::string::npos; };
        const VkClearDepthStencilValue depth{1.0f, 0};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        Require(graphicsOnly(Failure([&] { context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, &depth, 1, &range); })), "a depth clear on the compute queue family was not refused");
        const VkImageBlit blit{};
        Require(graphicsOnly(Failure([&] { context.Function<PFN_vkCmdBlitImage>("vkCmdBlitImage")(commands, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST); })), "a blit on the compute queue family was not refused");
        Require(graphicsOnly(Failure([&] { context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE); })), "a graphics pipeline bind on the compute queue family was not refused");
        Require(graphicsOnly(Failure([&] { context.Function<PFN_vkCmdDraw>("vkCmdDraw")(commands, 3, 1, 0, 0); })), "a draw on the compute queue family was not refused");
        VkBufferImageCopy depthCopy{};
        depthCopy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        Require(graphicsOnly(Failure([&] { context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, 1, &depthCopy); })), "a copy into a depth aspect on the compute queue family was not refused");
        const auto before = AgcDriver::QueueFamilyDispatchCounts();
        const VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(graphics.Commands(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        const auto after = AgcDriver::QueueFamilyDispatchCounts();
        Require(after.computeBarriers == before.computeBarriers + 2 && after.narrowedBarriers == before.narrowedBarriers + 2, "barriers with graphics stages on the compute queue family were not narrowed (and only there)");
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 256;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        Require(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &bufferInfo, nullptr, &buffer) == VK_SUCCESS, "a buffer could not be created");
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {16, 16, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkImage image = VK_NULL_HANDLE;
        Require(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &imageInfo, nullptr, &image) == VK_SUCCESS, "an image could not be created");
        context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        const auto created = AgcDriver::QueueFamilyDispatchCounts();
        Require(created.sharedBuffers == after.sharedBuffers + 1 && created.sharedImages == after.sharedImages + 1, "an exclusive buffer or image was not shared by both queue families");
        {
            AsQueue onCompute(0x20);
            Require(AgcDriver::Graphics::Recorder::Current() == &compute && AgcDriver::Graphics::Recorder::GraphicsRecorder() == &graphics, "graphics work from the compute queue does not go to the graphics recorder");
        }
        compute.Sync();
        graphics.Sync();
        std::puts("dedicated compute family: graphics-only commands refused, barriers narrowed, resources shared");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

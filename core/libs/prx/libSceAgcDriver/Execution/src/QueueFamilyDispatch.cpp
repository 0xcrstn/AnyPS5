#include "prx/libSceAgcDriver/Execution/include/QueueFamilyDispatch.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver {

namespace {

constexpr VkPipelineStageFlags GraphicsStages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
constexpr VkAccessFlags GraphicsAccess = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT | VK_ACCESS_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT | VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT | VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
constexpr VkAccessFlags ShaderAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
constexpr VkAccessFlags TransferAccess = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
constexpr VkAccessFlags HostAccess = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
constexpr VkAccessFlags MemoryAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

constexpr std::array<const char*, 40> GraphicsOnlyNames{
    "vkCmdBlitImage", "vkCmdResolveImage", "vkCmdClearDepthStencilImage", "vkCmdClearAttachments",
    "vkCmdBeginRenderPass", "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR", "vkCmdBeginRendering", "vkCmdBeginRenderingKHR",
    "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect", "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount", "vkCmdDrawMeshTasksEXT", "vkCmdDrawMeshTasksIndirectEXT",
    "vkCmdBindVertexBuffers", "vkCmdBindVertexBuffers2EXT", "vkCmdBindIndexBuffer",
    "vkCmdSetViewport", "vkCmdSetScissor", "vkCmdSetLineWidth", "vkCmdSetDepthBias", "vkCmdSetBlendConstants", "vkCmdSetDepthBounds", "vkCmdSetStencilCompareMask", "vkCmdSetStencilWriteMask", "vkCmdSetStencilReference",
    "vkCmdSetCullModeEXT", "vkCmdSetFrontFaceEXT", "vkCmdSetPrimitiveTopologyEXT", "vkCmdSetViewportWithCountEXT", "vkCmdSetScissorWithCountEXT", "vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthCompareOpEXT", "vkCmdSetDepthBoundsTestEnableEXT", "vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilOpEXT",
};

constexpr std::size_t GraphicsOnlyIndex(std::string_view name) {
    for (std::size_t i = 0; i < GraphicsOnlyNames.size(); ++i) {
        if (name == GraphicsOnlyNames[i]) return i;
    }
    throw std::logic_error("not a graphics-only command");
}

struct DeviceDispatch {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr real = nullptr;
    std::array<std::uint32_t, 2> families{};
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkCreateQueryPool createQueryPool = nullptr;
    PFN_vkDestroyQueryPool destroyQueryPool = nullptr;
    PFN_vkCmdPipelineBarrier pipelineBarrier = nullptr;
    PFN_vkCmdWriteTimestamp writeTimestamp = nullptr;
    PFN_vkCmdBeginQuery beginQuery = nullptr;
    PFN_vkCmdBindPipeline bindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets bindDescriptorSets = nullptr;
    PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet = nullptr;
    PFN_vkCmdCopyBufferToImage copyBufferToImage = nullptr;
    std::array<PFN_vkVoidFunction, GraphicsOnlyNames.size()> graphicsOnly{};
};

struct Buffer {
    const DeviceDispatch* device;
    VkCommandPool pool;
    bool compute;
};

struct Dispatch {
    std::shared_mutex mutex;
    std::unordered_map<VkDevice, std::unique_ptr<DeviceDispatch>> devices;
    std::unordered_set<VkCommandPool> computePools;
    std::unordered_map<VkCommandBuffer, Buffer> buffers;
    std::unordered_set<VkQueryPool> graphicsQueries;
    std::atomic<std::uint64_t> sharedImages{0};
    std::atomic<std::uint64_t> sharedBuffers{0};
    std::atomic<std::uint64_t> computeBarriers{0};
    std::atomic<std::uint64_t> narrowedBarriers{0};
};

Dispatch& State() {
    static auto* state = new Dispatch;
    return *state;
}

const DeviceDispatch& DeviceOf(VkDevice device) {
    auto& state = State();
    std::shared_lock lock(state.mutex);
    const auto found = state.devices.find(device);
    if (found == state.devices.end()) throw std::logic_error("queue family dispatch: a call for a device without a dispatch");
    return *found->second;
}

Buffer BufferOf(VkCommandBuffer commands) {
    auto& state = State();
    std::shared_lock lock(state.mutex);
    const auto found = state.buffers.find(commands);
    if (found == state.buffers.end()) throw std::logic_error("queue family dispatch: a command buffer not allocated through the dispatch");
    return found->second;
}

[[noreturn]] void GraphicsOnlyCommand(const char* name) {
    throw std::runtime_error(std::string("APS5_QUEUE_TOPOLOGY=dedicated: ") + name + " needs a graphics queue but was recorded into a command buffer of the compute queue family");
}

template<std::size_t N>
struct FixedName {
    char text[N];
    constexpr FixedName(const char (&name)[N]) { std::copy_n(name, N, text); }
};

template<FixedName Name, typename TFunction>
struct GraphicsOnly;

template<FixedName Name, typename TResult, typename... TArguments>
struct GraphicsOnly<Name, TResult (VKAPI_PTR*)(VkCommandBuffer, TArguments...)> {
    using Function = TResult (VKAPI_PTR*)(VkCommandBuffer, TArguments...);
    static constexpr std::size_t Index = GraphicsOnlyIndex(Name.text);
    static TResult VKAPI_CALL Call(VkCommandBuffer commands, TArguments... arguments) {
        const auto buffer = BufferOf(commands);
        if (buffer.compute) GraphicsOnlyCommand(Name.text);
        return reinterpret_cast<Function>(buffer.device->graphicsOnly[Index])(commands, arguments...);
    }
};

#define APS5_GRAPHICS_ONLY(name) reinterpret_cast<PFN_vkVoidFunction>(&GraphicsOnly<#name, PFN_##name>::Call)

const std::array<PFN_vkVoidFunction, GraphicsOnlyNames.size()> GraphicsOnlyCalls{
    APS5_GRAPHICS_ONLY(vkCmdBlitImage), APS5_GRAPHICS_ONLY(vkCmdResolveImage), APS5_GRAPHICS_ONLY(vkCmdClearDepthStencilImage), APS5_GRAPHICS_ONLY(vkCmdClearAttachments),
    APS5_GRAPHICS_ONLY(vkCmdBeginRenderPass), APS5_GRAPHICS_ONLY(vkCmdBeginRenderPass2), APS5_GRAPHICS_ONLY(vkCmdBeginRenderPass2KHR), APS5_GRAPHICS_ONLY(vkCmdBeginRendering), APS5_GRAPHICS_ONLY(vkCmdBeginRenderingKHR),
    APS5_GRAPHICS_ONLY(vkCmdDraw), APS5_GRAPHICS_ONLY(vkCmdDrawIndexed), APS5_GRAPHICS_ONLY(vkCmdDrawIndirect), APS5_GRAPHICS_ONLY(vkCmdDrawIndexedIndirect), APS5_GRAPHICS_ONLY(vkCmdDrawIndirectCount), APS5_GRAPHICS_ONLY(vkCmdDrawIndexedIndirectCount), APS5_GRAPHICS_ONLY(vkCmdDrawMeshTasksEXT), APS5_GRAPHICS_ONLY(vkCmdDrawMeshTasksIndirectEXT),
    APS5_GRAPHICS_ONLY(vkCmdBindVertexBuffers), APS5_GRAPHICS_ONLY(vkCmdBindVertexBuffers2EXT), APS5_GRAPHICS_ONLY(vkCmdBindIndexBuffer),
    APS5_GRAPHICS_ONLY(vkCmdSetViewport), APS5_GRAPHICS_ONLY(vkCmdSetScissor), APS5_GRAPHICS_ONLY(vkCmdSetLineWidth), APS5_GRAPHICS_ONLY(vkCmdSetDepthBias), APS5_GRAPHICS_ONLY(vkCmdSetBlendConstants), APS5_GRAPHICS_ONLY(vkCmdSetDepthBounds), APS5_GRAPHICS_ONLY(vkCmdSetStencilCompareMask), APS5_GRAPHICS_ONLY(vkCmdSetStencilWriteMask), APS5_GRAPHICS_ONLY(vkCmdSetStencilReference),
    APS5_GRAPHICS_ONLY(vkCmdSetCullModeEXT), APS5_GRAPHICS_ONLY(vkCmdSetFrontFaceEXT), APS5_GRAPHICS_ONLY(vkCmdSetPrimitiveTopologyEXT), APS5_GRAPHICS_ONLY(vkCmdSetViewportWithCountEXT), APS5_GRAPHICS_ONLY(vkCmdSetScissorWithCountEXT), APS5_GRAPHICS_ONLY(vkCmdSetDepthTestEnableEXT), APS5_GRAPHICS_ONLY(vkCmdSetDepthWriteEnableEXT), APS5_GRAPHICS_ONLY(vkCmdSetDepthCompareOpEXT), APS5_GRAPHICS_ONLY(vkCmdSetDepthBoundsTestEnableEXT), APS5_GRAPHICS_ONLY(vkCmdSetStencilTestEnableEXT), APS5_GRAPHICS_ONLY(vkCmdSetStencilOpEXT),
};

#undef APS5_GRAPHICS_ONLY

template<typename TInfo>
TInfo Shared(const TInfo& info, const DeviceDispatch& device) {
    auto shared = info;
    shared.sharingMode = VK_SHARING_MODE_CONCURRENT;
    shared.queueFamilyIndexCount = static_cast<std::uint32_t>(device.families.size());
    shared.pQueueFamilyIndices = device.families.data();
    return shared;
}

VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image) {
    const auto& dispatch = DeviceOf(device);
    if (info == nullptr || info->sharingMode != VK_SHARING_MODE_EXCLUSIVE) return dispatch.createImage(device, info, allocator, image);
    const auto shared = Shared(*info, dispatch);
    State().sharedImages.fetch_add(1, std::memory_order_relaxed);
    return dispatch.createImage(device, &shared, allocator, image);
}

VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
    const auto& dispatch = DeviceOf(device);
    if (info == nullptr || info->sharingMode != VK_SHARING_MODE_EXCLUSIVE) return dispatch.createBuffer(device, info, allocator, buffer);
    const auto shared = Shared(*info, dispatch);
    State().sharedBuffers.fetch_add(1, std::memory_order_relaxed);
    return dispatch.createBuffer(device, &shared, allocator, buffer);
}

VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    const auto& dispatch = DeviceOf(device);
    const auto result = dispatch.allocateCommandBuffers(device, info, buffers);
    if (result != VK_SUCCESS || info == nullptr) return result;
    auto& state = State();
    std::unique_lock lock(state.mutex);
    const bool compute = state.computePools.contains(info->commandPool);
    for (std::uint32_t i = 0; i < info->commandBufferCount; ++i) state.buffers[buffers[i]] = {&dispatch, info->commandPool, compute};
    return result;
}

void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool pool, std::uint32_t count, const VkCommandBuffer* buffers) {
    const auto& dispatch = DeviceOf(device);
    {
        auto& state = State();
        std::unique_lock lock(state.mutex);
        for (std::uint32_t i = 0; i < count; ++i) state.buffers.erase(buffers[i]);
    }
    dispatch.freeCommandBuffers(device, pool, count, buffers);
}

void VKAPI_CALL DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator) {
    const auto& dispatch = DeviceOf(device);
    {
        auto& state = State();
        std::unique_lock lock(state.mutex);
        state.computePools.erase(pool);
        std::erase_if(state.buffers, [&](const auto& entry) { return entry.second.pool == pool; });
    }
    dispatch.destroyCommandPool(device, pool, allocator);
}

VkResult VKAPI_CALL CreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkQueryPool* pool) {
    const auto result = DeviceOf(device).createQueryPool(device, info, allocator, pool);
    if (result == VK_SUCCESS && info != nullptr && (info->queryType == VK_QUERY_TYPE_OCCLUSION || info->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS)) {
        auto& state = State();
        std::unique_lock lock(state.mutex);
        state.graphicsQueries.insert(*pool);
    }
    return result;
}

void VKAPI_CALL DestroyQueryPool(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks* allocator) {
    {
        auto& state = State();
        std::unique_lock lock(state.mutex);
        state.graphicsQueries.erase(pool);
    }
    DeviceOf(device).destroyQueryPool(device, pool, allocator);
}

void VKAPI_CALL PipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags flags, std::uint32_t memoryCount, const VkMemoryBarrier* memory, std::uint32_t bufferCount, const VkBufferMemoryBarrier* buffers, std::uint32_t imageCount, const VkImageMemoryBarrier* images) {
    const auto buffer = BufferOf(commands);
    const auto barrier = buffer.device->pipelineBarrier;
    if (!buffer.compute) {
        barrier(commands, source, destination, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
        return;
    }
    auto& state = State();
    state.computeBarriers.fetch_add(1, std::memory_order_relaxed);
    const auto narrowedSource = ComputeFamilyStages(source, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    const auto narrowedDestination = ComputeFamilyStages(destination, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (narrowedSource == source && narrowedDestination == destination) {
        const auto unchanged = [&](VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) { return ComputeFamilyAccess(sourceAccess, source) == sourceAccess && ComputeFamilyAccess(destinationAccess, destination) == destinationAccess; };
        bool same = true;
        for (std::uint32_t i = 0; same && i < memoryCount; ++i) same = unchanged(memory[i].srcAccessMask, memory[i].dstAccessMask);
        for (std::uint32_t i = 0; same && i < bufferCount; ++i) same = unchanged(buffers[i].srcAccessMask, buffers[i].dstAccessMask);
        for (std::uint32_t i = 0; same && i < imageCount; ++i) same = unchanged(images[i].srcAccessMask, images[i].dstAccessMask);
        if (same) {
            barrier(commands, source, destination, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
            return;
        }
    }
    state.narrowedBarriers.fetch_add(1, std::memory_order_relaxed);
    std::vector<VkMemoryBarrier> memoryCopy(memory, memory + memoryCount);
    std::vector<VkBufferMemoryBarrier> bufferCopy(buffers, buffers + bufferCount);
    std::vector<VkImageMemoryBarrier> imageCopy(images, images + imageCount);
    const auto narrow = [&](auto& item) {
        item.srcAccessMask = ComputeFamilyAccess(item.srcAccessMask, narrowedSource);
        item.dstAccessMask = ComputeFamilyAccess(item.dstAccessMask, narrowedDestination);
    };
    std::ranges::for_each(memoryCopy, narrow);
    std::ranges::for_each(bufferCopy, narrow);
    std::ranges::for_each(imageCopy, narrow);
    barrier(commands, narrowedSource, narrowedDestination, flags, memoryCount, memoryCopy.data(), bufferCount, bufferCopy.data(), imageCount, imageCopy.data());
}

void VKAPI_CALL WriteTimestamp(VkCommandBuffer commands, VkPipelineStageFlagBits stage, VkQueryPool pool, std::uint32_t query) {
    const auto buffer = BufferOf(commands);
    if (buffer.compute && (stage & GraphicsStages) != 0) stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    buffer.device->writeTimestamp(commands, stage, pool, query);
}

void VKAPI_CALL BeginQuery(VkCommandBuffer commands, VkQueryPool pool, std::uint32_t query, VkQueryControlFlags flags) {
    const auto buffer = BufferOf(commands);
    if (buffer.compute) {
        auto& state = State();
        std::shared_lock lock(state.mutex);
        if (state.graphicsQueries.contains(pool)) GraphicsOnlyCommand("vkCmdBeginQuery of an occlusion or pipeline statistics query");
    }
    buffer.device->beginQuery(commands, pool, query, flags);
}

void VKAPI_CALL BindPipeline(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipeline pipeline) {
    const auto buffer = BufferOf(commands);
    if (buffer.compute && point == VK_PIPELINE_BIND_POINT_GRAPHICS) GraphicsOnlyCommand("vkCmdBindPipeline of a graphics pipeline");
    buffer.device->bindPipeline(commands, point, pipeline);
}

void VKAPI_CALL BindDescriptorSets(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipelineLayout layout, std::uint32_t first, std::uint32_t count, const VkDescriptorSet* sets, std::uint32_t dynamicCount, const std::uint32_t* dynamicOffsets) {
    const auto buffer = BufferOf(commands);
    if (buffer.compute && point == VK_PIPELINE_BIND_POINT_GRAPHICS) GraphicsOnlyCommand("vkCmdBindDescriptorSets at the graphics bind point");
    buffer.device->bindDescriptorSets(commands, point, layout, first, count, sets, dynamicCount, dynamicOffsets);
}

void VKAPI_CALL PushDescriptorSet(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipelineLayout layout, std::uint32_t set, std::uint32_t count, const VkWriteDescriptorSet* writes) {
    const auto buffer = BufferOf(commands);
    if (buffer.compute && point == VK_PIPELINE_BIND_POINT_GRAPHICS) GraphicsOnlyCommand("vkCmdPushDescriptorSetKHR at the graphics bind point");
    buffer.device->pushDescriptorSet(commands, point, layout, set, count, writes);
}

void VKAPI_CALL CopyBufferToImage(VkCommandBuffer commands, VkBuffer source, VkImage image, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions) {
    const auto buffer = BufferOf(commands);
    if (buffer.compute) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if ((regions[i].imageSubresource.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) GraphicsOnlyCommand("vkCmdCopyBufferToImage into a depth or stencil aspect");
        }
    }
    buffer.device->copyBufferToImage(commands, source, image, layout, count, regions);
}

template<typename TFunction>
void Load(const DeviceDispatch& dispatch, TFunction& slot, const char* name) {
    slot = reinterpret_cast<TFunction>(dispatch.real(dispatch.device, name));
}

PFN_vkVoidFunction VKAPI_CALL DeviceProc(VkDevice device, const char* name) {
    const auto& dispatch = DeviceOf(device);
    if (name == nullptr) return dispatch.real(device, name);
    struct Wrapped {
        const char* name;
        PFN_vkVoidFunction function;
        const void* loaded;
    };
    const std::array wrapped{
        Wrapped{"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(&CreateImage), reinterpret_cast<const void*>(dispatch.createImage)},
        Wrapped{"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(&CreateBuffer), reinterpret_cast<const void*>(dispatch.createBuffer)},
        Wrapped{"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(&AllocateCommandBuffers), reinterpret_cast<const void*>(dispatch.allocateCommandBuffers)},
        Wrapped{"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(&FreeCommandBuffers), reinterpret_cast<const void*>(dispatch.freeCommandBuffers)},
        Wrapped{"vkDestroyCommandPool", reinterpret_cast<PFN_vkVoidFunction>(&DestroyCommandPool), reinterpret_cast<const void*>(dispatch.destroyCommandPool)},
        Wrapped{"vkCreateQueryPool", reinterpret_cast<PFN_vkVoidFunction>(&CreateQueryPool), reinterpret_cast<const void*>(dispatch.createQueryPool)},
        Wrapped{"vkDestroyQueryPool", reinterpret_cast<PFN_vkVoidFunction>(&DestroyQueryPool), reinterpret_cast<const void*>(dispatch.destroyQueryPool)},
        Wrapped{"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(&PipelineBarrier), reinterpret_cast<const void*>(dispatch.pipelineBarrier)},
        Wrapped{"vkCmdWriteTimestamp", reinterpret_cast<PFN_vkVoidFunction>(&WriteTimestamp), reinterpret_cast<const void*>(dispatch.writeTimestamp)},
        Wrapped{"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(&BeginQuery), reinterpret_cast<const void*>(dispatch.beginQuery)},
        Wrapped{"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(&BindPipeline), reinterpret_cast<const void*>(dispatch.bindPipeline)},
        Wrapped{"vkCmdBindDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(&BindDescriptorSets), reinterpret_cast<const void*>(dispatch.bindDescriptorSets)},
        Wrapped{"vkCmdPushDescriptorSetKHR", reinterpret_cast<PFN_vkVoidFunction>(&PushDescriptorSet), reinterpret_cast<const void*>(dispatch.pushDescriptorSet)},
        Wrapped{"vkCmdCopyBufferToImage", reinterpret_cast<PFN_vkVoidFunction>(&CopyBufferToImage), reinterpret_cast<const void*>(dispatch.copyBufferToImage)},
    };
    for (const auto& entry : wrapped) {
        if (std::strcmp(entry.name, name) == 0) return entry.loaded != nullptr ? entry.function : nullptr;
    }
    for (std::size_t i = 0; i < GraphicsOnlyNames.size(); ++i) {
        if (std::strcmp(GraphicsOnlyNames[i], name) == 0) return dispatch.graphicsOnly[i] != nullptr ? GraphicsOnlyCalls[i] : nullptr;
    }
    return dispatch.real(device, name);
}

}

PFN_vkGetDeviceProcAddr InstallQueueFamilyDispatch(VkDevice device, PFN_vkGetDeviceProcAddr real, std::uint32_t graphicsFamily, std::uint32_t computeFamily) {
    if (device == VK_NULL_HANDLE || real == nullptr) throw std::invalid_argument("queue family dispatch: a device and its vkGetDeviceProcAddr are required");
    if (graphicsFamily == computeFamily) throw std::invalid_argument("queue family dispatch: the compute family must differ from the graphics family");
    auto dispatch = std::make_unique<DeviceDispatch>();
    dispatch->device = device;
    dispatch->real = real;
    dispatch->families = {graphicsFamily, computeFamily};
    Load(*dispatch, dispatch->createImage, "vkCreateImage");
    Load(*dispatch, dispatch->createBuffer, "vkCreateBuffer");
    Load(*dispatch, dispatch->allocateCommandBuffers, "vkAllocateCommandBuffers");
    Load(*dispatch, dispatch->freeCommandBuffers, "vkFreeCommandBuffers");
    Load(*dispatch, dispatch->destroyCommandPool, "vkDestroyCommandPool");
    Load(*dispatch, dispatch->createQueryPool, "vkCreateQueryPool");
    Load(*dispatch, dispatch->destroyQueryPool, "vkDestroyQueryPool");
    Load(*dispatch, dispatch->pipelineBarrier, "vkCmdPipelineBarrier");
    Load(*dispatch, dispatch->writeTimestamp, "vkCmdWriteTimestamp");
    Load(*dispatch, dispatch->beginQuery, "vkCmdBeginQuery");
    Load(*dispatch, dispatch->bindPipeline, "vkCmdBindPipeline");
    Load(*dispatch, dispatch->bindDescriptorSets, "vkCmdBindDescriptorSets");
    Load(*dispatch, dispatch->pushDescriptorSet, "vkCmdPushDescriptorSetKHR");
    Load(*dispatch, dispatch->copyBufferToImage, "vkCmdCopyBufferToImage");
    for (std::size_t i = 0; i < GraphicsOnlyNames.size(); ++i) dispatch->graphicsOnly[i] = real(device, GraphicsOnlyNames[i]);
    auto& state = State();
    std::unique_lock lock(state.mutex);
    if (state.devices.contains(device)) throw std::logic_error("queue family dispatch: installed twice for one device");
    state.devices.emplace(device, std::move(dispatch));
    return &DeviceProc;
}

void RemoveQueueFamilyDispatch(VkDevice device) {
    auto& state = State();
    std::unique_lock lock(state.mutex);
    const auto found = state.devices.find(device);
    if (found == state.devices.end()) return;
    const auto* dispatch = found->second.get();
    std::erase_if(state.buffers, [&](const auto& entry) { return entry.second.device == dispatch; });
    state.devices.erase(found);
}

void RegisterComputeFamilyPool(VkCommandPool pool) {
    auto& state = State();
    std::unique_lock lock(state.mutex);
    if (state.devices.empty()) throw std::logic_error("queue family dispatch: a compute pool registered without a dispatch");
    state.computePools.insert(pool);
}

bool ComputeFamilyCommands(VkCommandBuffer commands) {
    auto& state = State();
    std::shared_lock lock(state.mutex);
    const auto found = state.buffers.find(commands);
    return found != state.buffers.end() && found->second.compute;
}

VkPipelineStageFlags ComputeFamilyStages(VkPipelineStageFlags stages, VkPipelineStageFlags empty) {
    const auto kept = stages & ~GraphicsStages;
    return kept != 0 ? kept : empty;
}

VkAccessFlags ComputeFamilyAccess(VkAccessFlags access, VkPipelineStageFlags stages) {
    VkAccessFlags allowed = MemoryAccess;
    if ((stages & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) != 0) allowed |= ~GraphicsAccess;
    if ((stages & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) != 0) allowed |= ShaderAccess;
    if ((stages & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0) allowed |= TransferAccess;
    if ((stages & VK_PIPELINE_STAGE_HOST_BIT) != 0) allowed |= HostAccess;
    if ((stages & VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT) != 0) allowed |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    return access & allowed & ~GraphicsAccess;
}

QueueFamilyDispatchStatistics QueueFamilyDispatchCounts() {
    auto& state = State();
    return {state.sharedImages.load(std::memory_order_relaxed), state.sharedBuffers.load(std::memory_order_relaxed), state.computeBarriers.load(std::memory_order_relaxed), state.narrowedBarriers.load(std::memory_order_relaxed)};
}

}

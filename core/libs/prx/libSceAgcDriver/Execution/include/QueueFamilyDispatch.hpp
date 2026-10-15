#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUEFAMILYDISPATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUEFAMILYDISPATCH_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <cstdint>

namespace AgcDriver {

PFN_vkGetDeviceProcAddr InstallQueueFamilyDispatch(VkDevice device, PFN_vkGetDeviceProcAddr real, std::uint32_t graphicsFamily, std::uint32_t computeFamily);
void RemoveQueueFamilyDispatch(VkDevice device);
void RegisterComputeFamilyPool(VkCommandPool pool);
bool ComputeFamilyCommands(VkCommandBuffer commands);
VkPipelineStageFlags ComputeFamilyStages(VkPipelineStageFlags stages, VkPipelineStageFlags empty);
VkAccessFlags ComputeFamilyAccess(VkAccessFlags access, VkPipelineStageFlags stages);

struct QueueFamilyDispatchStatistics {
    std::uint64_t sharedImages = 0;
    std::uint64_t sharedBuffers = 0;
    std::uint64_t computeBarriers = 0;
    std::uint64_t narrowedBarriers = 0;
};
QueueFamilyDispatchStatistics QueueFamilyDispatchCounts();

}

#endif

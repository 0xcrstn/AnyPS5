#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINELINKOPTIMIZATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINELINKOPTIMIZATION_HPP

#include <vulkan/vulkan.h>

namespace AgcDriver {

inline bool PipelineLinkTimeOptimizationUsable(VkDriverId driver) {
    return driver != VK_DRIVER_ID_NVIDIA_PROPRIETARY;
}

inline VkPipelineCreateFlags PipelineLibraryCreateFlags(bool linkTimeOptimization) {
    return VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (linkTimeOptimization ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0u);
}

}

#endif

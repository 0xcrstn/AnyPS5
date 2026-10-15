#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATIONTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATIONTRACE_HPP

#include "CompileTrace.hpp"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver::Graphics {

class PipelineCreationTrace {
public:
    PipelineCreationTrace(std::string_view phase, bool feedbackSupported, std::uint32_t stageCount) : event(phase), feedback(feedbackSupported && event.Active()) {
        if (!event.Active()) return;
        event.Flag("feedback_supported", feedbackSupported);
        if (!feedback) return;
        stages.resize(stageCount);
        info.pPipelineCreationFeedback = &pipeline;
        info.pipelineStageCreationFeedbackCount = stageCount;
        info.pPipelineStageCreationFeedbacks = stages.empty() ? nullptr : stages.data();
    }

    PipelineCreationTrace(const PipelineCreationTrace&) = delete;
    PipelineCreationTrace& operator=(const PipelineCreationTrace&) = delete;

    template<typename TCreateInfo>
    void Chain(TCreateInfo& create) {
        if (!feedback) return;
        info.pNext = create.pNext;
        create.pNext = &info;
    }

    template<typename TCreateInfo>
    void Unchain(TCreateInfo& create) {
        if (!feedback || create.pNext != &info) return;
        create.pNext = info.pNext;
        info.pNext = nullptr;
    }

    ShaderCompileTrace::Event& Event() {
        return event;
    }

    void Created(VkResult result) {
        if (!event.Active()) return;
        event.Number("vk_result", static_cast<std::uint64_t>(static_cast<std::int64_t>(result)));
        if (result != VK_SUCCESS) event.Fail("vkCreate*Pipelines returned an error");
        if (!feedback) return;
        record("driver", pipeline);
        for (std::size_t index = 0; index < stages.size(); ++index) record("driver_stage" + std::to_string(index), stages[index]);
    }

private:
    void record(const std::string& prefix, const VkPipelineCreationFeedback& value) {
        const bool valid = (value.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0;
        event.Flag(prefix + "_valid", valid);
        if (!valid) return;
        event.Flag(prefix + "_cache_hit", (value.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0)
            .Flag(prefix + "_base_acceleration", (value.flags & VK_PIPELINE_CREATION_FEEDBACK_BASE_PIPELINE_ACCELERATION_BIT) != 0)
            .Real(prefix + "_us", static_cast<double>(value.duration) / 1000.0);
    }

    ShaderCompileTrace::Event event;
    bool feedback = false;
    VkPipelineCreationFeedback pipeline{};
    std::vector<VkPipelineCreationFeedback> stages;
    VkPipelineCreationFeedbackCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
};

}

#endif

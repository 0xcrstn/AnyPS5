#include "prx/libSceAgcDriver/Execution/include/QueueTopology.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueFamilyDispatch.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AgcDriver;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
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

constexpr std::uint32_t Graphics = 0x1u, Compute = 0x2u, Transfer = 0x4u, Sparse = 0x8u;

const std::vector<QueueFamilyInfo> Discrete{{Graphics | Compute | Transfer | Sparse, 16, true}, {Transfer | Sparse, 2, false}, {Compute | Transfer | Sparse, 8, false}};
const std::vector<QueueFamilyInfo> SingleQueue{{Graphics | Compute | Transfer, 1, true}};
const std::vector<QueueFamilyInfo> ComputeSharesSparse{{Graphics | Compute | Transfer, 4, true}, {Compute | Sparse, 1, false}};

}

int main() {
    Expect(ParseQueueTopology(nullptr) == QueueTopology::Serial && ParseQueueTopology("same-family") == QueueTopology::SameFamily && ParseQueueTopology("dedicated") == QueueTopology::Dedicated, "topologies parse");
    Expect(!Failure([] { ParseQueueTopology("two"); }).empty(), "an unknown topology throws");

    const auto serial = PlanQueueTopology(QueueTopology::Serial, Discrete, 0, 1);
    Expect(serial.computeFamily == 0xffffffffu && serial.queuesFromGraphicsFamily == 1, "the serial topology adds no queue");

    const auto same = PlanQueueTopology(QueueTopology::SameFamily, Discrete, 0, 1);
    Expect(same.computeFamily == 0 && same.computeIndex == 1 && same.queuesFromGraphicsFamily == 2, "same-family takes the graphics family's second queue");
    Expect(Failure([] { PlanQueueTopology(QueueTopology::SameFamily, SingleQueue, 0, 0xffffffffu); }).find("only one queue") != std::string::npos, "same-family on a one-queue family fails explicitly");

    const auto dedicated = PlanQueueTopology(QueueTopology::Dedicated, Discrete, 0, 1);
    Expect(dedicated.computeFamily == 2 && dedicated.computeIndex == 0, "dedicated picks the compute-only family");
    Expect(Failure([] { PlanQueueTopology(QueueTopology::Dedicated, SingleQueue, 0, 0xffffffffu); }).find("no compute-only queue family") != std::string::npos, "dedicated without a compute family fails explicitly");
    Expect(!Failure([] { PlanQueueTopology(QueueTopology::Dedicated, ComputeSharesSparse, 0, 1); }).empty(), "dedicated does not take the only queue of the sparse family");
    const auto shared = PlanQueueTopology(QueueTopology::Dedicated, ComputeSharesSparse, 0, 0xffffffffu);
    Expect(shared.computeFamily == 1 && shared.computeIndex == 0, "dedicated uses a compute family the sparse queue does not occupy");

    Expect(DefaultQueueTopology(Discrete, 0, 1) == QueueTopology::Dedicated, "a compute-only family makes dedicated the default");
    Expect(DefaultQueueTopology(SingleQueue, 0, 0xffffffffu) == QueueTopology::Serial, "a single graphics queue keeps the serial default");
    Expect(DefaultQueueTopology(ComputeSharesSparse, 0, 1) == QueueTopology::SameFamily, "a compute family the sparse queue fills falls back to same-family");
    const std::vector<QueueFamilyInfo> GraphicsOnly{{Graphics | Compute | Transfer, 2, true}};
    Expect(DefaultQueueTopology(GraphicsOnly, 0, 0xffffffffu) == QueueTopology::SameFamily, "two graphics queues default to same-family");

    Expect(ComputeFamilyStages(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, "graphics stages leave a compute barrier");
    Expect(ComputeFamilyStages(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, "a barrier of graphics stages only keeps the given empty stage");
    Expect(ComputeFamilyStages(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, "all commands stays");
    Expect(ComputeFamilyAccess(VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) == VK_ACCESS_SHADER_WRITE_BIT, "attachment access leaves a compute barrier");
    Expect(ComputeFamilyAccess(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT) == VK_ACCESS_TRANSFER_READ_BIT, "shader access without a shader stage is dropped");
    Expect(ComputeFamilyAccess(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT), "all commands keeps every compute-queue access");
    Expect(ComputeFamilyAccess(VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) == VK_ACCESS_MEMORY_READ_BIT, "memory access stays with any stage");
    Expect(!ComputeFamilyCommands(reinterpret_cast<VkCommandBuffer>(std::uintptr_t{0x1000})), "an unknown command buffer is not of the compute family");

    Expect(DescribeQueueFamilies(Discrete).find("2: 8 queues compute transfer sparse") != std::string::npos, "families are described with their flags");
    Expect(!Failure([] { PlanQueueTopology(QueueTopology::Serial, Discrete, 7, 1); }).empty(), "a graphics family out of range throws");

    if (failures != 0) return 1;
    std::puts("queue topology tests passed");
    return 0;
}

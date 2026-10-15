#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUETOPOLOGY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUETOPOLOGY_HPP

#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver {

enum class QueueTopology : std::uint8_t { Serial, SameFamily, Dedicated };

struct QueueFamilyInfo {
    std::uint32_t flags = 0;
    std::uint32_t count = 0;
    bool present = false;
};

struct TopologyPlan {
    QueueTopology topology = QueueTopology::Serial;
    std::uint32_t computeFamily = 0xffffffffu;
    std::uint32_t computeIndex = 0;
    std::uint32_t queuesFromGraphicsFamily = 1;
};

QueueTopology ParseQueueTopology(const char* value);
const char* QueueTopologyName(QueueTopology topology);
std::string DescribeQueueFamilies(std::span<const QueueFamilyInfo> families);
TopologyPlan PlanQueueTopology(QueueTopology topology, std::span<const QueueFamilyInfo> families, std::uint32_t graphicsFamily, std::uint32_t sparseFamily);
QueueTopology DefaultQueueTopology(std::span<const QueueFamilyInfo> families, std::uint32_t graphicsFamily, std::uint32_t sparseFamily);

}

#endif

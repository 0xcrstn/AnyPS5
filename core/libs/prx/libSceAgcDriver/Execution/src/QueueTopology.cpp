#include "prx/libSceAgcDriver/Execution/include/QueueTopology.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace AgcDriver {

namespace {

constexpr std::uint32_t GraphicsBit = 0x1u;
constexpr std::uint32_t ComputeBit = 0x2u;
constexpr std::uint32_t TransferBit = 0x4u;
constexpr std::uint32_t SparseBit = 0x8u;

}

QueueTopology ParseQueueTopology(const char* value) {
    if (value == nullptr || std::strcmp(value, "serial") == 0) return QueueTopology::Serial;
    if (std::strcmp(value, "same-family") == 0) return QueueTopology::SameFamily;
    if (std::strcmp(value, "dedicated") == 0) return QueueTopology::Dedicated;
    throw std::runtime_error(std::string("APS5_QUEUE_TOPOLOGY: unknown topology ") + value + " (serial, same-family or dedicated)");
}

const char* QueueTopologyName(QueueTopology topology) {
    switch (topology) {
        case QueueTopology::Serial: return "serial";
        case QueueTopology::SameFamily: return "same-family";
        case QueueTopology::Dedicated: return "dedicated";
    }
    return "?";
}

std::string DescribeQueueFamilies(std::span<const QueueFamilyInfo> families) {
    std::string text;
    for (std::size_t i = 0; i < families.size(); ++i) {
        const auto& family = families[i];
        char part[96];
        std::snprintf(part, sizeof(part), "%s%zu: %u queues%s%s%s%s%s", i == 0 ? "" : "; ", i, family.count, (family.flags & GraphicsBit) != 0 ? " graphics" : "", (family.flags & ComputeBit) != 0 ? " compute" : "", (family.flags & TransferBit) != 0 ? " transfer" : "", (family.flags & SparseBit) != 0 ? " sparse" : "", family.present ? " present" : "");
        text += part;
    }
    return text;
}

TopologyPlan PlanQueueTopology(QueueTopology topology, std::span<const QueueFamilyInfo> families, std::uint32_t graphicsFamily, std::uint32_t sparseFamily) {
    if (graphicsFamily >= families.size()) throw std::logic_error("queue topology: the graphics family is out of range");
    TopologyPlan plan;
    plan.topology = topology;
    if (topology == QueueTopology::Serial) return plan;
    if (topology == QueueTopology::SameFamily) {
        if (families[graphicsFamily].count < 2) throw std::runtime_error("APS5_QUEUE_TOPOLOGY=same-family: graphics family " + std::to_string(graphicsFamily) + " exposes only one queue (" + DescribeQueueFamilies(families) + ")");
        plan.computeFamily = graphicsFamily;
        plan.computeIndex = 1;
        plan.queuesFromGraphicsFamily = 2;
        return plan;
    }
    for (std::uint32_t i = 0; i < families.size(); ++i) {
        const auto& family = families[i];
        if (i == graphicsFamily || family.count == 0 || (family.flags & ComputeBit) == 0 || (family.flags & GraphicsBit) != 0) continue;
        if (i == sparseFamily && family.count < 2) continue;
        plan.computeFamily = i;
        plan.computeIndex = i == sparseFamily ? 1 : 0;
        return plan;
    }
    throw std::runtime_error("APS5_QUEUE_TOPOLOGY=dedicated: no compute-only queue family with a free queue (" + DescribeQueueFamilies(families) + ")");
}

}

#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitObservation.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LabelVersions.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t Segments = 5;
constexpr std::array<const char*, Segments> SegmentNames{"producer not recorded", "recorded not submitted", "submitted not device-ready", "device-ready not host-published", "published not seen"};
constexpr double BlockedMs = 0.05;

struct ChainStats {
    std::array<std::uint64_t, 7> bySource{};
    std::uint64_t blocked = 0;
    double blockedMs = 0;
    std::array<double, Segments> segmentMs{};
    std::uint64_t unknown = 0;
    double unknownMs = 0;
    double longestMs = 0;
    std::uint64_t longestStamp = 0;
    std::uint32_t longestProducer = 0xffffffffu;
};

ChainStats& Chain() {
    static thread_local ChainStats stats;
    return stats;
}

std::int64_t Ns(std::chrono::steady_clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count();
}

}

std::string DescribeObservation(const WaitObservation& observation) {
    constexpr std::array<const char*, 8> functions{"always", "<", "<=", "==", "!=", ">=", ">", "never"};
    char text[512];
    std::snprintf(text, sizeof(text), "queue 0x%x waits (0x%llx & 0x%llx) %s 0x%llx over %u bytes, received %llu, active from %llu, discharged at %llu by %s with value 0x%llx",
                  observation.queue, static_cast<unsigned long long>(observation.address), static_cast<unsigned long long>(observation.mask), functions[observation.function & 7u], static_cast<unsigned long long>(observation.reference), observation.bytes,
                  static_cast<unsigned long long>(observation.received), static_cast<unsigned long long>(observation.activation), static_cast<unsigned long long>(observation.discharged), WaitSourceNames[static_cast<std::size_t>(observation.source)], static_cast<unsigned long long>(observation.value));
    std::string description = text;
    if (observation.producerQueue != 0xffffffffu) {
        std::snprintf(text, sizeof(text), " stored by queue 0x%x at stamp %llu", observation.producerQueue, static_cast<unsigned long long>(observation.producerStamp));
        description += text;
    }
    return description;
}

WaitObservation& Driver::lastObservation() {
    static thread_local WaitObservation observation;
    return observation;
}

void Driver::noteWaitChain(const WaitObservation& observation, std::span<const std::uint32_t> packet) {
    if (!Graphics::LabelVersions::Enabled()) return;
    lastObservation() = observation;
    auto& stats = Chain();
    ++stats.bySource[static_cast<std::size_t>(observation.source)];
    const auto activated = Ns(observation.activatedAt);
    const auto discharged = Ns(observation.dischargedAt);
    const double waitedMs = static_cast<double>(discharged - activated) / 1e6;
    if (waitedMs < BlockedMs) return;
    ++stats.blocked;
    stats.blockedMs += waitedMs;
    std::uint64_t producer = observation.producerStamp;
    if (producer == 0 && observation.source == WaitSource::Memory) {
        if (const auto match = storedSince(packet, observation.address, observation.bytes, observation.activation)) producer = match->stamp;
    }
    const auto version = producer != 0 ? Graphics::LabelVersions::Find(producer) : std::nullopt;
    if (!version.has_value()) {
        ++stats.unknown;
        stats.unknownMs += waitedMs;
        return;
    }
    if (waitedMs > stats.longestMs) {
        stats.longestMs = waitedMs;
        stats.longestStamp = producer;
        stats.longestProducer = version->queue;
    }
    using Graphics::LabelVersions::Milestone;
    const bool batched = version->Reached(Milestone::Submitted);
    std::array<std::int64_t, Segments + 1> bounds{};
    bounds[0] = activated;
    bounds[1] = version->At(Milestone::Recorded);
    bounds[2] = batched ? version->At(Milestone::Submitted) : bounds[1];
    bounds[3] = batched ? version->At(Milestone::DeviceReady) : bounds[1];
    bounds[4] = batched ? version->At(Milestone::HostPublished) : bounds[1];
    bounds[5] = discharged;
    std::int64_t cursor = activated;
    for (std::size_t segment = 0; segment < Segments; ++segment) {
        const auto end = bounds[segment + 1] == 0 ? discharged : std::clamp(bounds[segment + 1], cursor, discharged);
        stats.segmentMs[segment] += static_cast<double>(end - cursor) / 1e6;
        cursor = end;
    }
}

void Driver::reportWaitChain(std::uint32_t queue) {
    auto& driver = Get();
    if (queue == 0) {
        auto& scheduler = driver.Scheduler();
        if (scheduler.Active()) {
            const auto taken = scheduler.Take();
            constexpr const char* modes[] = {"off", "serial", "dag"};
            AgcDriver::ProfilePrint_nid_no_patch("[scheduler] (10 s) mode %s%s: %llu turns, %llu handoffs, %.1f ms waiting for a turn, %llu blocked waits released the turn, %llu parks (%llu settled at arm) parked %.1f ms\n", modes[static_cast<std::size_t>(scheduler.Mode())], scheduler.Verifying() ? " verify" : "",
                                                 static_cast<unsigned long long>(taken.turns), static_cast<unsigned long long>(taken.handoffs), taken.turnWaitMs, static_cast<unsigned long long>(taken.blocks), static_cast<unsigned long long>(taken.parks), static_cast<unsigned long long>(taken.parksSettledAtArm), taken.parkedMs);
        }
    }
    if (!Graphics::LabelVersions::Enabled()) return;
    auto& stats = Chain();
    const auto& outcomes = waitOutcomes();
    std::string segments;
    for (std::size_t segment = 0; segment < Segments; ++segment) {
        char part[96];
        std::snprintf(part, sizeof(part), "%s%s %.1f ms", segment == 0 ? "" : ", ", SegmentNames[segment], stats.segmentMs[segment]);
        segments += part;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[waitchain] queue 0x%x (10 s): waits by source entry %llu, held at submit %llu, history %llu, table %llu, table-late %llu, memory %llu; %llu history matches below the activation floor refused; %llu landings ended on settled writes; blocked %llu waits %.1f ms: %s; unknown producer %llu waits %.1f ms; longest %.1f ms on stamp %llu by queue 0x%x; lifts latched %llu, authoritative %llu, producer %llu, exact %llu, %llu unproven refused; broker wakes %llu, timeouts %llu\n",
                                         queue, static_cast<unsigned long long>(stats.bySource[1]), static_cast<unsigned long long>(stats.bySource[2]), static_cast<unsigned long long>(stats.bySource[3]), static_cast<unsigned long long>(stats.bySource[4]), static_cast<unsigned long long>(stats.bySource[5]), static_cast<unsigned long long>(stats.bySource[6]),
                                         static_cast<unsigned long long>(outcomes.belowFloor), static_cast<unsigned long long>(outcomes.landedSettled), static_cast<unsigned long long>(stats.blocked), stats.blockedMs, segments.c_str(), static_cast<unsigned long long>(stats.unknown), stats.unknownMs, stats.longestMs, static_cast<unsigned long long>(stats.longestStamp), stats.longestProducer,
                                         static_cast<unsigned long long>(outcomes.lifts[0]), static_cast<unsigned long long>(outcomes.lifts[1]), static_cast<unsigned long long>(outcomes.lifts[2]), static_cast<unsigned long long>(outcomes.lifts[3]), static_cast<unsigned long long>(outcomes.unprovenRefused), static_cast<unsigned long long>(outcomes.brokerWakes), static_cast<unsigned long long>(outcomes.brokerTimeouts));
    if (queue == 0) AgcDriver::ProfilePrint_nid_no_patch("[labels] (10 s) %s; batch-age submits %llu of %llu checks\n", Graphics::LabelVersions::TakeReport().c_str(), static_cast<unsigned long long>(driver.batchAgeSubmits.exchange(0)), static_cast<unsigned long long>(driver.batchAgeChecks.exchange(0)));
    stats = ChainStats{};
}

}

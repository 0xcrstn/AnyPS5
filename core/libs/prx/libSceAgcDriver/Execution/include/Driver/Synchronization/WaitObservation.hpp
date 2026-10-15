#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATION_WAITOBSERVATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATION_WAITOBSERVATION_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace AgcDriver::DriverDetail {

enum class WaitSource : std::uint8_t { None = 0, Entry, HeldAtSubmit, History, Table, TableLate, Memory };

inline constexpr const char* WaitSourceNames[7] = {"none", "entry", "held-at-submit", "history", "table", "table-late", "memory"};

enum class WaitFloorMode : std::uint8_t { Activation = 0, Submission };

struct StoredMatch {
    std::uint64_t stamp = 0;
    std::uint64_t value = 0;
    std::uint32_t queue = 0xffffffffu;
};

struct WaitObservation {
    std::uint64_t address = 0;
    std::uint32_t bytes = 0;
    std::uint32_t function = 0;
    std::uint64_t reference = 0;
    std::uint64_t mask = 0;
    std::uint32_t queue = 0;
    std::uint64_t received = 0;
    std::uint64_t activation = 0;
    std::uint64_t discharged = 0;
    std::uint64_t value = 0;
    std::uint32_t producerQueue = 0xffffffffu;
    std::uint64_t producerStamp = 0;
    WaitSource source = WaitSource::None;
    std::chrono::steady_clock::time_point activatedAt{};
    std::chrono::steady_clock::time_point dischargedAt{};

    bool CrossQueue() const { return producerQueue != 0xffffffffu && producerQueue != queue; }
    bool FromRecordedStore() const { return source == WaitSource::History || source == WaitSource::Table || source == WaitSource::TableLate; }
};

inline std::uint64_t WaitActivationStamp(WaitFloorMode mode, std::uint64_t received, std::uint64_t floor) {
    return mode == WaitFloorMode::Submission ? received : std::max(received, floor);
}

inline bool ObservationProven(const WaitObservation& observation, std::uint64_t floor) {
    if (!observation.FromRecordedStore()) return observation.source != WaitSource::None;
    if (observation.source == WaitSource::TableLate) return true;
    return observation.producerStamp > std::max(observation.received, floor);
}

std::string DescribeObservation(const WaitObservation& observation);

}

#endif

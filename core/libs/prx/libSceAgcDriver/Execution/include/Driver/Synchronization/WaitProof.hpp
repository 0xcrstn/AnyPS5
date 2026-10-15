#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATION_WAITPROOF_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATION_WAITPROOF_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitObservation.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

enum class Lift : std::uint8_t { Latched = 0, Authoritative, Producer, Exact };

inline constexpr const char* LiftNames[4] = {"latched", "authoritative", "producer", "exact"};

enum class LabelTimelineMode : std::uint8_t { Off, On, Verify };

struct ProofInput {
    WaitSource source = WaitSource::None;
    std::uint32_t function = 0;
    std::uint64_t reference = 0;
    std::uint64_t mask = 0;
    std::uint32_t waitBytes = 0;
    std::uint64_t value = 0;
    std::uint32_t storeBytes = 0;
    std::uint32_t waitingQueue = 0;
    std::uint32_t producerQueue = 0xffffffffu;
    std::uint64_t producerStamp = 0;
    std::uint64_t activation = 0;
};

struct Proof {
    Lift lift = Lift::Exact;
    const char* reason = "";
};

inline LabelTimelineMode ParseLabelTimeline(const char* value) {
    if (value == nullptr || std::string(value) == "on") return LabelTimelineMode::On;
    if (std::string(value) == "off") return LabelTimelineMode::Off;
    if (std::string(value) == "verify") return LabelTimelineMode::Verify;
    throw std::runtime_error(std::string("APS5_LABEL_TIMELINE: unknown mode ") + value + " (off, on or verify)");
}

inline bool CompareMasked(std::uint32_t function, std::uint64_t value, std::uint64_t reference, std::uint64_t mask) {
    const auto masked = value & mask;
    switch (function) {
        case 0: return true;
        case 1: return masked < reference;
        case 2: return masked <= reference;
        case 3: return masked == reference;
        case 4: return masked != reference;
        case 5: return masked >= reference;
        case 6: return masked > reference;
        default: throw std::runtime_error("WAIT_REG_MEM compare function " + std::to_string(function) + " has no known semantics");
    }
}

inline Proof Prove(const ProofInput& input) {
    if (input.waitBytes != 4 && input.waitBytes != 8) throw std::runtime_error("WAIT_REG_MEM over " + std::to_string(input.waitBytes) + " bytes has no known semantics");
    if (input.source == WaitSource::None) throw std::logic_error("a wait proof needs the source of its observation");
    const bool satisfied = CompareMasked(input.function, input.value, input.reference, input.mask);
    if (!satisfied) return {Lift::Exact, "the observed value does not satisfy the predicate"};
    switch (input.source) {
        case WaitSource::Entry:
        case WaitSource::Memory:
        case WaitSource::HeldAtSubmit:
            return {Lift::Latched, "the predicate held in guest memory while the wait was active"};
        case WaitSource::TableLate:
            return {Lift::Authoritative, "the newest recorded value, unchanged by any CPU or GPU write since"};
        default:
            break;
    }
    if (input.storeBytes < input.waitBytes) return {Lift::Exact, "the producing store covers only part of the awaited bytes"};
    if (input.producerStamp <= input.activation) return {Lift::Exact, "the producing store was recorded before the wait became active"};
    return {Lift::Producer, "a store recorded after the wait became active, covering every awaited byte"};
}

}

#endif

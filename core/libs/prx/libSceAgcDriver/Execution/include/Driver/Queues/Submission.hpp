#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SUBMISSION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SUBMISSION_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/DrawTrace.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::GuestMemory {
class DriverStoreWatch;
}

namespace AgcDriver::DriverDetail {

struct RegisterListSnapshot {
    std::vector<std::uint32_t> values;
    std::shared_ptr<GuestMemory::DriverStoreWatch> writes;

    static RegisterListSnapshot Capture(std::span<const std::uint32_t> packet);
    std::vector<std::uint32_t> Resolve(std::span<const std::uint32_t> packet) const;
};

struct Submission {
    std::uint64_t serial;
    std::uint32_t queue;
    std::vector<std::uint32_t> commands;

    std::shared_ptr<const ShaderRegistry> shaders;
    std::map<std::size_t, std::shared_ptr<IFlipRequest>> flips;
    std::map<std::size_t, std::shared_ptr<IRenderingWait>> renderingWaits;
    std::map<std::size_t, std::size_t> conditionalEnds;
    std::map<std::size_t, RegisterListSnapshot> registerLists;
    bool suspend = false;
    bool waitFree = false;
    bool holdsFlip = false;

    std::uint64_t received = 0;
    std::vector<std::uint64_t> labelWrites;
    std::set<std::size_t> heldAtSubmit;
    std::chrono::steady_clock::time_point receivedAt{};
    std::chrono::steady_clock::time_point copiedAt{};
    std::chrono::steady_clock::time_point validatedAt{};
    std::chrono::steady_clock::time_point roomReadyAt{};
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::chrono::steady_clock::time_point dequeuedAt{};
    std::chrono::steady_clock::time_point orderedAt{};
    const std::uint32_t* rewindTail = nullptr;
    std::size_t rewindWords = 0;
    DrawTrace::Context traceContext;
    DrawTrace::JobToken traceJob;
    std::map<std::size_t, DrawTrace::Context> tracePackets;
    bool traceJobManagedByWorker = false;
    void InitializeTrace(DrawTrace::Context origin = {});
    DrawTrace::Context TraceAt(std::size_t offset) const;
};

struct QueueWorker {
    std::deque<Submission> pending;

    std::atomic<std::uint64_t> queued{0};
    std::unordered_map<std::uint64_t, std::uint32_t> unfinishedWrites;
    std::thread thread;
};

}

#endif

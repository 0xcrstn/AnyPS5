#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEASUREMENTWORKLOAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEASUREMENTWORKLOAD_HPP

#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace AgcDriver {

class MeasurementWorkload {
public:
    enum class Kind { Draw, Dispatch, IndirectRecord, HostDraw, HostDispatch, Job };
    enum class Outcome { Complete, Skipped, Rejected, Failed, Cancelled };
    static constexpr std::size_t MaximumIntervals = 65536;
    inline static constexpr std::array<std::string_view, 15> Names{
        "requested_draw_operations", "dispatch_operations", "indirect_record_operations",
        "host_draw_commands", "host_dispatch_commands", "operation_status.complete",
        "operation_status.skipped", "operation_status.rejected", "operation_status.failed",
        "operation_status.cancelled", "host_commands_completed", "host_commands_failed",
        "jobs_started", "jobs_completed", "jobs_failed"};
    using Counts = std::array<std::uint64_t, Names.size()>;
    using Pending = std::array<std::uint64_t, 3>;
    static bool Increment(std::uint64_t& value) noexcept {
        if (value == std::numeric_limits<std::uint64_t>::max()) return false;
        ++value;
        return true;
    }
    struct Row {
        std::uint64_t present = 0;
        std::uint64_t beginNs = 0;
        std::uint64_t endNs = 0;
        Counts counts{};
        Pending pendingAtBegin{};
        Pending pendingAtEnd{};
    };
    struct Snapshot {
        std::vector<Row> frames;
        Counts counts{};
        Pending pendingAtOpen{};
        Pending pendingAtClose{};
        std::uint64_t openPresent = 0;
        std::uint64_t closePresent = 0;
        std::uint64_t openNs = 0;
        std::uint64_t closeNs = 0;
        std::uint64_t lost = 0;
        bool overflow = false;
        bool closed = false;
        bool lifetimeError = false;
    };

    explicit MeasurementWorkload(std::size_t intervals) : capacity(intervals) {
        if (intervals == 0 || intervals > MaximumIntervals) throw std::invalid_argument("measurement workload interval bound exceeded");
        frames.reserve(intervals);
    }

    void Begin(Kind kind) {
        std::lock_guard lock(mutex);
        increment(pending[pendingIndex(kind)]);
        if (!recording) return;
        add(counterIndex(kind));
    }

    void End(Kind kind, Outcome outcome) {
        std::lock_guard lock(mutex);
        auto& active = pending[pendingIndex(kind)];
        if (active == 0) lifetimeError = true;
        else --active;
        if (!recording) return;
        if (kind == Kind::Job) add(outcome == Outcome::Complete ? 13 : 14);
        else if (kind == Kind::HostDraw || kind == Kind::HostDispatch) add(outcome == Outcome::Complete ? 10 : 11);
        else add(5 + static_cast<std::size_t>(outcome));
    }

    void Open(std::uint64_t present, std::uint64_t ns) {
        std::lock_guard lock(mutex);
        if (opened || closed) { lifetimeError = true; return; }
        opened = recording = true;
        openPresent = present;
        openNs = ns;
        pendingAtOpen = pending;
        row = {present, ns, 0, {}, pending, {}};
    }

    void Boundary(std::uint64_t present, std::uint64_t ns, bool close) {
        std::lock_guard lock(mutex);
        if (!recording) { lifetimeError = true; return; }
        if (present != row.present + 1 || present == 0 || ns <= row.beginNs) lifetimeError = true;
        row.endNs = ns;
        row.pendingAtEnd = pending;
        if (frames.size() == capacity) increment(lost);
        else frames.push_back(row);
        if (close) {
            recording = false;
            closed = true;
            closePresent = present;
            closeNs = ns;
            pendingAtClose = pending;
        } else row = {present, ns, 0, {}, pending, {}};
    }

    Snapshot Collect() const {
        std::lock_guard lock(mutex);
        return {frames, totals, pendingAtOpen, pendingAtClose, openPresent, closePresent,
                openNs, closeNs, lost, overflow, closed, lifetimeError};
    }

private:
    static std::size_t pendingIndex(Kind kind) {
        return kind == Kind::Job ? 2 : kind == Kind::HostDraw || kind == Kind::HostDispatch ? 1 : 0;
    }
    static std::size_t counterIndex(Kind kind) {
        switch (kind) {
        case Kind::Draw: return 0;
        case Kind::Dispatch: return 1;
        case Kind::IndirectRecord: return 2;
        case Kind::HostDraw: return 3;
        case Kind::HostDispatch: return 4;
        case Kind::Job: return 12;
        }
        return 0;
    }
    void increment(std::uint64_t& value) {
        if (!Increment(value)) overflow = true;
    }
    void add(std::size_t index) {
        increment(row.counts[index]);
        increment(totals[index]);
    }

    const std::size_t capacity;
    mutable std::mutex mutex;
    std::vector<Row> frames;
    Row row;
    Counts totals{};
    Pending pending{};
    Pending pendingAtOpen{};
    Pending pendingAtClose{};
    std::uint64_t openPresent = 0;
    std::uint64_t closePresent = 0;
    std::uint64_t openNs = 0;
    std::uint64_t closeNs = 0;
    std::uint64_t lost = 0;
    bool opened = false;
    bool recording = false;
    bool closed = false;
    bool overflow = false;
    bool lifetimeError = false;
};

}

#endif

#include "prx/libSceAgcDriver/Execution/include/QueueScheduler.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace AgcDriver {

namespace {

double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}

SchedulerMode QueueScheduler::ParseMode(const char* value) {
    if (value == nullptr || std::strcmp(value, "off") == 0) return SchedulerMode::Off;
    if (std::strcmp(value, "serial") == 0) return SchedulerMode::Serial;
    if (std::strcmp(value, "dag") == 0) return SchedulerMode::Dag;
    throw std::runtime_error(std::string("APS5_QUEUE_SCHEDULER: unknown mode ") + value + " (off, serial or dag)");
}

SchedulerMode QueueScheduler::ConfiguredMode() {
    return ParseMode(std::getenv("APS5_QUEUE_SCHEDULER"));
}

bool QueueScheduler::ConfiguredVerify() {
    const char* value = std::getenv("APS5_QUEUE_SCHEDULER_VERIFY");
    return value != nullptr && std::strcmp(value, "0") != 0;
}

void QueueScheduler::fail(const std::string& what) const {
    throw std::logic_error("APS5_QUEUE_SCHEDULER_VERIFY: " + what);
}

void QueueScheduler::rethrowClosedLocked() const {
    if (isClosed) std::rethrow_exception(closed);
}

bool QueueScheduler::grantableLocked(std::uint32_t queue) const {
    if (holder != NoQueue) return false;
    const auto& self = streams.at(queue);
    for (const auto& [other, stream] : streams) {
        if (other == queue || !stream.requesting) continue;
        if (stream.received < self.received || (stream.received == self.received && other < queue)) return false;
    }
    return true;
}

void QueueScheduler::acquireLocked(std::unique_lock<std::mutex>& lock, std::uint32_t queue) {
    auto& stream = streams[queue];
    if (holder == queue) {
        ++stream.depth;
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    stream.requesting = true;
    changed.wait(lock, [&] { return isClosed || grantableLocked(queue); });
    stream.requesting = false;
    if (isClosed) {
        changed.notify_all();
        std::rethrow_exception(closed);
    }
    if (verify && holder != NoQueue) fail("queue 0x" + std::to_string(queue) + " was granted the serial turn while queue 0x" + std::to_string(holder) + " holds it");
    holder = queue;
    stream.depth = 1;
    if (lastHolder != queue) ++statistics.handoffs;
    lastHolder = queue;
    statistics.turnWaitMs += MsSince(start);
}

void QueueScheduler::releaseLocked(std::uint32_t queue) {
    if (holder != queue) return;
    auto& stream = streams[queue];
    if (stream.depth > 1) {
        --stream.depth;
        return;
    }
    stream.depth = 0;
    holder = NoQueue;
    changed.notify_all();
}

QueueScheduler::Turn QueueScheduler::Begin(std::uint32_t queue, std::uint64_t submission, std::size_t cursor, std::uint64_t received) {
    if (!Active()) return {};
    std::unique_lock lock(mutex);
    rethrowClosedLocked();
    auto& stream = streams[queue];
    if (verify) {
        if (stream.parked) fail("queue 0x" + std::to_string(queue) + " began a packet while it is parked");
        if (stream.started && (submission < stream.submission || (submission == stream.submission && cursor <= stream.cursor)))
            fail("queue 0x" + std::to_string(queue) + " began submission " + std::to_string(submission) + " dword " + std::to_string(cursor) + " after submission " + std::to_string(stream.submission) + " dword " + std::to_string(stream.cursor));
    }
    stream.submission = submission;
    stream.cursor = cursor;
    stream.started = true;
    stream.received = received;
    ++statistics.turns;
    if (mode != SchedulerMode::Serial) return Turn(this, queue);
    acquireLocked(lock, queue);
    return Turn(this, queue);
}

void QueueScheduler::endTurn(std::uint32_t queue) noexcept {
    if (mode != SchedulerMode::Serial) return;
    std::lock_guard lock(mutex);
    releaseLocked(queue);
}

QueueScheduler::Turn::~Turn() {
    if (scheduler != nullptr) scheduler->endTurn(queue);
}

QueueScheduler::Blocked QueueScheduler::Block(std::uint32_t queue) {
    if (mode != SchedulerMode::Serial) return {};
    std::lock_guard lock(mutex);
    if (holder != queue) return {};
    auto& stream = streams[queue];
    const auto depth = stream.depth;
    stream.depth = 0;
    holder = NoQueue;
    ++statistics.blocks;
    changed.notify_all();
    return Blocked(this, queue, depth);
}

void QueueScheduler::reacquire(std::uint32_t queue, std::uint32_t depth) noexcept {
    std::unique_lock lock(mutex);
    if (isClosed) return;
    try {
        acquireLocked(lock, queue);
    } catch (...) {
        return;
    }
    streams[queue].depth = depth;
}

QueueScheduler::Blocked::~Blocked() {
    if (scheduler != nullptr) scheduler->reacquire(queue, depth);
}

void QueueScheduler::Park(ParkedOp op) {
    if (!op.arm || !op.resume) throw std::invalid_argument("a parked op needs an arm and a resume action");
    std::uint32_t depth = 0;
    {
        std::lock_guard lock(mutex);
        rethrowClosedLocked();
        auto& stream = streams[op.queue];
        if (verify && stream.parked) fail("queue 0x" + std::to_string(op.queue) + " parked " + op.what + " while already parked");
        stream.parked = true;
        stream.woken = false;
        ++statistics.parks;
        if (mode == SchedulerMode::Serial && holder == op.queue) {
            depth = stream.depth;
            stream.depth = 0;
            holder = NoQueue;
            changed.notify_all();
        }
    }
    const auto queue = op.queue;
    const auto wake = [this, queue] {
        {
            std::lock_guard lock(mutex);
            streams[queue].woken = true;
        }
        changed.notify_all();
    };
    const auto start = std::chrono::steady_clock::now();
    bool armed = false;
    try {
        armed = op.arm(wake);
    } catch (...) {
        std::lock_guard lock(mutex);
        streams[queue].parked = false;
        throw;
    }
    {
        std::unique_lock lock(mutex);
        auto& stream = streams[queue];
        if (!armed) {
            ++statistics.parksSettledAtArm;
            stream.woken = true;
        }
        changed.wait(lock, [&] { return stream.woken || isClosed; });
        stream.parked = false;
        statistics.parkedMs += MsSince(start);
        if (!stream.woken) std::rethrow_exception(closed);
        if (depth != 0) {
            acquireLocked(lock, queue);
            stream.depth = depth;
        }
    }
    op.resume();
}

void QueueScheduler::Close(std::exception_ptr reason) {
    {
        std::lock_guard lock(mutex);
        if (isClosed) return;
        isClosed = true;
        closed = reason != nullptr ? std::move(reason) : std::make_exception_ptr(std::runtime_error("the queue scheduler was closed"));
    }
    changed.notify_all();
}

std::uint32_t QueueScheduler::Holder() const {
    std::lock_guard lock(mutex);
    return holder;
}

QueueScheduler::Statistics QueueScheduler::Take() {
    std::lock_guard lock(mutex);
    const auto taken = statistics;
    statistics = Statistics{};
    return taken;
}

}

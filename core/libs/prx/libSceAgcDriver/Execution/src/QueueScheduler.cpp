#include "prx/libSceAgcDriver/Execution/include/QueueScheduler.hpp"
#include <chrono>
#include <cinttypes>
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
    if (value == nullptr || std::strcmp(value, "dag") == 0) return SchedulerMode::Dag;
    if (std::strcmp(value, "off") == 0) return SchedulerMode::Off;
    if (std::strcmp(value, "serial") == 0) return SchedulerMode::Serial;
    if (std::strcmp(value, "replay") == 0) return SchedulerMode::Replay;
    throw std::runtime_error(std::string("APS5_QUEUE_SCHEDULER: unknown mode ") + value + " (off, serial, dag or replay)");
}

ScheduleRecording QueueScheduler::ConfiguredRecording() {
    ScheduleRecording recording;
    if (const char* capture = std::getenv("APS5_QUEUE_SCHEDULER_CAPTURE")) recording.capture = capture;
    if (const char* replay = std::getenv("APS5_QUEUE_SCHEDULER_REPLAY")) recording.replay = replay;
    return recording;
}

QueueScheduler::QueueScheduler(SchedulerMode mode, bool verify, const ScheduleRecording& recording) : mode(mode), verify(verify) {
    if (mode == SchedulerMode::Replay) {
        if (recording.replay.empty()) throw std::runtime_error("APS5_QUEUE_SCHEDULER=replay needs APS5_QUEUE_SCHEDULER_REPLAY=<capture file>");
        loadReplay(recording.replay);
    } else if (!recording.replay.empty()) {
        throw std::runtime_error("APS5_QUEUE_SCHEDULER_REPLAY is set, but APS5_QUEUE_SCHEDULER is not replay");
    }
    if (!recording.capture.empty()) {
        if (recording.capture == recording.replay) throw std::runtime_error("APS5_QUEUE_SCHEDULER_CAPTURE would overwrite the capture being replayed");
        capture = std::fopen(recording.capture.c_str(), "w");
        if (capture == nullptr) throw std::runtime_error("APS5_QUEUE_SCHEDULER_CAPTURE: cannot open " + recording.capture);
        std::fputs("aps5-queue-schedule 1\n", capture);
    }
}

QueueScheduler::~QueueScheduler() {
    if (capture != nullptr) std::fclose(capture);
}

void QueueScheduler::loadReplay(const std::string& path) {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.c_str(), "r"), &std::fclose);
    if (!file) throw std::runtime_error("APS5_QUEUE_SCHEDULER_REPLAY: cannot open " + path);
    char line[160];
    if (std::fgets(line, sizeof(line), file.get()) == nullptr || std::strcmp(line, "aps5-queue-schedule 1\n") != 0) throw std::runtime_error("APS5_QUEUE_SCHEDULER_REPLAY: " + path + " is not a queue schedule capture");
    std::size_t number = 1;
    while (std::fgets(line, sizeof(line), file.get()) != nullptr) {
        ++number;
        char kind = 0;
        std::uint32_t queue = 0;
        std::uint64_t index = 0;
        std::uint64_t cursor = 0;
        std::uint64_t value = 0;
        std::uint32_t source = 0;
        if (std::sscanf(line, "T %" SCNu32 " %" SCNu64 " %" SCNu64, &queue, &index, &cursor) == 3 && line[0] == 'T') {
            replayTurns.emplace_back(queue, index, static_cast<std::size_t>(cursor));
            continue;
        }
        if (std::sscanf(line, "%c %" SCNu32 " %" SCNu64 " %" SCNu64 " %" SCNx64 " %" SCNu32, &kind, &queue, &index, &cursor, &value, &source) == 6 && kind == 'W') {
            replayWaits[{queue, index, static_cast<std::size_t>(cursor)}] = {value, source};
            continue;
        }
        throw std::runtime_error("APS5_QUEUE_SCHEDULER_REPLAY: " + path + " line " + std::to_string(number) + " is malformed");
    }
}

std::string QueueScheduler::describe(const Position& position) const {
    char text[96];
    std::snprintf(text, sizeof(text), "queue 0x%x submission %" PRIu64 " dword %zu", std::get<0>(position), std::get<1>(position), std::get<2>(position));
    return text;
}

void QueueScheduler::captureTurnLocked(const Stream& stream, std::uint32_t queue) {
    if (capture == nullptr) return;
    std::fprintf(capture, "T %" PRIu32 " %" PRIu64 " %zu\n", queue, stream.index, stream.cursor);
    ++statistics.capturedTurns;
}

void QueueScheduler::NoteWait(std::uint32_t queue, std::uint64_t value, std::uint32_t source) {
    if (capture == nullptr && mode != SchedulerMode::Replay) return;
    std::lock_guard lock(mutex);
    const auto found = streams.find(queue);
    if (found == streams.end() || !found->second.started) fail("a wait on queue 0x" + std::to_string(queue) + " was noted outside a scheduler turn");
    const Position position{queue, found->second.index, found->second.cursor};
    if (capture != nullptr) {
        std::fprintf(capture, "W %" PRIu32 " %" PRIu64 " %zu %" PRIx64 " %" PRIu32 "\n", queue, std::get<1>(position), std::get<2>(position), value, source);
        ++statistics.capturedWaits;
    }
    if (mode != SchedulerMode::Replay) return;
    ++statistics.replayedWaits;
    const auto recorded = replayWaits.find(position);
    if (recorded != replayWaits.end() && recorded->second.first == value) return;
    ++statistics.replayMismatches;
    if (!verify) return;
    char text[160];
    if (recorded == replayWaits.end()) std::snprintf(text, sizeof(text), "observed 0x%" PRIx64 " (source %" PRIu32 "), the capture has no wait there", value, source);
    else std::snprintf(text, sizeof(text), "observed 0x%" PRIx64 " (source %" PRIu32 "), the capture observed 0x%" PRIx64 " (source %" PRIu32 ")", value, source, recorded->second.first, recorded->second.second);
    throw std::logic_error("APS5_QUEUE_SCHEDULER_REPLAY: the wait at " + describe(position) + " " + text);
}

void QueueScheduler::Flush() {
    std::lock_guard lock(mutex);
    if (capture != nullptr) std::fflush(capture);
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
    if (mode == SchedulerMode::Replay && replayNext < replayTurns.size()) return !self.beginning || std::get<0>(replayTurns[replayNext]) == queue;
    for (const auto& [other, stream] : streams) {
        if (other == queue || !stream.requesting) continue;
        if (stream.received < self.received || (stream.received == self.received && other < queue)) return false;
    }
    return true;
}

void QueueScheduler::acquireLocked(std::unique_lock<std::mutex>& lock, std::uint32_t queue, bool begin) {
    auto& stream = streams[queue];
    if (holder == queue) {
        ++stream.depth;
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    stream.requesting = true;
    stream.beginning = begin;
    if (mode == SchedulerMode::Replay) {
        if (!changed.wait_for(lock, std::chrono::seconds(10), [&] { return isClosed || grantableLocked(queue); })) {
            stream.requesting = false;
            fail("the replay waited 10 s at " + describe({queue, stream.index, stream.cursor}) + " for the captured turn " + (replayNext < replayTurns.size() ? describe(replayTurns[replayNext]) : std::string("(none)")));
        }
    } else {
        changed.wait(lock, [&] { return isClosed || grantableLocked(queue); });
    }
    stream.requesting = false;
    stream.beginning = false;
    if (isClosed) {
        changed.notify_all();
        std::rethrow_exception(closed);
    }
    if (begin && mode == SchedulerMode::Replay && replayNext < replayTurns.size()) {
        const Position here{queue, stream.index, stream.cursor};
        if (here != replayTurns[replayNext]) fail("the replay reached " + describe(here) + " where the capture had " + describe(replayTurns[replayNext]));
        ++replayNext;
        ++statistics.replayedTurns;
    }
    if (begin) captureTurnLocked(stream, queue);
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
    if (!serialOrder() && !verify && capture == nullptr && !closedFlag.load(std::memory_order_acquire)) {
        concurrentTurns.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    std::unique_lock lock(mutex);
    rethrowClosedLocked();
    auto& stream = streams[queue];
    if (verify) {
        if (stream.parked) fail("queue 0x" + std::to_string(queue) + " began a packet while it is parked");
        if (stream.started && (submission < stream.submission || (submission == stream.submission && cursor <= stream.cursor)))
            fail("queue 0x" + std::to_string(queue) + " began submission " + std::to_string(submission) + " dword " + std::to_string(cursor) + " after submission " + std::to_string(stream.submission) + " dword " + std::to_string(stream.cursor));
    }
    if (!stream.started || submission != stream.submission) ++stream.index;
    stream.submission = submission;
    stream.cursor = cursor;
    stream.started = true;
    stream.received = received;
    ++statistics.turns;
    if (!serialOrder()) {
        captureTurnLocked(stream, queue);
        return Turn(this, queue);
    }
    acquireLocked(lock, queue, true);
    return Turn(this, queue);
}

void QueueScheduler::endTurn(std::uint32_t queue) noexcept {
    if (!serialOrder()) return;
    std::lock_guard lock(mutex);
    releaseLocked(queue);
}

QueueScheduler::Turn::~Turn() {
    if (scheduler != nullptr) scheduler->endTurn(queue);
}

QueueScheduler::Blocked QueueScheduler::Block(std::uint32_t queue) {
    if (!serialOrder()) return {};
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
        if (serialOrder() && holder == op.queue) {
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
        if (capture != nullptr) std::fflush(capture);
        if (isClosed) return;
        isClosed = true;
        closedFlag.store(true, std::memory_order_release);
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
    auto taken = statistics;
    taken.turns += concurrentTurns.exchange(0, std::memory_order_relaxed);
    statistics = Statistics{};
    return taken;
}

}

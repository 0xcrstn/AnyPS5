#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/RecordQueue.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

struct Driver::DrawRecordResult {
    std::uint64_t ticket = 0;
    DrawRecordEffects effects;
    std::shared_ptr<const DrawRecipe> built;
    std::uint64_t recipeKey = 0;
    std::vector<std::shared_ptr<DispatchVariant>> recipeStages;
};

namespace {

constexpr std::uint32_t RecordWorkerTag = 0x100;

RecordQueue& drawRecords() {
    static auto* queue = new RecordQueue;
    return *queue;
}

struct DrawRecordCounters {
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> dispatches{0};
    std::atomic<std::uint64_t> overlapped{0};
    std::atomic<std::uint64_t> stalePending{0};
    std::atomic<std::uint64_t> staleStored{0};
    std::atomic<std::uint64_t> staleUnchecked{0};
    std::atomic<std::uint64_t> redos{0};
    std::atomic<std::uint64_t> packetWaits{0};
    std::atomic<std::uint64_t> aheadPosts{0};
    std::atomic<std::uint64_t> aheadFull{0};
    std::atomic<std::uint64_t> aheadDrained{0};
    std::atomic<std::uint64_t> aheadStalePost{0};
    std::atomic<std::uint64_t> aheadStaleRun{0};
    std::atomic<std::uint64_t> aheadMaxDepth{0};
};

DrawRecordCounters& drawRecordCounters() {
    static DrawRecordCounters counters;
    return counters;
}

void noteAheadDepth(std::uint64_t depth) {
    auto& deepest = drawRecordCounters().aheadMaxDepth;
    auto seen = deepest.load(std::memory_order_relaxed);
    while (seen < depth && !deepest.compare_exchange_weak(seen, depth, std::memory_order_relaxed)) {
    }
}

std::vector<PreparedRead> preparedReads(std::span<const Graphics::GuestMemorySnapshot> reads) {
    std::vector<PreparedRead> prepared;
    prepared.reserve(reads.size());
    for (const auto& read : reads) prepared.push_back({read.address, read.bytes.size()});
    return prepared;
}

bool pendingWriteOverlaps(std::uint64_t address, std::size_t bytes) {
    return Graphics::Recorder::SnapshotWriteOverlaps(address, bytes);
}

const char* staleReason(DrawPrepareStale verdict) {
    return verdict == DrawPrepareStale::Stored ? "a driver store" : "a pending write";
}

}

Driver::DrawRecordState& Driver::drawRecordState() {
    thread_local DrawRecordState state;
    return state;
}

bool Driver::recordThread() {
    static const bool enabled = [] {
        if (std::getenv("APS5_RECORD_THREAD") == nullptr) return false;
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr) {
            std::fprintf(stderr, "[record-thread] off: APS5_LOCKED_DRAW_PREPARE prepares draws under the GPU mutex\n");
            return false;
        }
        GuestMemory::SetGpuLockGate([] {
            auto& records = drawRecords();
            if (records.Pending() && !RecordQueue::OnWorker()) records.WaitIdle();
        }, [] {
            const auto& records = drawRecords();
            return records.Pending() && !RecordQueue::OnWorker();
        });
        std::fprintf(stderr, "[record-thread] on: queue 0 records draws on a record thread\n");
        return true;
    }();
    return enabled;
}

void Driver::runDrawRecords() {
    auto& records = drawRecords();
    if (!records.Pending() || RecordQueue::Running()) return;
    std::lock_guard nested(GuestMemory::GpuMutex());
    records.RunPending();
}

bool Driver::deferDrawRecords(std::uint32_t queue) const {
    return queue == 0 && recordThread();
}

std::uint32_t Driver::prepareAhead() {
    static const std::uint32_t depth = [] {
        const char* text = std::getenv("APS5_PREPARE_AHEAD");
        if (text == nullptr) return std::uint32_t{0};
        char* end = nullptr;
        const auto value = std::strtoul(text, &end, 10);
        if (end == text || *end != '\0' || value < 1 || value > 64) throw std::runtime_error(std::string("APS5_PREPARE_AHEAD=") + text + ": expected a record depth from 1 to 64");
        if (!recordThread()) {
            std::fprintf(stderr, "[record-thread] APS5_PREPARE_AHEAD ignored: it needs APS5_RECORD_THREAD\n");
            return std::uint32_t{0};
        }
        if (value == 1) return std::uint32_t{0};
        drawRecords().StopAfterFailure(true);
        std::fprintf(stderr, "[record-thread] prepare ahead: up to %lu records in flight\n", value);
        return static_cast<std::uint32_t>(value);
    }();
    return depth;
}

bool Driver::recordDispatches() {
    static const bool enabled = recordThread() && std::getenv("APS5_RECORD_THREAD_DISPATCH") != nullptr;
    return enabled;
}

bool Driver::deferDispatchRecords(std::uint32_t queue) const {
    return queue == 0 && recordDispatches();
}

std::string Driver::recordIdentity(const Submission& submission, const char* what, std::uint64_t program) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s of program 0x%llx in submission %llu queue 0x%x at dword %zu", what, static_cast<unsigned long long>(program), static_cast<unsigned long long>(submission.serial), submission.queue, currentPacketOffset());
    return text;
}

DrawVerdict Driver::draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::string& rejected) {
    if (drawRecordState().active || !deferDrawRecords(submission.queue)) return drawNow(queue, packet, submission, rejected);
    ++drawRecordCounters().draws;
    DrawVerdict verdict = DrawVerdict::Nothing;
    std::string attempt;
    deferRecord([&] {
        attempt.clear();
        verdict = drawNow(queue, packet, submission, attempt);
    });
    rejected = std::move(attempt);
    return verdict;
}

void Driver::dispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments) {
    if (drawRecordState().active || resolvingAhead() || !deferDispatchRecords(submission.queue)) {
        dispatchNow(queue, packet, submission, indirectArguments);
        return;
    }
    ++drawRecordCounters().dispatches;
    deferRecord([&] { dispatchNow(queue, packet, submission, indirectArguments); });
}

void Driver::deferRecord(const std::function<void()>& prepare) {
    if (prepareAhead() != 0) {
        deferAhead(prepare);
        return;
    }
    auto& state = drawRecordState();
    auto& records = drawRecords();
    auto& counters = drawRecordCounters();
    for (;;) {
        state = DrawRecordState{true, records.Pending() ? records.Posted() : 0, false, false};
        struct Reset {
            DrawRecordState& state;
            ~Reset() { state = DrawRecordState{}; }
        } reset{state};
        if (state.overlapped != 0) ++counters.overlapped;
        const bool locked = GuestMemory::GpuMutex().HeldByThisThread();
        prepare();
        if (!state.settled) {
            state.settled = true;
            if (state.overlapped != 0) {
                if (locked || GuestMemory::GpuMutex().HeldByThisThread()) runDrawRecords();
                else records.WaitFinished(state.overlapped);
                finishDrawRecords();
                if (drawRecordResult != nullptr) {
                    const auto& effects = drawRecordResult->effects;
                    state.stale = effects.writeGenerationAfter != effects.writeGenerationBefore || !effects.stores.empty();
                    if (state.stale) ++counters.staleUnchecked;
                }
            }
        }
        if (!state.stale) return;
        ++counters.redos;
    }
}

void Driver::deferAhead(const std::function<void()>& prepare) {
    auto& state = drawRecordState();
    auto& records = drawRecords();
    auto& counters = drawRecordCounters();
    for (;;) {
        const auto finished = records.Finished();
        finishDrawRecords();
        drawRecordWindow.Begin(finished);
        const auto posted = records.Posted();
        state = DrawRecordState{true, posted != finished ? posted : 0, false, false};
        struct Reset {
            DrawRecordState& state;
            ~Reset() { state = DrawRecordState{}; }
        } reset{state};
        if (state.overlapped != 0) ++counters.overlapped;
        const bool locked = GuestMemory::GpuMutex().HeldByThisThread();
        prepare();
        drawRecordWindow.Take();
        if (!state.settled) {
            state.settled = true;
            if (state.overlapped != 0) {
                if (locked || GuestMemory::GpuMutex().HeldByThisThread()) runDrawRecords();
                else records.WaitFinished(state.overlapped);
                finishDrawRecords();
                state.stale = drawRecordWindow.Touched(state.overlapped);
                if (state.stale) ++counters.staleUnchecked;
            }
        }
        if (!state.stale) return;
        ++counters.redos;
    }
}

bool Driver::settleAhead(std::span<const Graphics::GuestMemorySnapshot> reads, bool drain) {
    auto& state = drawRecordState();
    if (state.settled) return !state.stale;
    state.settled = true;
    if (state.overlapped == 0) {
        finishDrawRecords();
        return true;
    }
    auto& records = drawRecords();
    auto& counters = drawRecordCounters();
    auto prepared = preparedReads(reads);
    bool full = drain || GuestMemory::GpuMutex().HeldByThisThread();
    for (std::size_t i = 0; !full && i < prepared.size(); ++i) full = prepared[i].bytes != 0 && pendingWriteOverlaps(prepared[i].address, prepared[i].bytes);
    if (full) {
        ++counters.aheadDrained;
        if (GuestMemory::GpuMutex().HeldByThisThread()) runDrawRecords();
        else records.WaitFinished(state.overlapped);
    } else if (const auto ticket = AheadWaitTicket(records.Posted(), records.Finished(), prepareAhead()); ticket != 0) {
        ++counters.aheadFull;
        records.WaitFinished(ticket);
    }
    finishDrawRecords();
    const auto finished = records.Finished();
    const auto verdict = drawRecordWindow.CheckFinished(prepared, finished, pendingWriteOverlaps);
    if (verdict != DrawPrepareStale::No) {
        if (verdict == DrawPrepareStale::PendingWrite) ++counters.stalePending;
        else ++counters.staleStored;
        ++counters.aheadStalePost;
        state.stale = true;
        return false;
    }
    if (finished < state.overlapped) drawRecordWindow.Hold({std::move(prepared), drawRecordWindow.Between(finished, state.overlapped)});
    return true;
}

bool Driver::settleDrawRecords(std::span<const Graphics::GuestMemorySnapshot> reads, bool drain) {
    if (prepareAhead() != 0) return settleAhead(reads, drain);
    auto& state = drawRecordState();
    if (state.settled) return !state.stale;
    state.settled = true;
    if (state.overlapped == 0) {
        finishDrawRecords();
        return true;
    }
    auto& records = drawRecords();
    if (GuestMemory::GpuMutex().HeldByThisThread()) runDrawRecords();
    else records.WaitFinished(state.overlapped);
    finishDrawRecords();
    if (drawRecordResult == nullptr) return true;
    std::vector<PreparedRead> prepared;
    prepared.reserve(reads.size());
    for (const auto& read : reads) prepared.push_back({read.address, read.bytes.size()});
    const auto verdict = StaleDrawPrepare(drawRecordResult->effects, prepared, [](std::uint64_t address, std::size_t bytes) { return Graphics::Recorder::SnapshotWriteOverlaps(address, bytes); });
    auto& counters = drawRecordCounters();
    if (verdict == DrawPrepareStale::PendingWrite) ++counters.stalePending;
    else if (verdict == DrawPrepareStale::Stored) ++counters.staleStored;
    state.stale = verdict != DrawPrepareStale::No;
    return !state.stale;
}

void Driver::finishDrawRecords() {
    auto& records = drawRecords();
    if (auto failure = records.TakeFailure()) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("deferred record failed: ") + error.what());
        }
    }
    if (prepareAhead() != 0) {
        drawRecordWindow.ForFinished(records.Finished(), [&](DrawRecordResult& result) {
            if (result.built == nullptr) return;
            auto built = std::move(result.built);
            result.built = nullptr;
            attachDrawRecipe(result.recipeKey, result.recipeStages, std::move(built));
            result.recipeStages.clear();
        });
        return;
    }
    if (drawRecordResult == nullptr || drawRecordResult->built == nullptr || records.Finished() < drawRecordResult->ticket) return;
    auto built = std::move(drawRecordResult->built);
    drawRecordResult->built = nullptr;
    attachDrawRecipe(drawRecordResult->recipeKey, drawRecordResult->recipeStages, std::move(built));
    drawRecordResult->recipeStages.clear();
}

void Driver::noteDrawRecordRecipe(DrawRecordResult& result, std::shared_ptr<const DrawRecipe> built, std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages) {
    result.built = std::move(built);
    result.recipeKey = key;
    result.recipeStages = stages;
}

void Driver::waitDrawRecords() {
    auto& records = drawRecords();
    if (records.Pending()) {
        ++drawRecordCounters().packetWaits;
        if (GuestMemory::GpuMutex().HeldByThisThread()) runDrawRecords();
        else records.WaitIdle();
    }
    if (auto& state = drawRecordState(); state.active && !state.settled) state.settled = true;
    finishDrawRecords();
}

void Driver::postDrawRecord(const std::shared_ptr<VulkanDevice>& localDevice, std::uint32_t queue, std::string identity, std::function<void(DrawRecordResult&)> record) {
    auto& records = drawRecords();
    if (!records.Started()) {
        records.Start([] {
            thread_local const bool tagged = (GuestMemory::TagGpuLockThread(RecordWorkerTag), true);
            (void)tagged;
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
            std::lock_guard lock(GuestMemory::GpuMutex());
            std::lock_guard nested(GuestMemory::GpuMutex());
            drawRecords().RunPending();
        });
    }
    auto result = std::make_shared<DrawRecordResult>();
    drawRecordResult = result;
    const bool ahead = prepareAhead() != 0;
    auto provisional = ahead ? drawRecordWindow.Take() : PrepareAheadWindow<DrawRecordResult>::Provisional{};
    if (!provisional.earlier.empty()) ++drawRecordCounters().aheadPosts;
    result->ticket = records.Post([result, record = std::move(record), identity = std::move(identity), queue, epoch = GuestMemory::CollectEpoch(), device = localDevice, provisional = std::move(provisional)] {
        struct Restore {
            std::uint32_t tag;
            std::uint64_t epoch;
            ~Restore() {
                GuestMemory::LogDriverStores(nullptr);
                GuestMemory::ExchangeCollectEpoch(epoch);
                GuestMemory::TagGpuLockThread(tag);
            }
        } restore{GuestMemory::GpuLockThreadTag(), GuestMemory::ExchangeCollectEpoch(epoch)};
        GuestMemory::TagGpuLockThread(queue);
        result->effects.writeGenerationBefore = Graphics::Recorder::WriteGeneration();
        result->effects.writeGenerationAfter = result->effects.writeGenerationBefore + 1;
        GuestMemory::LogDriverStores(&result->effects.stores);
        try {
            if (!provisional.earlier.empty()) {
                const auto verdict = StaleAfterEach(provisional.earlier, provisional.reads, pendingWriteOverlaps);
                if (verdict != DrawPrepareStale::No) {
                    ++drawRecordCounters().aheadStaleRun;
                    throw std::runtime_error(std::string("preparation went stale behind ") + std::to_string(provisional.earlier.size()) + " unfinished records (" + staleReason(verdict) + " over a prepared read); queue 0 has moved past its packet, so it cannot be prepared again");
                }
            }
            record(*result);
        } catch (const std::exception& error) {
            throw std::runtime_error(identity + ": " + error.what());
        }
        GuestMemory::LogDriverStores(nullptr);
        result->effects.writeGenerationAfter = Graphics::Recorder::WriteGeneration();
    });
    if (ahead) {
        drawRecordWindow.Add(result);
        noteAheadDepth(records.Posted() - records.Finished());
    }
    reportDrawRecords();
}

void Driver::reportDrawRecords() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    static auto lastReport = std::chrono::steady_clock::now();
    static RecordQueue::Statistics lastCounts{};
    static std::array<std::uint64_t, 8> last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    const auto seconds = std::chrono::duration<double>(now - lastReport).count();
    lastReport = now;
    const auto counts = drawRecords().Counts();
    auto& counters = drawRecordCounters();
    const std::array<std::uint64_t, 8> values{counters.draws.load(), counters.overlapped.load(), counters.stalePending.load(), counters.staleStored.load(), counters.staleUnchecked.load(), counters.redos.load(), counters.packetWaits.load(), counters.dispatches.load()};
    std::array<std::uint64_t, 8> delta{};
    for (std::size_t i = 0; i < values.size(); ++i) delta[i] = values[i] - last[i];
    last = values;
    const auto jobs = counts.finished - lastCounts.finished;
    const auto jobMs = static_cast<double>(counts.jobNs - lastCounts.jobNs) / 1e6;
    const auto waitedMs = static_cast<double>(counts.waitedNs - lastCounts.waitedNs) / 1e6;
    std::string ahead;
    if (const auto depth = prepareAhead(); depth != 0) {
        static std::array<std::uint64_t, 5> lastAhead{};
        const std::array<std::uint64_t, 5> aheadValues{counters.aheadPosts.load(), counters.aheadFull.load(), counters.aheadDrained.load(), counters.aheadStalePost.load(), counters.aheadStaleRun.load()};
        std::array<std::uint64_t, 5> aheadDelta{};
        for (std::size_t i = 0; i < aheadValues.size(); ++i) aheadDelta[i] = aheadValues[i] - lastAhead[i];
        lastAhead = aheadValues;
        char text[320];
        std::snprintf(text, sizeof(text), "; prepare ahead %u: %llu posted behind unfinished records, %llu found the queue full, max depth %llu, %llu drained first, stale at post %llu, stale at run %llu, %llu dropped after a failure", static_cast<unsigned>(depth), static_cast<unsigned long long>(aheadDelta[0]), static_cast<unsigned long long>(aheadDelta[1]), static_cast<unsigned long long>(counters.aheadMaxDepth.exchange(0)), static_cast<unsigned long long>(aheadDelta[2]), static_cast<unsigned long long>(aheadDelta[3]), static_cast<unsigned long long>(aheadDelta[4]), static_cast<unsigned long long>(counts.dropped - lastCounts.dropped));
        ahead = text;
    }
    ProfilePrint_nid_no_patch("[record-thread] %.3f s: %llu draws, %llu dispatches (%llu overlapped a record), %llu jobs run (%llu on the record thread, %llu inline on queue 0, %llu failed) %.1f ms, %.1f us per job; producer waits %llu (%llu blocked, %.1f ms); %llu packet waits; stale preparations: %llu pending write, %llu stored, %llu unchecked, %llu redone%s\n", seconds, static_cast<unsigned long long>(delta[0]), static_cast<unsigned long long>(delta[7]), static_cast<unsigned long long>(delta[1]), static_cast<unsigned long long>(jobs), static_cast<unsigned long long>(counts.byWorker - lastCounts.byWorker), static_cast<unsigned long long>(counts.byOthers - lastCounts.byOthers), static_cast<unsigned long long>(counts.failed - lastCounts.failed), jobMs, jobs != 0 ? jobMs * 1000 / static_cast<double>(jobs) : 0.0, static_cast<unsigned long long>(counts.waits - lastCounts.waits), static_cast<unsigned long long>(counts.blockedWaits - lastCounts.blockedWaits), waitedMs, static_cast<unsigned long long>(delta[6]), static_cast<unsigned long long>(delta[2]), static_cast<unsigned long long>(delta[3]), static_cast<unsigned long long>(delta[4]), static_cast<unsigned long long>(delta[5]), ahead.c_str());
    lastCounts = counts;
}

}

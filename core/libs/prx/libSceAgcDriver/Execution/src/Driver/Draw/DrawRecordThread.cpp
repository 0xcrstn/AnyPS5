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
    std::atomic<std::uint64_t> overlapped{0};
    std::atomic<std::uint64_t> stalePending{0};
    std::atomic<std::uint64_t> staleStored{0};
    std::atomic<std::uint64_t> staleUnchecked{0};
    std::atomic<std::uint64_t> redos{0};
    std::atomic<std::uint64_t> packetWaits{0};
};

DrawRecordCounters& drawRecordCounters() {
    static DrawRecordCounters counters;
    return counters;
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

DrawVerdict Driver::draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::string& rejected) {
    auto& state = drawRecordState();
    if (state.active || !deferDrawRecords(submission.queue)) return drawNow(queue, packet, submission, rejected);
    auto& records = drawRecords();
    auto& counters = drawRecordCounters();
    ++counters.draws;
    for (;;) {
        state = DrawRecordState{true, records.Pending() ? records.Posted() : 0, false, false};
        struct Reset {
            DrawRecordState& state;
            ~Reset() { state = DrawRecordState{}; }
        } reset{state};
        if (state.overlapped != 0) ++counters.overlapped;
        const bool locked = GuestMemory::GpuMutex().HeldByThisThread();
        std::string attempt;
        const auto verdict = drawNow(queue, packet, submission, attempt);
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
        if (!state.stale) {
            rejected = std::move(attempt);
            return verdict;
        }
        ++counters.redos;
    }
}

bool Driver::settleDrawRecords(std::span<const Graphics::GuestMemorySnapshot> reads) {
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
            throw std::runtime_error(std::string("deferred draw record failed: ") + error.what());
        }
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
    finishDrawRecords();
}

void Driver::postDrawRecord(const std::shared_ptr<VulkanDevice>& localDevice, std::uint32_t queue, std::function<void(DrawRecordResult&)> record) {
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
    result->ticket = records.Post([result, record = std::move(record), queue, epoch = GuestMemory::CollectEpoch(), device = localDevice] {
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
        record(*result);
        GuestMemory::LogDriverStores(nullptr);
        result->effects.writeGenerationAfter = Graphics::Recorder::WriteGeneration();
    });
    reportDrawRecords();
}

void Driver::reportDrawRecords() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    static auto lastReport = std::chrono::steady_clock::now();
    static RecordQueue::Statistics lastCounts{};
    static std::array<std::uint64_t, 7> last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    const auto seconds = std::chrono::duration<double>(now - lastReport).count();
    lastReport = now;
    const auto counts = drawRecords().Counts();
    auto& counters = drawRecordCounters();
    const std::array<std::uint64_t, 7> values{counters.draws.load(), counters.overlapped.load(), counters.stalePending.load(), counters.staleStored.load(), counters.staleUnchecked.load(), counters.redos.load(), counters.packetWaits.load()};
    std::array<std::uint64_t, 7> delta{};
    for (std::size_t i = 0; i < values.size(); ++i) delta[i] = values[i] - last[i];
    last = values;
    const auto jobs = counts.finished - lastCounts.finished;
    const auto jobMs = static_cast<double>(counts.jobNs - lastCounts.jobNs) / 1e6;
    const auto waitedMs = static_cast<double>(counts.waitedNs - lastCounts.waitedNs) / 1e6;
    ProfilePrint_nid_no_patch("[record-thread] %.3f s: %llu draws (%llu overlapped a record), %llu jobs run (%llu on the record thread, %llu inline on queue 0, %llu failed) %.1f ms, %.1f us per job; producer waits %llu (%llu blocked, %.1f ms); %llu packet waits; stale preparations: %llu pending write, %llu stored, %llu unchecked, %llu redone\n", seconds, static_cast<unsigned long long>(delta[0]), static_cast<unsigned long long>(delta[1]), static_cast<unsigned long long>(jobs), static_cast<unsigned long long>(counts.byWorker - lastCounts.byWorker), static_cast<unsigned long long>(counts.byOthers - lastCounts.byOthers), static_cast<unsigned long long>(counts.failed - lastCounts.failed), jobMs, jobs != 0 ? jobMs * 1000 / static_cast<double>(jobs) : 0.0, static_cast<unsigned long long>(counts.waits - lastCounts.waits), static_cast<unsigned long long>(counts.blockedWaits - lastCounts.blockedWaits), waitedMs, static_cast<unsigned long long>(delta[6]), static_cast<unsigned long long>(delta[2]), static_cast<unsigned long long>(delta[3]), static_cast<unsigned long long>(delta[4]), static_cast<unsigned long long>(delta[5]));
    lastCounts = counts;
}

}

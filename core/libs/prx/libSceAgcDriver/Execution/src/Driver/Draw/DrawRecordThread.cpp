#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/PrepareWorkers.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/RecordQueue.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
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

struct Driver::FrozenDraw {
    std::vector<std::uint32_t> packet;
    FrozenQueueState state;
    std::uint64_t serial = 0;
    std::uint32_t queue = 0;
    std::shared_ptr<const ShaderRegistry> shaders;
    std::size_t offset = 0;
    std::uint64_t epoch = 0;
};

struct Driver::PreparedFrozenDraw {
    std::uint64_t start = 0;
    DrawVerdict verdict = DrawVerdict::Nothing;
    std::string rejected;
    FrozenFallback refused = FrozenFallback::None;
    std::shared_ptr<DrawInputs> inputs;
    std::vector<PreparedRead> reads;
    std::uint64_t program = 0;
    std::shared_ptr<VulkanDevice> device;
};

namespace {

constexpr std::uint32_t RecordWorkerTag = 0x100;
constexpr std::uint32_t PrepareWorkerTag = 0x200;
constexpr std::size_t RecordHistoryEntries = 4096;

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
    std::atomic<std::uint64_t> frozen{0};
    std::atomic<std::uint64_t> frozenNs{0};
    std::atomic<std::uint64_t> frozenCopies{0};
    std::atomic<std::uint64_t> frozenNothing{0};
    std::atomic<std::uint64_t> recordsFull{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FrozenFallback::Count)> inlineFallbacks{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FrozenFallback::Count)> orderedRedos{};
    std::atomic<std::uint64_t> redoPending{0};
    std::atomic<std::uint64_t> redoStored{0};
    std::atomic<std::uint64_t> redoUnchecked{0};
    std::atomic<std::uint64_t> redoErrors{0};
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

RecordHistory& recordHistory() {
    static auto* history = new RecordHistory(RecordHistoryEntries);
    return *history;
}

void surfaceRecordFailure() {
    if (auto failure = drawRecords().TakeFailure()) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("deferred record failed: ") + error.what());
        }
    }
}

std::string frozenIdentity(std::uint64_t serial, std::uint32_t queue, std::size_t offset, std::uint64_t program) {
    char text[160];
    std::snprintf(text, sizeof(text), "draw of program 0x%llx in submission %llu queue 0x%x at dword %zu (prepared from a frozen packet)", static_cast<unsigned long long>(program), static_cast<unsigned long long>(serial), queue, offset);
    return text;
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
            if (frozenProducer() && prepareWorkers() != 0) Get().drainFrozen(false);
            auto& records = drawRecords();
            if (records.Pending() && !RecordQueue::OnWorker()) records.WaitIdle();
        }, [] {
            const auto& records = drawRecords();
            if (RecordQueue::OnWorker()) return false;
            return records.Pending() || (frozenProducer() && prepareWorkers() != 0 && !frozenDraws().Idle());
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

std::uint32_t Driver::prepareWorkers() {
    static const std::uint32_t workers = [] {
        const char* text = std::getenv("APS5_PREPARE_WORKERS");
        if (text == nullptr) return std::uint32_t{0};
        char* end = nullptr;
        const auto value = std::strtoul(text, &end, 10);
        if (end == text || *end != '\0' || value < 1 || value > 16) throw std::runtime_error(std::string("APS5_PREPARE_WORKERS=") + text + ": expected a preparation worker count from 1 to 16");
        if (!recordThread()) {
            std::fprintf(stderr, "[record-thread] APS5_PREPARE_WORKERS ignored: it needs APS5_RECORD_THREAD\n");
            return std::uint32_t{0};
        }
        if (std::getenv("APS5_PREPARE_AHEAD") == nullptr) {
            std::fprintf(stderr, "[record-thread] APS5_PREPARE_WORKERS ignored: it needs APS5_PREPARE_AHEAD\n");
            return std::uint32_t{0};
        }
        const auto depth = std::max<std::uint32_t>(prepareAhead(), 1u);
        drawRecords().StopAfterFailure(true);
        std::fprintf(stderr, "[record-thread] prepare workers: %lu threads prepare frozen draws, up to %lu ahead of the record thread\n", value, 2ul * value + depth);
        return static_cast<std::uint32_t>(value);
    }();
    return workers;
}

bool Driver::parallelResolve() {
    static const bool enabled = [] {
        if (!Graphics::ParallelResolveRequested()) return false;
        if (prepareWorkers() == 0) {
            std::fprintf(stderr, "[record-thread] APS5_PARALLEL_RESOLVE ignored: it needs APS5_PREPARE_WORKERS\n");
            return false;
        }
        if (Graphics::ParallelRevalidateRequested()) std::fprintf(stderr, "[record-thread] parallel revalidate: preparation workers check template epoch proofs against the dirty log; the record thread adopts them after a stamp compare%s\n", Graphics::VerifyParallelRevalidate() ? " and verifies each against the serial Revalidate" : "");
        std::fprintf(stderr, "[record-thread] parallel resolve: preparation workers resolve draw inputs, templates and pipelines; the record thread encodes%s\n", Graphics::VerifyParallelResolve() ? " and verifies every adopted decision against the serial resolve" : "");
        return true;
    }();
    return enabled;
}

std::uint32_t Driver::frozenBound() {
    return 2u * prepareWorkers() + std::max<std::uint32_t>(prepareAhead(), 1u);
}

Driver::FrozenPipeline& Driver::frozenDraws() {
    static auto* pipeline = new FrozenPipeline(prepareWorkers(), frozenBound(), [](std::shared_ptr<FrozenDraw>& frozen) {
        return Get().prepareFrozen(*frozen, false);
    }, [](std::shared_ptr<FrozenDraw>& frozen, std::optional<std::shared_ptr<PreparedFrozenDraw>>& prepared, std::exception_ptr error) {
        Get().emitFrozen(frozen, prepared, error);
    }, [](std::size_t index) {
        GuestMemory::TagGpuLockThread(PrepareWorkerTag + static_cast<std::uint32_t>(index));
    });
    return *pipeline;
}

Driver::FrozenSink*& Driver::frozenSink() {
    thread_local FrozenSink* sink = nullptr;
    return sink;
}

bool& Driver::frozenProducer() {
    thread_local bool producer = false;
    return producer;
}

void Driver::noteRecordHistory(std::uint64_t ticket, const std::shared_ptr<DrawRecordResult>& result) {
    recordHistory().Note(ticket, std::shared_ptr<const DrawRecordEffects>(result, &result->effects));
}

FrozenFallback Driver::frozenFallback(std::span<const std::uint32_t> packet) {
    static const bool dumps = [] {
        const char* shaders = std::getenv("APS5_DUMP_DRAW_SHADERS");
        const char* slot = std::getenv("APS5_DUMP_DRAW_SLOT1");
        return (shaders != nullptr && std::strtoull(shaders, nullptr, 16) != 0) || (slot != nullptr && std::strtoull(slot, nullptr, 16) != 0);
    }();
    if (Pm4::IndirectDrawOpcode((packet[0] >> 8u) & 0xffu)) return FrozenFallback::Indirect;
    if (!bindingPlan()) return FrozenFallback::DrawCache;
    if (dumps || ShaderRecompiler::DebugProbeActive()) return FrozenFallback::Debug;
    if (device.Load() == nullptr) return FrozenFallback::NoDevice;
    if (GuestMemory::GpuMutex().HeldByThisThread()) return FrozenFallback::GpuMutexHeld;
    if (!deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value()) return FrozenFallback::Labels;
    return FrozenFallback::None;
}

DrawVerdict Driver::freezeDraw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission) {
    const auto begin = std::chrono::steady_clock::now();
    auto& counters = drawRecordCounters();
    frozenProducer() = true;
    surfaceRecordFailure();
    startDrawRecords();
    static RegisterFreezer freezer;
    auto frozen = std::make_shared<FrozenDraw>();
    frozen->packet.assign(packet.begin(), packet.end());
    frozen->state = freezer.Freeze(queue);
    frozen->serial = submission.serial;
    frozen->queue = submission.queue;
    frozen->shaders = submission.shaders;
    frozen->offset = currentPacketOffset();
    frozen->epoch = GuestMemory::CollectEpoch();
    counters.frozenCopies.store(freezer.Copies(), std::memory_order_relaxed);
    const auto frozenAt = std::chrono::steady_clock::now();
    counters.frozenNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(frozenAt - begin).count());
    auto& records = drawRecords();
    if (const auto ticket = AheadWaitTicket(records.Posted(), records.Finished(), frozenBound()); ticket != 0) {
        ++counters.recordsFull;
        records.WaitFinished(ticket);
    }
    frozenDraws().Submit(std::move(frozen));
    ++counters.frozen;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile) {
        const auto now = std::chrono::steady_clock::now();
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = 0;
        pending.ms = {};
        if (packetStartedAt() != std::chrono::steady_clock::time_point{}) pending.ms[DrawRowPrologue] = std::chrono::duration<double, std::milli>(begin - packetStartedAt()).count();
        pending.ms[DrawRowVectors] = std::chrono::duration<double, std::milli>(frozenAt - begin).count();
        pending.ms[DrawRowLockWait] = std::chrono::duration<double, std::milli>(now - frozenAt).count();
        pending.tailAt = now;
    }
    reportDrawRecords();
    return DrawVerdict::Drawn;
}

std::shared_ptr<Driver::PreparedFrozenDraw> Driver::prepareFrozen(const FrozenDraw& frozen, bool inOrder) {
    if (frozenProducer()) throw std::logic_error("prepare workers: a frozen draw reached preparation on queue 0's own thread");
    thread_local FrozenQueueCopy copy;
    thread_local Submission submission{};
    auto& queue = copy.Apply(frozen.state);
    submission.serial = frozen.serial;
    submission.queue = frozen.queue;
    submission.shaders = frozen.shaders;
    FrozenSink sink;
    sink.inOrder = inOrder;
    struct Scope {
        std::size_t offset;
        std::uint64_t epoch;
        FrozenSink* sink;
        ~Scope() {
            currentPacketOffset() = offset;
            GuestMemory::ExchangeCollectEpoch(epoch);
            frozenSink() = sink;
        }
    } scope{currentPacketOffset(), GuestMemory::ExchangeCollectEpoch(frozen.epoch), frozenSink()};
    currentPacketOffset() = frozen.offset;
    frozenSink() = &sink;
    auto prepared = std::make_shared<PreparedFrozenDraw>();
    prepared->start = drawRecords().Finished();
    prepared->verdict = drawNow(queue, frozen.packet, submission, prepared->rejected);
    prepared->refused = sink.refused;
    prepared->inputs = std::move(sink.inputs);
    prepared->reads = std::move(sink.reads);
    prepared->program = sink.program;
    prepared->device = device.Load();
    return prepared;
}

void Driver::emitFrozen(const std::shared_ptr<FrozenDraw>& frozen, std::optional<std::shared_ptr<PreparedFrozenDraw>>& prepared, std::exception_ptr error) {
    auto& counters = drawRecordCounters();
    std::shared_ptr<PreparedFrozenDraw> ready = prepared.has_value() ? *prepared : nullptr;
    const bool redo = error != nullptr || ready == nullptr || ready->refused != FrozenFallback::None || !ready->rejected.empty();
    if (!redo && ready->inputs == nullptr) {
        ++counters.frozenNothing;
        return;
    }
    if (!redo) noteDrawInputWriters(*ready->inputs, frozen->queue);
    auto result = std::make_shared<DrawRecordResult>();
    result->ticket = drawRecords().Post([this, frozen, ready, error, result] {
        struct Restore {
            std::uint32_t tag;
            std::uint64_t epoch;
            ~Restore() {
                GuestMemory::LogDriverStores(nullptr);
                GuestMemory::ExchangeCollectEpoch(epoch);
                GuestMemory::TagGpuLockThread(tag);
            }
        } restore{GuestMemory::GpuLockThreadTag(), GuestMemory::ExchangeCollectEpoch(frozen->epoch)};
        GuestMemory::TagGpuLockThread(frozen->queue);
        const auto ticket = drawRecords().Finished() + 1;
        result->effects.writeGenerationBefore = Graphics::Recorder::WriteGeneration();
        result->effects.writeGenerationAfter = result->effects.writeGenerationBefore + 1;
        GuestMemory::LogDriverStores(&result->effects.stores);
        struct History {
            std::uint64_t ticket;
            const std::shared_ptr<DrawRecordResult>& result;
            ~History() { noteRecordHistory(ticket, result); }
        } history{ticket, result};
        auto& counters = drawRecordCounters();
        auto use = ready;
        try {
            bool redo = false;
            if (error != nullptr || use == nullptr || !use->rejected.empty()) {
                redo = true;
                ++counters.redoErrors;
            } else if (use->refused != FrozenFallback::None) {
                redo = true;
                ++counters.orderedRedos[static_cast<std::size_t>(use->refused)];
            } else {
                switch (CheckFrozenPrepare(recordHistory(), use->start, ticket, use->reads, pendingWriteOverlaps)) {
                case FrozenPrepareVerdict::Fresh: break;
                case FrozenPrepareVerdict::PendingWrite: redo = true; ++counters.redoPending; break;
                case FrozenPrepareVerdict::Stored: redo = true; ++counters.redoStored; break;
                case FrozenPrepareVerdict::Unchecked: redo = true; ++counters.redoUnchecked; break;
                }
            }
            if (redo) {
                use = prepareFrozen(*frozen, true);
                if (use->refused != FrozenFallback::None) throw std::logic_error("a frozen draw refused its in-order preparation");
                if (use->inputs != nullptr) noteDrawInputWriters(*use->inputs, frozen->queue);
            }
            if (!use->rejected.empty()) throw std::runtime_error(use->rejected);
            if (use->inputs != nullptr) recordDrawInputs(*use->inputs, *result, use->device);
        } catch (const std::exception& failure) {
            throw std::runtime_error(frozenIdentity(frozen->serial, frozen->queue, frozen->offset, use != nullptr ? use->program : 0) + ": " + failure.what());
        }
        GuestMemory::LogDriverStores(nullptr);
        result->effects.writeGenerationAfter = Graphics::Recorder::WriteGeneration();
    });
    std::lock_guard lock(frozenPostedMutex);
    frozenPosted.push_back(std::move(result));
}

void Driver::drainFrozen(bool records) {
    if (prepareWorkers() == 0) return;
    auto& pipeline = frozenDraws();
    if (!pipeline.Idle()) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::logic_error("prepare workers: frozen draws are still being prepared while queue 0 holds the GPU mutex");
        pipeline.WaitEmitted();
    }
    if (!records) return;
    auto& posted = drawRecords();
    if (!posted.Pending()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) runDrawRecords();
    else posted.WaitIdle();
}

void Driver::mergeFrozenRecords() {
    if (prepareWorkers() == 0) return;
    std::vector<std::shared_ptr<DrawRecordResult>> posted;
    {
        std::lock_guard lock(frozenPostedMutex);
        posted.swap(frozenPosted);
    }
    if (prepareAhead() == 0) return;
    const auto finished = drawRecords().Finished();
    for (auto& result : posted) {
        if (result->ticket > finished) drawRecordWindow.Add(std::move(result));
    }
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
    if (drawRecordState().active || frozenSink() != nullptr || !deferDrawRecords(submission.queue)) return drawNow(queue, packet, submission, rejected);
    if (prepareWorkers() != 0) {
        const auto reason = frozenFallback(packet);
        if (reason == FrozenFallback::None) return freezeDraw(queue, packet, submission);
        ++drawRecordCounters().inlineFallbacks[static_cast<std::size_t>(reason)];
        drainFrozen(true);
        mergeFrozenRecords();
    }
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
    if (prepareWorkers() != 0) {
        drainFrozen(prepareAhead() == 0);
        mergeFrozenRecords();
    }
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
    drainFrozen(false);
    mergeFrozenRecords();
    auto& records = drawRecords();
    if (records.Pending()) {
        ++drawRecordCounters().packetWaits;
        if (GuestMemory::GpuMutex().HeldByThisThread()) runDrawRecords();
        else records.WaitIdle();
    }
    if (auto& state = drawRecordState(); state.active && !state.settled) state.settled = true;
    finishDrawRecords();
}

void Driver::startDrawRecords() {
    auto& records = drawRecords();
    if (records.Started()) return;
    records.Start([] {
        thread_local const bool tagged = (GuestMemory::TagGpuLockThread(RecordWorkerTag), true);
        (void)tagged;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard lock(GuestMemory::GpuMutex());
        std::lock_guard nested(GuestMemory::GpuMutex());
        drawRecords().RunPending();
    });
}

void Driver::postDrawRecord(const std::shared_ptr<VulkanDevice>& localDevice, std::uint32_t queue, std::string identity, std::function<void(DrawRecordResult&)> record) {
    auto& records = drawRecords();
    startDrawRecords();
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
        struct History {
            std::uint64_t ticket;
            const std::shared_ptr<DrawRecordResult>& result;
            ~History() {
                if (ticket != 0) noteRecordHistory(ticket, result);
            }
        } history{prepareWorkers() != 0 ? drawRecords().Finished() + 1 : 0, result};
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
    if (const auto workers = prepareWorkers(); workers != 0) {
        constexpr std::size_t Reasons = static_cast<std::size_t>(FrozenFallback::Count);
        static std::array<std::uint64_t, 8> lastWorkers{};
        static std::array<std::uint64_t, Reasons> lastInline{};
        static std::array<std::uint64_t, Reasons> lastOrdered{};
        const auto pipeline = frozenDraws().Counts();
        const std::array<std::uint64_t, 8> workerValues{counters.frozen.load(), counters.frozenNs.load(), counters.frozenNothing.load(), counters.recordsFull.load(), counters.redoPending.load() + counters.redoStored.load(), counters.redoUnchecked.load(), counters.redoErrors.load(), pipeline.busyNs};
        std::array<std::uint64_t, 8> workerDelta{};
        for (std::size_t i = 0; i < workerValues.size(); ++i) workerDelta[i] = workerValues[i] - lastWorkers[i];
        lastWorkers = workerValues;
        static std::uint64_t lastFullWaits = 0;
        const auto fullWaits = pipeline.fullWaits - lastFullWaits;
        lastFullWaits = pipeline.fullWaits;
        const auto reasons = [](const std::array<std::atomic<std::uint64_t>, Reasons>& values, std::array<std::uint64_t, Reasons>& last) {
            std::string text;
            for (std::size_t reason = 1; reason < Reasons; ++reason) {
                const auto value = values[reason].load();
                const auto delta = value - last[reason];
                last[reason] = value;
                if (delta == 0) continue;
                text += std::string(text.empty() ? "" : ", ") + FrozenFallbackNames[reason] + " " + std::to_string(delta);
            }
            return text.empty() ? std::string("none") : text;
        };
        const auto inlineText = reasons(counters.inlineFallbacks, lastInline);
        const auto orderedText = reasons(counters.orderedRedos, lastOrdered);
        const auto busy = seconds > 0 ? static_cast<double>(workerDelta[7]) / (seconds * 1e9 * workers) * 100.0 : 0.0;
        char text[512];
        std::snprintf(text, sizeof(text), "; prepare workers %u: %llu frozen draws (freeze %.2f us each, %llu register files copied so far), %llu prepared to nothing; inline on queue 0 after a drain: %s; redone in order on the record thread: stale %llu, unchecked %llu, after a worker error or rejection %llu, refused by a worker: %s; workers busy %.1f %%, reorder buffer full %llu, record queue full %llu", workers, static_cast<unsigned long long>(workerDelta[0]), workerDelta[0] != 0 ? static_cast<double>(workerDelta[1]) / 1000.0 / static_cast<double>(workerDelta[0]) : 0.0, static_cast<unsigned long long>(counters.frozenCopies.load()), static_cast<unsigned long long>(workerDelta[2]), inlineText.c_str(), static_cast<unsigned long long>(workerDelta[4]), static_cast<unsigned long long>(workerDelta[5]), static_cast<unsigned long long>(workerDelta[6]), orderedText.c_str(), busy, static_cast<unsigned long long>(fullWaits), static_cast<unsigned long long>(workerDelta[3]));
        ahead += text;
    }
    ProfilePrint_nid_no_patch("[record-thread] %.3f s: %llu draws, %llu dispatches (%llu overlapped a record), %llu jobs run (%llu on the record thread, %llu inline on queue 0, %llu failed) %.1f ms, %.1f us per job; producer waits %llu (%llu blocked, %.1f ms); %llu packet waits; stale preparations: %llu pending write, %llu stored, %llu unchecked, %llu redone%s\n", seconds, static_cast<unsigned long long>(delta[0]), static_cast<unsigned long long>(delta[7]), static_cast<unsigned long long>(delta[1]), static_cast<unsigned long long>(jobs), static_cast<unsigned long long>(counts.byWorker - lastCounts.byWorker), static_cast<unsigned long long>(counts.byOthers - lastCounts.byOthers), static_cast<unsigned long long>(counts.failed - lastCounts.failed), jobMs, jobs != 0 ? jobMs * 1000 / static_cast<double>(jobs) : 0.0, static_cast<unsigned long long>(counts.waits - lastCounts.waits), static_cast<unsigned long long>(counts.blockedWaits - lastCounts.blockedWaits), waitedMs, static_cast<unsigned long long>(delta[6]), static_cast<unsigned long long>(delta[2]), static_cast<unsigned long long>(delta[3]), static_cast<unsigned long long>(delta[4]), static_cast<unsigned long long>(delta[5]), ahead.c_str());
    lastCounts = counts;
}

}

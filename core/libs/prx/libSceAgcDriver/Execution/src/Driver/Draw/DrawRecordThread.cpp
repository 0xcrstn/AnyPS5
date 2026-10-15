#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/DefaultSwitch.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/PrepareWorkers.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/RecordQueue.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <bit>
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
#include <cstring>
#include <thread>
#include <string_view>
#include <tuple>
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
    bool dispatch = false;
    std::uint64_t indirectArguments = 0;
    std::vector<DeferredLabel> labels;
    bool endOfPipe = false;
};

struct Driver::PreparedFrozenDraw {
    std::uint64_t start = 0;
    std::uint64_t storeSequence = 0;
    DrawVerdict verdict = DrawVerdict::Nothing;
    std::string rejected;
    FrozenFallback refused = FrozenFallback::None;
    std::shared_ptr<DrawInputs> inputs;
    std::shared_ptr<DispatchInputs> dispatchInputs;
    std::vector<PreparedRead> reads;
    std::uint64_t program = 0;
    std::shared_ptr<VulkanDevice> device;
};

namespace {

constexpr std::uint32_t RecordWorkerTag = 0x100;
constexpr std::uint32_t PrepareWorkerTag = 0x200;
constexpr std::size_t RecordHistoryEntries = 4096;

std::chrono::steady_clock::time_point& drawEnteredAt() {
    thread_local std::chrono::steady_clock::time_point entered{};
    return entered;
}

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
    std::atomic<std::uint64_t> frozenDispatches{0};
    std::atomic<std::uint64_t> frozenDispatchNs{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FrozenFallback::Count)> dispatchInline{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FrozenFallback::Count)> dispatchOrdered{};
    std::atomic<std::uint64_t> dispatchRedoStale{0};
    std::atomic<std::uint64_t> dispatchRedoUnchecked{0};
    std::atomic<std::uint64_t> dispatchRedoErrors{0};
    std::atomic<std::uint64_t> dispatchVerified{0};
    std::atomic<std::uint64_t> dispatchVerifyStale{0};
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

std::uint64_t& aheadStoreSequence() {
    thread_local std::uint64_t sequence = 0;
    return sequence;
}

FrozenPrepareVerdict lateStale(std::span<const PreparedRead> reads, std::uint64_t storeSequence, bool pendingResults = true) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(reads.size());
    for (const auto& read : reads) {
        if (read.bytes == 0) continue;
        if (pendingResults && (Graphics::PendingStorageOverlaps(read.address, read.bytes, nullptr) || Graphics::AnyShadowedOverlaps(read.address, read.bytes))) return FrozenPrepareVerdict::Stored;
        ranges.emplace_back(read.address, read.address + read.bytes);
    }
    switch (GuestMemory::DriverStoresSinceSequence(storeSequence, ranges)) {
    case GuestMemory::DriverStoredSince::No: return FrozenPrepareVerdict::Fresh;
    case GuestMemory::DriverStoredSince::Stored: return FrozenPrepareVerdict::Stored;
    case GuestMemory::DriverStoredSince::Unknown: return FrozenPrepareVerdict::Unchecked;
    }
    return FrozenPrepareVerdict::Unchecked;
}

const char* staleReason(DrawPrepareStale verdict) {
    return verdict == DrawPrepareStale::Stored ? "a driver store" : "a pending write";
}

RecordHistory& recordHistory() {
    static auto* history = new RecordHistory(RecordHistoryEntries);
    return *history;
}

RegisterFreezer& registerFreezer() {
    static RegisterFreezer freezer;
    return freezer;
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

std::string frozenIdentity(const char* what, std::uint64_t serial, std::uint32_t queue, std::size_t offset, std::uint64_t program) {
    char text[192];
    std::snprintf(text, sizeof(text), "%s of program 0x%llx in submission %llu queue 0x%x at dword %zu (prepared from a frozen packet)", what, static_cast<unsigned long long>(program), static_cast<unsigned long long>(serial), queue, offset);
    return text;
}

}

struct PacketWaitCounts {
    std::array<std::atomic<std::uint64_t>, 0x103> waits{};
    std::array<std::atomic<std::uint64_t>, 0x103> waitedNs{};
    std::atomic<std::uint64_t> commandProcessorReads{0};
    std::atomic<std::uint64_t> commandProcessorReadsAhead{0};
    std::atomic<std::uint64_t> commandProcessorReadsVerified{0};
    std::atomic<std::uint64_t> registerStatePackets{0};
    std::atomic<std::uint64_t> labelJobs{0};
    std::atomic<std::uint64_t> labelJobLabels{0};
    std::atomic<std::uint64_t> labelPackets{0};
    std::atomic<std::uint64_t> endOfPipeJobs{0};
};

std::atomic<int>& commandProcessorReadsOverride() {
    static std::atomic<int> mode{-1};
    return mode;
}

PacketWaitCounts& packetWaitCounts();

Driver::DrawRecordState& Driver::drawRecordState() {
    thread_local DrawRecordState state;
    return state;
}

bool Driver::recordThread() {
    static const bool enabled = [] {
        if (!DefaultOnSwitch("APS5_RECORD_THREAD")) return false;
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr) {
            std::fprintf(stderr, "[record-thread] off: APS5_LOCKED_DRAW_PREPARE prepares draws under the GPU mutex\n");
            return false;
        }
        GuestMemory::SetGpuLockGate([] {
            if (frozenProducer() && prepareWorkers() != 0) Get().drainFrozen(false);
            auto& records = drawRecords();
            if (records.Pending() && !RecordQueue::OnWorker()) records.WaitIdle();
            Graphics::Recorder::DrainChunks();
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

namespace {

unsigned long defaultPrepareWorkers() {
    const auto& layout = CpuTopology::Get();
    if (layout.hybrid) return std::clamp<unsigned long>(static_cast<unsigned long>(std::popcount(layout.efficient & (layout.process != 0 ? layout.process : ~std::uint64_t{0}))), 1ul, 16ul);
    const auto logical = static_cast<unsigned long>(std::thread::hardware_concurrency());
    return std::clamp<unsigned long>(logical > 4ul ? logical / 2ul - 2ul : 1ul, 1ul, 16ul);
}

}

std::uint32_t Driver::prepareAhead() {
    static const std::uint32_t depth = [] {
        const char* text = std::getenv("APS5_PREPARE_AHEAD");
        unsigned long value = std::max(4ul, defaultPrepareWorkers());
        if (text != nullptr) {
            if (std::strcmp(text, "0") == 0) return std::uint32_t{0};
            char* end = nullptr;
            value = std::strtoul(text, &end, 10);
            if (end == text || *end != '\0' || value < 1 || value > 64) throw std::runtime_error(std::string("APS5_PREPARE_AHEAD=") + text + ": expected 0 (off) or a record depth from 1 to 64");
        }
        if (!recordThread()) {
            if (text != nullptr) std::fprintf(stderr, "[record-thread] APS5_PREPARE_AHEAD ignored: it needs APS5_RECORD_THREAD\n");
            return std::uint32_t{0};
        }
        if (value == 1) return std::uint32_t{0};
        drawRecords().StopAfterFailure(true);
        GuestMemory::EnableDriverStoreLog();
        std::fprintf(stderr, "[record-thread] prepare ahead: up to %lu records in flight\n", value);
        return static_cast<std::uint32_t>(value);
    }();
    return depth;
}

std::uint32_t Driver::prepareWorkers() {
    static const std::uint32_t workers = [] {
        const char* text = std::getenv("APS5_PREPARE_WORKERS");
        unsigned long value = defaultPrepareWorkers();
        if (text != nullptr) {
            if (std::strcmp(text, "0") == 0) return std::uint32_t{0};
            char* end = nullptr;
            value = std::strtoul(text, &end, 10);
            if (end == text || *end != '\0' || value < 1 || value > 16) throw std::runtime_error(std::string("APS5_PREPARE_WORKERS=") + text + ": expected 0 (off) or a preparation worker count from 1 to 16");
        }
        if (!recordThread()) {
            if (text != nullptr) std::fprintf(stderr, "[record-thread] APS5_PREPARE_WORKERS ignored: it needs APS5_RECORD_THREAD\n");
            return std::uint32_t{0};
        }
        if (prepareAhead() == 0) {
            if (text != nullptr) std::fprintf(stderr, "[record-thread] APS5_PREPARE_WORKERS ignored: it needs APS5_PREPARE_AHEAD\n");
            return std::uint32_t{0};
        }
        const auto depth = std::max<std::uint32_t>(prepareAhead(), 1u);
        drawRecords().StopAfterFailure(true);
        GuestMemory::EnableDriverStoreLog();
        std::fprintf(stderr, "[record-thread] prepare workers: %lu threads prepare frozen draws, up to %lu ahead of the record thread\n", value, 2ul * value + depth);
        return static_cast<std::uint32_t>(value);
    }();
    return workers;
}

bool Driver::prepareDispatches() {
    static const bool enabled = [] {
        if (!DefaultOnSwitch("APS5_PREPARE_DISPATCHES")) return false;
        if (prepareWorkers() == 0) {
            std::fprintf(stderr, "[record-thread] APS5_PREPARE_DISPATCHES ignored: it needs APS5_PREPARE_WORKERS\n");
            return false;
        }
        if (!recordDispatches()) {
            std::fprintf(stderr, "[record-thread] APS5_PREPARE_DISPATCHES ignored: it needs APS5_RECORD_THREAD_DISPATCH\n");
            return false;
        }
        Graphics::GuestBufferMemory::RecordPrepareCpuCopies(true, std::getenv("APS5_VERIFY_PREPARE_DISPATCHES") != nullptr);
        std::fprintf(stderr, "[record-thread] prepare dispatches: queue 0 freezes dispatches into the draw ticket order; preparation workers prepare them through stage A%s\n", std::getenv("APS5_VERIFY_PREPARE_DISPATCHES") != nullptr ? ", and the record thread verifies each against an in-order preparation" : "");
        return true;
    }();
    return enabled;
}

bool Driver::verifyPrepareDispatches() {
    static const bool enabled = prepareDispatches() && std::getenv("APS5_VERIFY_PREPARE_DISPATCHES") != nullptr;
    return enabled;
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
        if (Graphics::ParallelTargetsRequested()) std::fprintf(stderr, "[record-thread] parallel targets: preparation workers look up resident target images, views and framebuffers; the record thread proves the targets and adopts the views and framebuffer for the same images%s\n", Graphics::VerifyParallelTargets() ? ", verifying each against the serial lookup" : "");
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
    if (!deferredLabels().labels.empty() || ((!LabelRecordJobs() || !LabelBatchSubmit()) && Graphics::Recorder::PendingLabelSince().has_value())) return FrozenFallback::Labels;
    return FrozenFallback::None;
}

DrawVerdict Driver::freezeDraw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission) {
    const auto begin = std::chrono::steady_clock::now();
    auto& counters = drawRecordCounters();
    frozenProducer() = true;
    surfaceRecordFailure();
    startDrawRecords();
    auto& freezer = registerFreezer();
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
        const auto entered = std::exchange(drawEnteredAt(), std::chrono::steady_clock::time_point{});
        const auto split = entered != std::chrono::steady_clock::time_point{} && entered <= begin ? entered : begin;
        if (packetStartedAt() != std::chrono::steady_clock::time_point{}) pending.ms[DrawRowPrologue] = std::chrono::duration<double, std::milli>(split - packetStartedAt()).count();
        pending.ms[DrawRowFreezeCheck] = std::chrono::duration<double, std::milli>(begin - split).count();
        pending.ms[DrawRowVectors] = std::chrono::duration<double, std::milli>(frozenAt - begin).count();
        pending.ms[DrawRowLockWait] = std::chrono::duration<double, std::milli>(now - frozenAt).count();
        pending.tailAt = now;
    }
    reportDrawRecords();
    return DrawVerdict::Drawn;
}

void Driver::freezeDispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments, std::chrono::steady_clock::time_point begin) {
    auto& counters = drawRecordCounters();
    frozenProducer() = true;
    surfaceRecordFailure();
    startDrawRecords();
    auto& freezer = registerFreezer();
    auto frozen = std::make_shared<FrozenDraw>();
    frozen->packet.assign(packet.begin(), packet.end());
    frozen->state = freezer.Freeze(queue);
    frozen->serial = submission.serial;
    frozen->queue = submission.queue;
    frozen->shaders = submission.shaders;
    frozen->offset = currentPacketOffset();
    frozen->epoch = GuestMemory::CollectEpoch();
    frozen->dispatch = true;
    frozen->indirectArguments = indirectArguments;
    counters.frozenCopies.store(freezer.Copies(), std::memory_order_relaxed);
    const auto frozenAt = std::chrono::steady_clock::now();
    counters.frozenDispatchNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(frozenAt - begin).count());
    auto& records = drawRecords();
    if (const auto ticket = AheadWaitTicket(records.Posted(), records.Finished(), frozenBound()); ticket != 0) {
        ++counters.recordsFull;
        records.WaitFinished(ticket);
    }
    frozenDraws().Submit(std::move(frozen));
    ++counters.frozenDispatches;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (profile) {
        const auto now = std::chrono::steady_clock::now();
        auto& pending = pendingDispatchPhases();
        pending.outcome = DispatchOutcome::Real;
        pending.phases = true;
        pending.hit = false;
        pending.validated = false;
        pending.ms = {};
        if (packetStartedAt() != std::chrono::steady_clock::time_point{}) pending.ms[PhasePrologue] = std::chrono::duration<double, std::milli>(begin - packetStartedAt()).count();
        pending.ms[PhaseKey] = std::chrono::duration<double, std::milli>(frozenAt - begin).count();
        pending.ms[PhaseLockWait] = std::chrono::duration<double, std::milli>(now - frozenAt).count();
        pending.tailAt = now;
    }
    reportDrawRecords();
}

std::shared_ptr<Driver::PreparedFrozenDraw> Driver::prepareFrozen(const FrozenDraw& frozen, bool inOrder) {
    if (frozenProducer()) throw std::logic_error("prepare workers: a frozen draw reached preparation on queue 0's own thread");
    if (!frozen.labels.empty() || frozen.endOfPipe) return std::make_shared<PreparedFrozenDraw>();
    struct CopyTag {};
    struct SubmissionTag {};
    auto& copy = HostThreadLocal<FrozenQueueCopy, CopyTag>();
    auto& submission = HostThreadLocal<Submission, SubmissionTag>();
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
    prepared->storeSequence = GuestMemory::DriverStoreSequence();
    if (frozen.dispatch) dispatchNow(queue, frozen.packet, submission, frozen.indirectArguments);
    else prepared->verdict = drawNow(queue, frozen.packet, submission, prepared->rejected);
    prepared->refused = sink.refused;
    prepared->inputs = std::move(sink.inputs);
    prepared->dispatchInputs = std::move(sink.dispatchInputs);
    prepared->reads = std::move(sink.reads);
    prepared->program = sink.program;
    prepared->device = device.Load();
    return prepared;
}

void Driver::emitFrozen(const std::shared_ptr<FrozenDraw>& frozen, std::optional<std::shared_ptr<PreparedFrozenDraw>>& prepared, std::exception_ptr error) {
    if (!frozen->labels.empty() || frozen->endOfPipe) {
        emitFrozenLabels(frozen);
        return;
    }
    auto& counters = drawRecordCounters();
    std::shared_ptr<PreparedFrozenDraw> ready = prepared.has_value() ? *prepared : nullptr;
    const bool redo = error != nullptr || ready == nullptr || ready->refused != FrozenFallback::None || !ready->rejected.empty();
    if (!redo && ready->inputs == nullptr && ready->dispatchInputs == nullptr) {
        ++counters.frozenNothing;
        return;
    }
    if (!redo && ready->dispatchInputs != nullptr) noteDispatchInputWriters(*ready->dispatchInputs, frozen->queue);
    if (!redo && ready->inputs != nullptr) noteDrawInputWriters(*ready->inputs, frozen->queue);
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
            const bool dispatch = frozen->dispatch;
            bool redo = false;
            if (error != nullptr || use == nullptr || !use->rejected.empty()) {
                redo = true;
                ++(dispatch ? counters.dispatchRedoErrors : counters.redoErrors);
            } else if (use->refused != FrozenFallback::None) {
                redo = true;
                ++(dispatch ? counters.dispatchOrdered : counters.orderedRedos)[static_cast<std::size_t>(use->refused)];
            } else {
                auto verdict = CheckFrozenPrepare(recordHistory(), use->start, ticket, use->reads, pendingWriteOverlaps);
                if (verdict == FrozenPrepareVerdict::Fresh) verdict = lateStale(use->reads, use->storeSequence);
                switch (verdict) {
                case FrozenPrepareVerdict::Fresh: break;
                case FrozenPrepareVerdict::PendingWrite: redo = true; ++(dispatch ? counters.dispatchRedoStale : counters.redoPending); break;
                case FrozenPrepareVerdict::Stored: redo = true; ++(dispatch ? counters.dispatchRedoStale : counters.redoStored); break;
                case FrozenPrepareVerdict::Unchecked: redo = true; ++(dispatch ? counters.dispatchRedoUnchecked : counters.redoUnchecked); break;
                }
            }
            if (redo) {
                use = prepareFrozen(*frozen, true);
                if (use->refused != FrozenFallback::None) throw std::logic_error(dispatch ? "a frozen dispatch refused its in-order preparation" : "a frozen draw refused its in-order preparation");
                if (use->inputs != nullptr) noteDrawInputWriters(*use->inputs, frozen->queue);
                if (use->dispatchInputs != nullptr) noteDispatchInputWriters(*use->dispatchInputs, frozen->queue);
            } else if (dispatch && verifyPrepareDispatches() && use->dispatchInputs != nullptr) {
                auto fresh = prepareFrozen(*frozen, true);
                if (fresh->refused != FrozenFallback::None) throw std::logic_error("APS5_VERIFY_PREPARE_DISPATCHES: the in-order preparation refused a dispatch a worker prepared");
                if (fresh->dispatchInputs == nullptr) throw std::runtime_error("APS5_VERIFY_PREPARE_DISPATCHES: the in-order preparation prepared nothing where a worker prepared a dispatch");
                const auto touchedEarlier = [&](std::uint64_t address, std::size_t bytes) {
                    const auto earlier = recordHistory().Between(use->start, ticket - 1);
                    if (!earlier) return true;
                    const std::array<PreparedRead, 1> read{PreparedRead{address, bytes}};
                    return StaleAfterEach(*earlier, read, pendingWriteOverlaps) != DrawPrepareStale::No;
                };
                if (staleDispatchInputs(*use->dispatchInputs, *fresh->dispatchInputs, touchedEarlier)) {
                    ++counters.dispatchVerifyStale;
                    use = std::move(fresh);
                } else {
                    ++counters.dispatchVerified;
                }
            }
            if (!use->rejected.empty()) throw std::runtime_error(use->rejected);
            if (use->dispatchInputs != nullptr) recordDispatchInputs(*use->dispatchInputs, use->device);
            if (use->inputs != nullptr) recordDrawInputs(*use->inputs, *result, use->device);
        } catch (const std::exception& failure) {
            const char* what = !frozen->dispatch ? "draw" : frozen->indirectArguments != 0 ? "indirect dispatch" : "dispatch";
            throw std::runtime_error(frozenIdentity(what, frozen->serial, frozen->queue, frozen->offset, use != nullptr ? use->program : 0) + ": " + failure.what());
        }
        GuestMemory::LogDriverStores(nullptr);
        result->effects.writeGenerationAfter = Graphics::Recorder::WriteGeneration();
    });
    std::lock_guard lock(frozenPostedMutex);
    frozenPosted.push_back(std::move(result));
}

void Driver::emitFrozenLabels(const std::shared_ptr<FrozenDraw>& frozen) {
    auto result = std::make_shared<DrawRecordResult>();
    result->ticket = drawRecords().Post([this, frozen, result] {
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
        auto labels = frozen->labels;
        const auto localDevice = device.Load();
        if (!labels.empty()) recordLabels(localDevice.get(), frozen->queue, labels);
        if (frozen->endOfPipe) {
            const auto outcome = recordEndOfPipe(localDevice.get(), frozen->queue, frozen->packet);
            if (!outcome.wroteOnGpu) {
                if (localDevice != nullptr) localDevice->WaitIdle();
                if (const auto label = Pm4::DecodeLabelWrite(frozen->packet)) {
                    GuestMemory::Write(label->address, label->Bytes(), 4);
                    noteLabelStore(label->address, label->Bytes(), ++eventSerial, frozen->queue);
                }
            }
            if (!outcome.interruptDeferred) AgcDriverDeliverEopInterrupt(frozen->queue);
        }
        GuestMemory::LogDriverStores(nullptr);
        result->effects.writeGenerationAfter = Graphics::Recorder::WriteGeneration();
    });
    std::lock_guard lock(frozenPostedMutex);
    frozenPosted.push_back(std::move(result));
}

bool Driver::freezeLabels(std::uint32_t queue, bool force) {
    if (!LabelRecordJobs() || prepareWorkers() == 0 || !deferDrawRecords(queue) || frozenSink() != nullptr || GuestMemory::GpuMutex().HeldByThisThread()) return false;
    auto& deferred = deferredLabels();
    if (deferred.labels.empty()) return false;
    if (!force && !drawRecords().Pending() && frozenDraws().Idle()) return false;
    frozenProducer() = true;
    surfaceRecordFailure();
    startDrawRecords();
    auto frozen = std::make_shared<FrozenDraw>();
    frozen->labels.swap(deferred.labels);
    frozen->queue = queue;
    frozen->offset = currentPacketOffset();
    frozen->epoch = GuestMemory::CollectEpoch();
    Graphics::Recorder::ForgetQueuedLabels();
    queuedLabelsNoted() = 0;
    auto& records = drawRecords();
    if (const auto ticket = AheadWaitTicket(records.Posted(), records.Finished(), frozenBound()); ticket != 0) {
        ++drawRecordCounters().recordsFull;
        records.WaitFinished(ticket);
    }
    auto& counts = packetWaitCounts();
    counts.labelJobs.fetch_add(1, std::memory_order_relaxed);
    counts.labelJobLabels.fetch_add(frozen->labels.size(), std::memory_order_relaxed);
    frozenDraws().Submit(std::move(frozen));
    return true;
}

bool Driver::freezeEndOfPipe(std::uint32_t queue, std::span<const std::uint32_t> packet) {
    if (!LabelRecordJobs() || prepareWorkers() == 0 || !deferDrawRecords(queue) || frozenSink() != nullptr || GuestMemory::GpuMutex().HeldByThisThread() || device.Load() == nullptr) return false;
    if (!drawRecords().Pending() && frozenDraws().Idle()) return false;
    frozenProducer() = true;
    surfaceRecordFailure();
    startDrawRecords();
    auto frozen = std::make_shared<FrozenDraw>();
    frozen->labels.swap(deferredLabels().labels);
    frozen->packet.assign(packet.begin(), packet.end());
    frozen->endOfPipe = true;
    frozen->queue = queue;
    frozen->offset = currentPacketOffset();
    frozen->epoch = GuestMemory::CollectEpoch();
    Graphics::Recorder::ForgetQueuedLabels();
    queuedLabelsNoted() = 0;
    auto& records = drawRecords();
    if (const auto ticket = AheadWaitTicket(records.Posted(), records.Finished(), frozenBound()); ticket != 0) {
        ++drawRecordCounters().recordsFull;
        records.WaitFinished(ticket);
    }
    auto& counts = packetWaitCounts();
    counts.endOfPipeJobs.fetch_add(1, std::memory_order_relaxed);
    counts.labelJobLabels.fetch_add(frozen->labels.size(), std::memory_order_relaxed);
    frozenDraws().Submit(std::move(frozen));
    return true;
}

bool Driver::deferredLabelPacket(std::span<const std::uint32_t> packet) const {
    static const bool drainAll = std::getenv("APS5_DRAIN_ALL") != nullptr;
    if (drainAll || !LabelRecordJobs() || prepareWorkers() == 0 || !DeferLabels() || packet.empty()) return false;
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    if (opcode != 0x49 && opcode != 0x37) return false;
    if (opcode == 0x49 && packet.size() > 4) {
        const auto interruptSelect = (packet[2] >> 24u) & 7u;
        const bool storesNothing = (packet[2] >> 29u) == 0 || (packet[3] | (static_cast<std::uint64_t>(packet[4]) << 32u)) == 0;
        if (interruptSelect != 0 && interruptSelect != 3) {
            if (!storesNothing && !Pm4::DecodeLabelWrite(packet).has_value()) return false;
            packetWaitCounts().labelPackets.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    const auto label = Pm4::DecodeLabelWrite(packet);
    if (!label.has_value()) return false;
    const auto bytes = label->Bytes();
    if (bytes.size() > DeferredLabel::Capacity || bytes.size() % 4 != 0 || label->address % 4 != 0) return false;
    packetWaitCounts().labelPackets.fetch_add(1, std::memory_order_relaxed);
    return true;
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
    static const bool enabled = recordThread() && DefaultOnSwitch("APS5_RECORD_THREAD_DISPATCH");
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
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        if (profile) drawEnteredAt() = std::chrono::steady_clock::now();
        freezeLabels(submission.queue, true);
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
    if (drawRecordState().active || resolvingAhead() || frozenSink() != nullptr || !deferDispatchRecords(submission.queue)) {
        dispatchNow(queue, packet, submission, indirectArguments);
        return;
    }
    if (prepareDispatches()) {
        const auto begin = std::chrono::steady_clock::now();
        freezeLabels(submission.queue, true);
        const auto reason = dispatchFrozenFallback(queue, packet, submission, indirectArguments);
        if (reason == FrozenFallback::None) {
            freezeDispatch(queue, packet, submission, indirectArguments, begin);
            return;
        }
        ++drawRecordCounters().dispatchInline[static_cast<std::size_t>(reason)];
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
        aheadStoreSequence() = GuestMemory::DriverStoreSequence();
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
    const auto checkedSequence = GuestMemory::DriverStoreSequence();
    if (lateStale(prepared, aheadStoreSequence(), false) != FrozenPrepareVerdict::Fresh) {
        ++counters.staleStored;
        ++counters.aheadStalePost;
        state.stale = true;
        return false;
    }
    if (finished < state.overlapped) drawRecordWindow.Hold({std::move(prepared), drawRecordWindow.Between(finished, state.overlapped), checkedSequence});
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

PacketWaitCounts& packetWaitCounts() {
    static PacketWaitCounts counts;
    return counts;
}

std::uint32_t& packetWaitOpcode() {
    thread_local std::uint32_t opcode = 0x102u;
    return opcode;
}

void Driver::waitDrawRecordsAt(std::uint32_t opcode) {
    packetWaitOpcode() = std::min<std::uint32_t>(opcode, 0x102u);
    struct Reset {
        ~Reset() { packetWaitOpcode() = 0x102u; }
    } reset;
    waitDrawRecords();
}

CommandProcessorReads ParseCommandProcessorReads(const char* text) {
    if (text == nullptr) return CommandProcessorReads::Overlap;
    const std::string_view value(text);
    if (value == "0" || value == "off") return CommandProcessorReads::Drain;
    if (value == "1" || value == "on") return CommandProcessorReads::Overlap;
    if (value == "verify") return CommandProcessorReads::Verify;
    throw std::invalid_argument("APS5_CP_READS_OVERLAP must be 0, 1, off, on or verify, not \"" + std::string(value) + "\"");
}

CommandProcessorReads CommandProcessorReadsMode() {
    static const auto parsed = ParseCommandProcessorReads(std::getenv("APS5_CP_READS_OVERLAP"));
    const auto forced = commandProcessorReadsOverride().load(std::memory_order_relaxed);
    return forced >= 0 ? static_cast<CommandProcessorReads>(forced) : parsed;
}

void SetCommandProcessorReadsMode(CommandProcessorReads mode) {
    commandProcessorReadsOverride().store(static_cast<int>(mode), std::memory_order_relaxed);
}

LabelRecordJobCounters LabelRecordJobTotals() {
    const auto& counts = packetWaitCounts();
    return {counts.labelPackets.load(std::memory_order_relaxed), counts.labelJobs.load(std::memory_order_relaxed), counts.labelJobLabels.load(std::memory_order_relaxed), counts.endOfPipeJobs.load(std::memory_order_relaxed)};
}

CommandProcessorReadCounters CommandProcessorReadTotals() {
    const auto& counts = packetWaitCounts();
    return {counts.commandProcessorReads.load(std::memory_order_relaxed), counts.commandProcessorReadsAhead.load(std::memory_order_relaxed), counts.commandProcessorReadsVerified.load(std::memory_order_relaxed), counts.registerStatePackets.load(std::memory_order_relaxed)};
}

bool Driver::overlapCommandProcessorRead(std::span<const std::uint32_t> packet) {
    const auto mode = CommandProcessorReadsMode();
    if (mode == CommandProcessorReads::Drain || packet.empty()) return false;
    if (RegisterStatePacket(packet[0])) {
        packetWaitCounts().registerStatePackets.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const auto read = Pm4::CommandProcessorReadOf(packet);
    if (!read) return false;
    auto& counts = packetWaitCounts();
    counts.commandProcessorReads.fetch_add(1, std::memory_order_relaxed);
    const bool ahead = drawRecords().Pending() || !frozenDraws().Idle();
    if (ahead) counts.commandProcessorReadsAhead.fetch_add(1, std::memory_order_relaxed);
    if (mode == CommandProcessorReads::Overlap || !ahead) return true;
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    std::vector<std::byte> early(read->bytes);
    std::vector<std::byte> late(read->bytes);
    {
        const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Registers);
        GuestMemory::Read(read->address, early, 4);
    }
    waitDrawRecordsAt(opcode);
    {
        const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Registers);
        GuestMemory::Read(read->address, late, 4);
    }
    counts.commandProcessorReadsVerified.fetch_add(1, std::memory_order_relaxed);
    const auto differs = std::mismatch(early.begin(), early.end(), late.begin());
    if (differs.first == early.end()) return true;
    const auto offset = static_cast<std::size_t>(differs.first - early.begin());
    char what[256];
    std::snprintf(what, sizeof(what), "APS5_CP_READS_OVERLAP=verify: packet 0x%02x read 0x%llx+0x%zx ahead of the draw records, and byte 0x%zx changed from 0x%02x to 0x%02x once they were recorded", opcode, static_cast<unsigned long long>(read->address), read->bytes, offset, static_cast<unsigned>(*differs.first), static_cast<unsigned>(*differs.second));
    throw std::runtime_error(what);
}

void Driver::waitDrawRecords() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto opcode = packetWaitOpcode();
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto frozenPending = profile && !frozenDraws().Idle();
    drainFrozen(false);
    mergeFrozenRecords();
    auto& records = drawRecords();
    const bool pending = records.Pending();
    if (pending) {
        ++drawRecordCounters().packetWaits;
        if (GuestMemory::GpuMutex().HeldByThisThread()) runDrawRecords();
        else records.WaitIdle();
    }
    if (profile && (pending || frozenPending)) {
        auto& counts = packetWaitCounts();
        counts.waits[opcode].fetch_add(1, std::memory_order_relaxed);
        counts.waitedNs[opcode].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
    }
    if (auto& state = drawRecordState(); state.active && !state.settled) state.settled = true;
    Graphics::Recorder::DrainChunks();
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
                auto verdict = StaleAfterEach(provisional.earlier, provisional.reads, pendingWriteOverlaps);
                if (verdict == DrawPrepareStale::No && lateStale(provisional.reads, provisional.storeSequence) != FrozenPrepareVerdict::Fresh) verdict = DrawPrepareStale::Stored;
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
    {
        auto& counts = packetWaitCounts();
        std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint32_t>> rows;
        for (std::uint32_t opcode = 0; opcode < counts.waits.size(); ++opcode) {
            const auto waits = counts.waits[opcode].exchange(0, std::memory_order_relaxed);
            const auto waitedNs = counts.waitedNs[opcode].exchange(0, std::memory_order_relaxed);
            if (waits != 0) rows.emplace_back(waitedNs, waits, opcode);
        }
        std::sort(rows.begin(), rows.end(), std::greater<>{});
        std::string text;
        for (std::size_t i = 0; i < rows.size() && i < 12; ++i) {
            const auto [waitedNs, waits, opcode] = rows[i];
            char item[96];
            const char* name = opcode == 0x100u ? "flip" : opcode == 0x101u ? "rendering wait" : opcode == 0x102u ? "not a packet" : nullptr;
            if (name != nullptr) std::snprintf(item, sizeof(item), " %s x%llu %.1fms", name, static_cast<unsigned long long>(waits), static_cast<double>(waitedNs) / 1e6);
            else std::snprintf(item, sizeof(item), " 0x%02x x%llu %.1fms", opcode, static_cast<unsigned long long>(waits), static_cast<double>(waitedNs) / 1e6);
            text += item;
        }
        static CommandProcessorReadCounters lastReads{};
        const auto reads = CommandProcessorReadTotals();
        ProfilePrint_nid_no_patch("[record-thread] drains by packet (10 s, waits and time):%s; command processor reads without a drain: %llu, %llu of them ahead of pending records, %llu verified; register-state packets without a drain: %llu\n", text.empty() ? " none" : text.c_str(), static_cast<unsigned long long>(reads.reads - lastReads.reads), static_cast<unsigned long long>(reads.ahead - lastReads.ahead), static_cast<unsigned long long>(reads.verified - lastReads.verified), static_cast<unsigned long long>(reads.registerState - lastReads.registerState));
        lastReads = reads;
        static std::array<std::uint64_t, 4> lastLabels{};
        const std::array<std::uint64_t, 4> labelTotals{counts.labelPackets.load(std::memory_order_relaxed), counts.labelJobs.load(std::memory_order_relaxed), counts.labelJobLabels.load(std::memory_order_relaxed), counts.endOfPipeJobs.load(std::memory_order_relaxed)};
        if (LabelRecordJobs()) ProfilePrint_nid_no_patch("[record-thread] labels as ordered record jobs (10 s): %llu label packets without a drain, %llu jobs recorded %llu deferred labels, %llu end-of-pipe labels and interrupts recorded as jobs\n", static_cast<unsigned long long>(labelTotals[0] - lastLabels[0]), static_cast<unsigned long long>(labelTotals[1] - lastLabels[1]), static_cast<unsigned long long>(labelTotals[2] - lastLabels[2]), static_cast<unsigned long long>(labelTotals[3] - lastLabels[3]));
        lastLabels = labelTotals;
    }
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
        if (prepareDispatches()) {
            static std::array<std::uint64_t, Reasons> lastDispatchInline{};
            static std::array<std::uint64_t, Reasons> lastDispatchOrdered{};
            static std::array<std::uint64_t, 7> lastDispatch{};
            const std::array<std::uint64_t, 7> dispatchValues{counters.frozenDispatches.load(), counters.frozenDispatchNs.load(), counters.dispatchRedoStale.load(), counters.dispatchRedoUnchecked.load(), counters.dispatchRedoErrors.load(), counters.dispatchVerified.load(), counters.dispatchVerifyStale.load()};
            std::array<std::uint64_t, 7> dispatchDelta{};
            for (std::size_t i = 0; i < dispatchValues.size(); ++i) dispatchDelta[i] = dispatchValues[i] - lastDispatch[i];
            lastDispatch = dispatchValues;
            const auto dispatchInlineText = reasons(counters.dispatchInline, lastDispatchInline);
            const auto dispatchOrderedText = reasons(counters.dispatchOrdered, lastDispatchOrdered);
            char dispatchText[512];
            std::snprintf(dispatchText, sizeof(dispatchText), "; prepare dispatches: %llu frozen (check and freeze %.2f us each); inline on queue 0 after a drain: %s; redone in order on the record thread: stale %llu, unchecked %llu, after a worker error %llu, refused by a worker: %s; verified %llu, %llu recorded from the in-order preparation after a CPU store", static_cast<unsigned long long>(dispatchDelta[0]), dispatchDelta[0] != 0 ? static_cast<double>(dispatchDelta[1]) / 1000.0 / static_cast<double>(dispatchDelta[0]) : 0.0, dispatchInlineText.c_str(), static_cast<unsigned long long>(dispatchDelta[2]), static_cast<unsigned long long>(dispatchDelta[3]), static_cast<unsigned long long>(dispatchDelta[4]), dispatchOrderedText.c_str(), static_cast<unsigned long long>(dispatchDelta[5]), static_cast<unsigned long long>(dispatchDelta[6]));
            ahead += dispatchText;
        }
    }
    ProfilePrint_nid_no_patch("[record-thread] %.3f s: %llu draws, %llu dispatches (%llu overlapped a record), %llu jobs run (%llu on the record thread, %llu inline on queue 0, %llu failed) %.1f ms, %.1f us per job; producer waits %llu (%llu blocked, %.1f ms); %llu packet waits; stale preparations: %llu pending write, %llu stored, %llu unchecked, %llu redone%s\n", seconds, static_cast<unsigned long long>(delta[0]), static_cast<unsigned long long>(delta[7]), static_cast<unsigned long long>(delta[1]), static_cast<unsigned long long>(jobs), static_cast<unsigned long long>(counts.byWorker - lastCounts.byWorker), static_cast<unsigned long long>(counts.byOthers - lastCounts.byOthers), static_cast<unsigned long long>(counts.failed - lastCounts.failed), jobMs, jobs != 0 ? jobMs * 1000 / static_cast<double>(jobs) : 0.0, static_cast<unsigned long long>(counts.waits - lastCounts.waits), static_cast<unsigned long long>(counts.blockedWaits - lastCounts.blockedWaits), waitedMs, static_cast<unsigned long long>(delta[6]), static_cast<unsigned long long>(delta[2]), static_cast<unsigned long long>(delta[3]), static_cast<unsigned long long>(delta[4]), static_cast<unsigned long long>(delta[5]), ahead.c_str());
    lastCounts = counts;
}

}

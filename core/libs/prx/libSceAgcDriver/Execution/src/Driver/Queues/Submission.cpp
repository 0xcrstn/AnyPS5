#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Coherence.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/SubmissionRing.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/SubmissionCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/ThreadPriority.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <span>
#include <utility>
#include <vector>
#include "ThreadOwned.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace AgcDriver::DriverDetail {

namespace {

std::uint64_t captureTime() {
    return static_cast<std::uint64_t>(std::llround(TraceMs() * 1000.0));
}

void writeCaptured(Driver& driver, SubmissionCapture& capture, const CapturedSubmission& captured) {
    try {
        capture.Write(captured);
    } catch (...) {
        driver.ReportFailure(std::current_exception());
        throw;
    }
}

bool EopRelease(std::span<const std::uint32_t> words) {
    return words.size() >= 8 && (words[0] >> 30u) == 3u && Pm4::PacketWords(words[0]) == 8u && ((words[0] >> 8u) & 0xffu) == 0x49u && ((words[2] >> 24u) & 7u) == 2u;
}

}

void Submission::InitializeTrace(DrawTrace::Context origin) {
    if (!DrawTrace::Enabled()) return;
    if (origin.submission == 0 && traceContext.submission != 0) origin = traceContext;
    if (origin.driver == 0) {
        static const auto driver = DrawTrace::NextId();
        origin.driver = driver;
    }
    if (origin.submission == 0) origin.submission = DrawTrace::NextId();
    if (traceContext.submission == 0 || origin.submission != traceContext.submission) origin.segment = DrawTrace::NextId();
    else origin.segment = traceContext.segment;
    origin.packet = origin.operation = origin.job = 0;
    origin.queue = queue;
    origin.packetOffset = 0;
    origin.opcode = 0xffffffffu;
    traceContext = origin;
    DrawTrace::Link(origin.submission, origin.segment, "contains");
    tracePackets.clear();
    for (std::size_t at = 0; at < commands.size();) {
        const auto header = commands[at];
        const auto words = Pm4::FillerPacket(header) ? std::size_t{1} : Pm4::PacketWords(header);
        if (words == 0 || words > commands.size() - at) break;
        auto packet = origin;
        packet.packet = DrawTrace::NextId();
        packet.packetOffset = at;
        packet.opcode = (header >> 8u) & 0xffu;
        tracePackets.emplace(at, packet);
        at += words;
    }
}

DrawTrace::Context Submission::TraceAt(std::size_t offset) const {
    if (!DrawTrace::Enabled()) return {};
    if (const auto found = tracePackets.find(offset); found != tracePackets.end()) return found->second;
    return traceContext;
}

std::deque<const std::uint32_t*>& Driver::releasedTails() {
    struct ReleasedTailsStorage {};
    return HostThreadLocal<std::deque<const std::uint32_t*>, ReleasedTailsStorage>();
}

void Driver::noteReleasedTails(const Submission& submission) {
    auto& released = releasedTails();
    released.clear();
    thread_local Submission* scratchSlot = nullptr;
    auto& scratch = ShaderRecompiler::ThreadOwned(scratchSlot);
    const std::uint32_t* tail = submission.rewindTail;
    std::size_t words = submission.rewindWords;
    for (std::size_t chunk = 0; chunk < 256 && tail != nullptr; ++chunk) {
        if ((std::atomic_ref<std::uint32_t>(*const_cast<std::uint32_t*>(tail - 1)).load(std::memory_order_acquire) & 0x80000000u) == 0) break;
        released.push_back(tail - 1);
        scratch.rewindTail = nullptr;
        scratch.rewindWords = 0;
        try {
            copyCommands(scratch, tail, words);
        } catch (const std::exception&) {
            break;
        }
        tail = scratch.rewindTail;
        words = scratch.rewindWords;
    }
}

void Driver::copyCommands(Submission& submission, const std::uint32_t* guest, std::size_t words) {
    submission.commands.clear();
    submission.conditionalEnds.clear();
    std::size_t budget = std::size_t{1} << 26u;
    copySegment(submission, guest, words, budget);
}

bool Driver::copySegment(Submission& submission, const std::uint32_t* guest, std::size_t words, std::size_t& budget) {
    require(words <= budget, "command buffer jumps exceed the copy limit (a jump loop?)");
    budget -= words;
    const auto* guestAddress = guest;
    std::vector<std::uint32_t> copied(words);
    GuestMemory::CopyDriverMemory(copied.data(), guest, copied.size() * sizeof(std::uint32_t));
    guest = copied.data();
    std::vector<std::pair<std::size_t, std::size_t>> guarded;
    const auto reach = [&](std::size_t cursor, std::size_t next) {
        std::erase_if(guarded, [&](const auto& range) {
            if (range.first != cursor) return false;
            submission.conditionalEnds.emplace(range.second, submission.commands.size());
            return true;
        });
        for (const auto& range : guarded) require(range.first >= next, "conditional execution range ends inside a packet");
    };
    for (std::size_t cursor = 0; cursor < words;) {
        const auto header = guest[cursor];
        if (Pm4::FillerPacket(header)) {
            reach(cursor, cursor + 1);
            submission.commands.push_back(header);
            ++cursor;
            continue;
        }
        const auto count = (header & 0xc0000000u) == 0xc0000000u ? Pm4::PacketWords(header) : words - cursor;
        if ((header & 0xc0000000u) != 0xc0000000u || count > words - cursor) {
            submission.commands.insert(submission.commands.end(), guest + cursor, guest + words);
            return false;
        }
        reach(cursor, cursor + count);
        const auto opcode = (header >> 8u) & 0xffu;
        if (opcode == 0x3fu && count == 14) {
            const auto mode = guest[cursor + 1] & 3u;
            require(mode == 1u || mode == 2u, "invalid COND_INDIRECT_BUFFER mode");
            require(((guest[cursor + 1] >> 8u) & 7u) == 0u, "a COND_INDIRECT_BUFFER with a comparison is not implemented");
            require(!Pm4::Predicated(header), "a predicated COND_INDIRECT_BUFFER is not implemented");
            const auto* target = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(guest[cursor + 8] & ~3u) | (static_cast<std::uintptr_t>(guest[cursor + 9] & 0xffffu) << 32u));
            const std::size_t targetWords = guest[cursor + 10] & 0xfffffu;
            if (targetWords != 0) {
                GuestMemory::CheckRange(target, targetWords * sizeof(std::uint32_t), alignof(std::uint32_t));
                if (copySegment(submission, target, targetWords, budget)) {
                    require(guarded.empty(), "a REWIND inside a conditional execution range is not implemented");
                    return true;
                }
            }
            cursor += count;
            continue;
        }
        if (opcode == 0x3fu) {
            require(count == 4, "invalid INDIRECT_BUFFER size");
            const auto* target = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(guest[cursor + 1] & ~3u) | (static_cast<std::uintptr_t>(guest[cursor + 2] & 0xffffu) << 32u));
            const std::size_t targetWords = guest[cursor + 3] & 0xfffffu;
            const bool chain = (guest[cursor + 3] & (1u << 20u)) != 0;
            require(!chain || guarded.empty(), "a chained INDIRECT_BUFFER inside a conditional execution range is not implemented");
            GuestMemory::CheckRange(target, targetWords * sizeof(std::uint32_t), alignof(std::uint32_t));
            if (Pm4::Predicated(header)) {
                require(!chain, "predicated command buffer chains are not implemented");
                const auto start = submission.commands.size();
                submission.commands.insert(submission.commands.end(), guest + cursor, guest + cursor + count);
                require(!copySegment(submission, target, targetWords, budget), "a REWIND inside a predicated command buffer is not implemented");
                for (auto inner = start + count; inner < submission.commands.size(); inner += Pm4::PacketWords(submission.commands[inner])) {
                    const auto innerHeader = submission.commands[inner];
                    require(innerHeader != FlipPacketHeader && innerHeader != RenderingWaitPacketHeader, "flips and rendering waits in predicated command buffers are not implemented");
                }
                submission.conditionalEnds.emplace(start, submission.commands.size());
                cursor += count;
                continue;
            }
            if (copySegment(submission, target, targetWords, budget)) {
                require(guarded.empty(), "a REWIND inside a conditional execution range is not implemented");
                return true;
            }
            if (chain) return false;
            cursor += count;
            continue;
        }
        if (opcode == 0x22u && count == 5) guarded.emplace_back(cursor + count + Pm4::ConditionalWords(std::span(guest + cursor, count)), submission.commands.size());
        submission.commands.insert(submission.commands.end(), guest + cursor, guest + cursor + count);
        cursor += count;
        if (opcode == 0x59u) {
            require(guarded.empty(), "a REWIND inside a conditional execution range is not implemented");
            submission.rewindTail = guestAddress + cursor;
            submission.rewindWords = words - cursor;
            return true;
        }
    }
    reach(words, words);
    require(guarded.empty(), "conditional execution range exceeds its command buffer");
    return false;
}

namespace {

std::atomic<std::uint64_t>& deferredRegisterLists() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

}

std::uint64_t Driver::DeferredRegisterLists() {
    return deferredRegisterLists().load(std::memory_order_relaxed);
}

RegisterListSnapshot RegisterListSnapshot::Capture(std::span<const std::uint32_t> packet) {
    const auto read = Pm4::CommandProcessorReadOf(packet);
    require(read.has_value() && Pm4::IndirectRegisterOpcode((packet[0] >> 8u) & 0xffu), "expected an indirect register-list packet");
    GuestMemory::FlushGpuWrites(read->address, read->bytes);
    std::lock_guard gpu(GuestMemory::GpuMutex());
    RegisterListSnapshot snapshot;
    snapshot.values = Pm4::ReadIndirectRegisterWords(packet, &snapshot.writes);
    return snapshot;
}

std::vector<std::uint32_t> RegisterListSnapshot::Resolve(std::span<const std::uint32_t> packet) const {
    const auto read = Pm4::CommandProcessorReadOf(packet);
    require(read.has_value() && Pm4::IndirectRegisterOpcode((packet[0] >> 8u) & 0xffu), "expected an indirect register-list packet");
    GuestMemory::FlushGpuWrites(read->address, read->bytes);
    std::lock_guard gpu(GuestMemory::GpuMutex());
    require(writes != nullptr, "the register-list snapshot has no driver-store watch");
    if (GuestMemory::DriverSnapshotUnchanged(*writes)) {
        if (packet[3] == 0x80000000u) return values;
        return Pm4::IndirectRegisterPairs(packet, values);
    }
    auto updated = values;
    GuestMemory::ApplyDriverSnapshot(*writes, std::as_writable_bytes(std::span(updated)));
    if (packet[3] == 0x80000000u) return updated;
    return Pm4::IndirectRegisterPairs(packet, updated);
}

void Driver::readRegisterLists(Submission& submission) {
    submission.registerLists.clear();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> own;
    const auto storedBefore = [&](std::uint64_t address, std::size_t bytes) {
        const auto overlaps = [&](const std::pair<std::uint64_t, std::uint64_t>& range) { return address < range.second && range.first < address + bytes; };
        return std::any_of(own.begin(), own.end(), overlaps);
    };
    for (std::size_t cursor = 0; cursor < submission.commands.size(); cursor += Pm4::PacketWords(submission.commands[cursor])) {
        const auto header = submission.commands[cursor];
        if ((header >> 30u) != 3u) continue;
        const auto packet = std::span<const std::uint32_t>(submission.commands).subspan(cursor, Pm4::PacketWords(header));
        if (!Pm4::IndirectRegisterOpcode((header >> 8u) & 0xffu)) {
            for (const auto& store : Pm4::DecodeStores(packet)) {
                if (store.bytes != 0) own.emplace_back(store.address, store.address + store.bytes);
            }
            continue;
        }
        if (const auto read = Pm4::CommandProcessorReadOf(packet); read.has_value() && storedBefore(read->address, read->bytes)) {
            deferredRegisterLists().fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        try {
            submission.registerLists.emplace(cursor, RegisterListSnapshot::Capture(packet));
        } catch (const std::exception& error) {
            throw std::runtime_error("AGC driver: " + Pm4::Name(header) + " at DWORD " + std::to_string(cursor) + ": " + error.what());
        }
    }
}

void Driver::waitForFlipRoom(const Submission& submission) {
    for (std::size_t cursor = 0; cursor < submission.commands.size(); cursor += Pm4::PacketWords(submission.commands[cursor])) {
        if (submission.commands[cursor] != FlipPacketHeader) continue;
        std::shared_ptr<IVideoOutput> output;
        {
            std::lock_guard lock(mutex);
            const auto found = outputs.find(submission.commands[cursor + 1]);
            require(found != outputs.end(), "flip references an unregistered video output");
            output = found->second;
        }
        output->WaitForFlipRoom();
    }
}

bool Driver::awaitsTitle(std::uint64_t awaited) const {
    return runningWorkers.load(std::memory_order_acquire) == 0 && orderHolders.load(std::memory_order_acquire) == 0 && !completionsPending() && !Graphics::Recorder::SnapshotWriteOverlaps(awaited, 4);
}

void Driver::noteAwaitingTitle(std::uint32_t queue, std::uint64_t awaited) {
    if (queue != 0 || flipHolders.load(std::memory_order_acquire) == 0 || queue0AwaitsTitle.load(std::memory_order_acquire) || !awaitsTitle(awaited)) return;
    std::lock_guard lock(mutex);
    queue0AwaitsTitle.store(true, std::memory_order_release);
    changed.notify_all();
}

void Driver::holdFlipBehindWorker(const Submission& submission) {
    if (submission.queue != 0 || onWorkerThread()) return;
    bool candidate = false;
    for (std::size_t cursor = 0; cursor < submission.commands.size() && !candidate; cursor += Pm4::PacketWords(submission.commands[cursor])) {
        const auto words = std::span(submission.commands).subspan(cursor);
        candidate = words[0] == FlipPacketHeader || EopRelease(words);
    }
    if (!candidate) return;
    std::unique_lock lock(mutex);
    bool flips = false;
    for (std::size_t cursor = 0; cursor < submission.commands.size() && !flips; cursor += Pm4::PacketWords(submission.commands[cursor])) {
        const auto words = std::span(submission.commands).subspan(cursor);
        flips = words[0] == FlipPacketHeader || (EopRelease(words) && eopFlips.contains(words[7]));
    }
    if (!flips) return;
    DrawTrace::Span flipHoldWait("Driver.FlipHoldWait");
    DrawTrace::Note("wait_reason", "prior_flip_reservation");
    DrawTrace::Note("wait_dependency", "prior_flip_identity_unavailable");
    flipHolders.fetch_add(1, std::memory_order_acq_rel);
    changed.wait(lock, [&] { return failure != nullptr || stopping.load(std::memory_order_acquire) || shutdownToken.stop_requested() || flipsAhead.load(std::memory_order_acquire) == 0 || queue0AwaitsTitle.load(std::memory_order_acquire); });
    DrawTrace::Note("wait_outcome", failure != nullptr ? "failed" : stopping.load(std::memory_order_acquire) || shutdownToken.stop_requested() ? "cancelled" : "satisfied");
    flipHolders.fetch_sub(1, std::memory_order_acq_rel);
}

void Driver::releaseFlipHold() {
    std::lock_guard lock(mutex);
    flipsAhead.fetch_sub(1, std::memory_order_acq_rel);
    if (flipHolders.load(std::memory_order_acquire) != 0) changed.notify_all();
}

void Driver::reserveOutputs(Submission& submission) {
    std::set<std::uint32_t> eopTokens;
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        const auto* words = submission.commands.data() + cursor;
        if (words[0] == RenderingWaitPacketHeader) {
            const auto output = outputs.find(words[1]);
            require(output != outputs.end(), "rendering wait references an unregistered video output");
            auto wait = output->second->CaptureRenderingWait(words[2]);
            require(wait != nullptr, "video output returned a null rendering wait");
            submission.renderingWaits.emplace(cursor, std::move(wait));
        }
        if (EopRelease(std::span(submission.commands).subspan(cursor))) {
            const auto token = words[7];
            const auto flip = eopFlips.find(token);
            if (flip != eopFlips.end()) {
                require(submission.queue == 0, "EOP flips on async queues are not implemented");
                require(eopTokens.insert(token).second, "EOP flip token repeated in a submission");
                submission.flips.emplace(cursor, flip->second.request);
            }
        }
        noteHeldAtSubmit(submission, cursor);
        if (words[0] == FlipPacketHeader) {
            const auto output = outputs.find(words[1]);
            require(output != outputs.end(), "flip references an unregistered video output");
            const FlipInfo info{words[1], std::bit_cast<std::int32_t>(words[2]), words[3], std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(words[4]) | (static_cast<std::uint64_t>(words[5]) << 32u))};
            auto request = output->second->Reserve(info);
            require(request != nullptr, "video output returned a null flip reservation");
            submission.flips.emplace(cursor, std::move(request));
        }
        cursor += Pm4::PacketWords(words[0]);
    }
    for (const auto token : eopTokens) eopFlips.erase(token);
}

void Driver::executeRewindTail(const Submission& stalled) {
    PerformanceTimer timing("Driver.Rewind");
    static const bool ring = ReadSubmissionRingSettings();
    if (ring) PrepareSubmissionRing(stalled.queue);
    std::atomic_ref<std::uint32_t> control(*const_cast<std::uint32_t*>(stalled.rewindTail - 1));
    if ((control.load(std::memory_order_acquire) & Pm4::RewindValid) == 0) {
        DrawTrace::Span rewindWait("Driver.RewindControlWait");
        DrawTrace::Note("wait_reason", "guest_rewind_valid");
        DrawTrace::Note("wait_address", reinterpret_cast<std::uint64_t>(stalled.rewindTail - 1));
        noteWaitBlocked(stalled.queue, reinterpret_cast<std::uint64_t>(stalled.rewindTail - 1), true);
        struct BlockedWait {
            Driver& driver;
            std::uint32_t queue;
            ~BlockedWait() { driver.noteWaitBlocked(queue, 0, false); }
        } blocked{*this, stalled.queue};
        StallWatch stall(stalled.queue);
        while ((control.load(std::memory_order_acquire) & Pm4::RewindValid) == 0) {
            CheckFailure();
            checkStopping();
            noteAwaitingTitle(stalled.queue, reinterpret_cast<std::uint64_t>(stalled.rewindTail - 1));
            stall.Check("REWIND at", reinterpret_cast<std::uint64_t>(stalled.rewindTail - 1));
            PollSleep();
        }
        DrawTrace::Note("wait_outcome", "satisfied");
    }
    Submission tail{};
    tail.queue = stalled.queue;
    if (APS5_ENABLE_TIMING_LOG) tail.receivedAt = std::chrono::steady_clock::now();
    tail.InitializeTrace(stalled.traceContext);
    DrawTrace::ContextScope traceContext(tail.traceContext);
    DrawTrace::Record(DrawTrace::EventKind::Segment, tail.traceContext.segment, stalled.traceContext.segment);
    DrawTrace::Link(stalled.traceContext.segment, tail.traceContext.segment, "rewinds_to");
    auto& released = releasedTails();
    if (!released.empty() && released.front() == stalled.rewindTail - 1) {
        released.pop_front();
        tail.enqueuedAt = lastEpochBump();
    } else {
        released.clear();
        tail.enqueuedAt = std::chrono::steady_clock::now();
    }
    copyCommands(tail, stalled.rewindTail, stalled.rewindWords);
    DrawTrace::RecordCounter("command_copy_bytes", tail.commands.size() * sizeof(std::uint32_t), "bytes", tail.traceContext.segment);
    if (APS5_ENABLE_TIMING_LOG) tail.copiedAt = std::chrono::steady_clock::now();
    validate(tail, stalled.rewindTail);
    readRegisterLists(tail);
    tail.InitializeTrace(stalled.traceContext);
    if (APS5_ENABLE_TIMING_LOG) tail.validatedAt = std::chrono::steady_clock::now();
    waitForFlipRoom(tail);
    if (APS5_ENABLE_TIMING_LOG) tail.roomReadyAt = std::chrono::steady_clock::now();
    auto* const capture = SubmissionCapture::Active();
    auto captured = capture != nullptr ? SubmissionCapture::Prepare(tail.queue, SubmissionKind::Rewind, tail.commands, tail.conditionalEnds) : CapturedSubmission{};
    {
        std::lock_guard lock(mutex);
        rethrowFailure();
        checkStopping();
        reserveOutputs(tail);
        tail.shaders = shaders;
        tail.serial = stalled.serial;
        tail.received = ++eventSerial;
        if (capture != nullptr) capture->Take(captured, tail.serial, captureTime(), tail.heldAtSubmit);
        if (ring) RecordSubmission(tail.queue, tail.serial, tail.commands, stalled.rewindTail, true);
    }
    if (capture != nullptr) writeCaptured(*this, *capture, captured);
    if (APS5_ENABLE_TIMING_LOG) tail.enqueuedAt = tail.dequeuedAt = tail.orderedAt = std::chrono::steady_clock::now();
    timing.Finish();
    tail.traceJob = DrawTrace::ScheduleJob("Driver.RewindSegment");
    execute(tail);
}

void Driver::Submit(const Packet* packet, std::uint32_t queue) {
    Submission submission{};
    submission.queue = queue;
    submission.InitializeTrace();
    DrawTrace::ContextScope traceContext(submission.traceContext);
    DrawTrace::BindThread("submission caller");
    DrawTrace::Span submitScope("Driver.Submit");
    DrawTrace::Record(DrawTrace::EventKind::Submission, submission.traceContext.submission);
    DrawTrace::Record(DrawTrace::EventKind::Segment, submission.traceContext.segment);
    const auto receivedAt = APS5_ENABLE_TIMING_LOG ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    CheckFailure();
    require(queue == 0 || (queue >= 0x20 && queue < SubmissionQueueEnd), "unsupported compute queue");
    static const bool ring = ReadSubmissionRingSettings();
    if (ring) PrepareSubmissionRing(queue);
    GuestMemory::CheckRange(packet, sizeof(Packet), alignof(Packet));
    const auto descriptor = *packet;
    require(descriptor.flags == 0, "nonzero submission flags are not implemented");
    submission.receivedAt = receivedAt;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& costs = submissionCosts(queue);
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (descriptor.dw_num != 0) {
        require(descriptor.dw_num <= std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t), "command size overflow");
        GuestMemory::CheckRange(descriptor.addr, static_cast<std::size_t>(descriptor.dw_num) * sizeof(std::uint32_t), alignof(std::uint32_t));
        copyCommands(submission, descriptor.addr, descriptor.dw_num);
    }
    const auto copied = profile ? std::chrono::steady_clock::now() : start;
    DrawTrace::RecordCounter("command_copy_bytes", submission.commands.size() * sizeof(std::uint32_t), "bytes", submission.traceContext.segment);
    if (APS5_ENABLE_TIMING_LOG) submission.copiedAt = std::chrono::steady_clock::now();
    validate(submission, descriptor.addr);
    readRegisterLists(submission);
    submission.InitializeTrace();
    if (APS5_ENABLE_TIMING_LOG) submission.validatedAt = std::chrono::steady_clock::now();
    {
        DrawTrace::Span flipRoomWait("Driver.FlipRoomWait");
        DrawTrace::Note("wait_reason", "video_output_flip_capacity");
        waitForFlipRoom(submission);
        holdFlipBehindWorker(submission);
    }
    {
        DrawTrace::Span preparationWait("Shader.SubmittedPreparationWait");
        DrawTrace::Note("wait_reason", "submitted_shader_preparation_ticket");
        AwaitRegistrationPreparation();
    }
    if (APS5_ENABLE_TIMING_LOG) submission.roomReadyAt = std::chrono::steady_clock::now();
    static const bool trace = std::getenv("APS5_TRACE_GPU") != nullptr;
    if (trace) std::fprintf(stderr, "[gpu] %.1f submit queue=0x%x dwords=%zu at %p\n", TraceMs(), queue, submission.commands.size(), static_cast<const void*>(descriptor.addr));
    const auto validated = profile ? std::chrono::steady_clock::now() : start;
    auto* const capture = SubmissionCapture::Active();
    auto captured = capture != nullptr ? SubmissionCapture::Prepare(queue, SubmissionKind::Submit, submission.commands, submission.conditionalEnds) : CapturedSubmission{};
    {
        std::lock_guard lock(mutex);
        rethrowFailure();
        checkStopping();
        require(accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
        reserveOutputs(submission);
        submission.shaders = shaders;
        submission.serial = accepted + 1;
        DrawTrace::Note("submission_serial", submission.serial);

        submission.received = ++eventSerial;
        const auto now = std::chrono::steady_clock::now();
        submission.enqueuedAt = now;
        if (capture != nullptr) capture->Take(captured, submission.serial, captureTime(), submission.heldAtSubmit);
        if (ring) RecordSubmission(queue, submission.serial, submission.commands, descriptor.addr, false);
        if (profile) {
            ++costs.submissions;
            costs.validateNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(validated - copied).count());
            costs.copyNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>((copied - start) + (now - validated)).count());
        }
        if (APS5_ENABLE_TIMING_LOG) submission.enqueuedAt = std::chrono::steady_clock::now();
        if (queue == 0 && !submission.flips.empty()) {
            submission.holdsFlip = true;
            flipsAhead.fetch_add(1, std::memory_order_acq_rel);
        }
        const auto traceJob = DrawTrace::ScheduleJob("Driver.SubmissionExecution");
        submission.traceJob = traceJob;
        try {
            enqueue(std::move(submission));
        } catch (...) {
            DrawTrace::CancelJob(traceJob, DrawTrace::Status::Failed);
            throw;
        }
        ++accepted;
    }
    changed.notify_all();
    if (capture != nullptr) writeCaptured(*this, *capture, captured);
}

void Driver::SuspendPoint() {
    DrawTrace::BindThread("submission caller");
    DrawTrace::Span suspendScope("Driver.SuspendPoint");
    const auto receivedAt = APS5_ENABLE_TIMING_LOG ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    require(!onWorkerThread(), "worker cannot suspend itself");
    auto* const capture = SubmissionCapture::Active();
    auto captured = capture != nullptr ? SubmissionCapture::Prepare(0, SubmissionKind::Suspend, {}, {}) : CapturedSubmission{};
    std::unique_lock lock(mutex);
    rethrowFailure();
    checkStopping();
    require(accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
    Submission boundary{};
    boundary.serial = accepted + 1;
    boundary.suspend = true;

    boundary.queue = 0;
    boundary.InitializeTrace();
    DrawTrace::ContextScope traceContext(boundary.traceContext);
    DrawTrace::Record(DrawTrace::EventKind::Submission, boundary.traceContext.submission, 0, boundary.serial);
    DrawTrace::Record(DrawTrace::EventKind::Segment, boundary.traceContext.segment);
    boundary.receivedAt = receivedAt;
    boundary.enqueuedAt = std::chrono::steady_clock::now();
    if (capture != nullptr) capture->Take(captured, boundary.serial, captureTime(), boundary.heldAtSubmit);
    const auto traceJob = DrawTrace::ScheduleJob("Driver.SuspendExecution");
    boundary.traceJob = traceJob;
    try {
        enqueue(std::move(boundary));
    } catch (...) {
        DrawTrace::CancelJob(traceJob, DrawTrace::Status::Failed);
        throw;
    }
    ++accepted;

    changed.notify_all();
    if (capture == nullptr) return;
    lock.unlock();
    writeCaptured(*this, *capture, captured);
}

bool Driver::waitFree(const Submission& submission) {
    if (submission.queue == 0 || submission.suspend || !submission.flips.empty() || !submission.renderingWaits.empty() || submission.rewindTail != nullptr) return false;
    for (std::size_t at = 0; at < submission.commands.size(); at += std::max<std::size_t>(1, Pm4::PacketWords(submission.commands[at]))) {
        const auto header = submission.commands[at];
        const auto opcode = (header >> 8u) & 0xffu;
        if ((header >> 30u) == 3u && (opcode == 0x3c || opcode == 0x93)) return false;
    }
    return true;
}

bool Driver::queue0Before(std::uint64_t received) const {
    if (queue0Executing != 0 && queue0Executing < received) return true;
    const auto worker = workers.find(0);
    if (worker == workers.end()) return false;
    for (const auto& pending : worker->second.pending) {
        if (pending.suspend) continue;
        return pending.received < received;
    }
    return false;
}

bool Driver::orderReleased(std::uint32_t queue, std::uint64_t received) const {
    if (!queue0Before(received)) return true;
    const auto awaited = queue0Awaited.load(std::memory_order_acquire);
    if (awaited == 0) return false;
    if (workers.at(queue).unfinishedWrites.contains(awaited & ~std::uint64_t{3})) return true;
    return runningWorkers.load(std::memory_order_acquire) == 0 && !completionsPending() && !Graphics::Recorder::SnapshotWriteOverlaps(awaited, 4);
}

void Driver::noteWaitBlocked(std::uint32_t queue, std::uint64_t awaited, bool blocked) {
    if (blocked) {
        if (runningWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            idleEpoch.fetch_add(1, std::memory_order_release);
            changed.notify_all();
        }
    } else runningWorkers.fetch_add(1, std::memory_order_acq_rel);
    if (queue == 0) queue0Awaited.store(blocked ? awaited : 0, std::memory_order_release);
    if (queue == 0 && !blocked) queue0AwaitsTitle.store(false, std::memory_order_release);
    if (blocked && orderHolders.load(std::memory_order_acquire) != 0) {
        std::lock_guard lock(mutex);
        changed.notify_all();
    }
}

void Driver::enqueue(Submission submission) {
    const auto queue = submission.queue;
    submission.waitFree = waitFree(submission);
    auto& worker = workers[queue];
    for (const auto dword : submission.labelWrites) ++worker.unfinishedWrites[dword];
    worker.pending.push_back(std::move(submission));
    worker.queued.fetch_add(1, std::memory_order_acq_rel);
    if (const auto bound = batchAgeBound(); bound.count() != 0 && !batchAgeThread.joinable()) batchAgeThread = std::thread([this, bound] { runBatchAge(bound); });
    if (completionWatcher() && !completionThread.joinable()) completionThread = std::thread([this] { runCompletionWatcher(); });
    if (!worker.thread.joinable()) worker.thread = std::thread([this, queue] {
        const Coherence::DriverAccessScope driverAccess;
        char role[32];
        std::snprintf(role, sizeof(role), "queue 0x%x worker", queue);
        try {
            RaiseWorkerThreadPriority(role);
        } catch (...) {
            ReportFailure(std::current_exception());
            return;
        }
        run(queue);
    });
}

void Driver::noteHeldAtSubmit(Submission& submission, std::size_t cursor) {
    const auto packet = std::span<const std::uint32_t>(submission.commands).subspan(cursor, std::min<std::size_t>(Pm4::PacketWords(submission.commands[cursor]), submission.commands.size() - cursor));
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    if ((packet[0] >> 30u) != 3u) return;
    if (opcode == 0x49 || opcode == 0x37) {
        if (const auto label = Pm4::DecodeLabelWrite(packet)) {
            const auto bytes = label->Bytes();
            if (label->address % 4 == 0 && bytes.size() <= 64) {
                for (std::size_t offset = 0; offset < bytes.size(); offset += 4) submission.labelWrites.push_back(label->address + offset);
            }
        }
        return;
    }
    if ((opcode != 0x3c && opcode != 0x93) || packet.size() < 7 || ((packet[1] >> 4u) & 3u) != 1u) return;
    const auto wait = Pm4::DecodeWait(packet);
    const auto address = wait.address;
    const std::size_t bytes = wait.bytes;
    if (address % 4 != 0 || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) return;
    const auto worker = workers.find(submission.queue);
    for (std::size_t offset = 0; offset < bytes; offset += 4) {
        const auto dword = address + offset;
        if (std::find(submission.labelWrites.begin(), submission.labelWrites.end(), dword) != submission.labelWrites.end()) return;
        if (worker != workers.end() && worker->second.unfinishedWrites.contains(dword)) return;
    }
    std::uint64_t value = GuestMemory::LoadDriverAtomic(address, 4);
    if (bytes == 8) value |= GuestMemory::LoadDriverAtomic(address + 4, 4) << 32u;
    if (Pm4::WaitComparesValue(packet, value)) submission.heldAtSubmit.insert(cursor);
}

void Driver::forgetUnfinishedWrites(QueueWorker& worker, const Submission& submission) {
    forgetUnfinishedWrites(worker, submission.labelWrites);
}

void Driver::forgetUnfinishedWrites(QueueWorker& worker, std::span<const std::uint64_t> labelWrites) {
    for (const auto dword : labelWrites) {
        const auto found = worker.unfinishedWrites.find(dword);
        if (found == worker.unfinishedWrites.end()) continue;
        if (--found->second == 0) worker.unfinishedWrites.erase(found);
    }
}

}

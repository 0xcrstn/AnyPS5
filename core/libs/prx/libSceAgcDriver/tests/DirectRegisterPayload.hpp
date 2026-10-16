#pragma once

#include "ProducerMappingIncarnation.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>
#include <cstring>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace DirectRegisterPayloadTests {

using namespace AgcDriver::Graphics;
namespace Memory = AgcDriver::GuestMemory;
namespace Coherence = AgcDriver::Coherence;

#ifdef _WIN32
constexpr std::size_t DescriptorBytes = 1024;
constexpr std::size_t SnapshotOffset = 32;
constexpr std::array<std::uint32_t, 4> Original{0x00cu, 11u, 0x00du, 22u};

void Progress(const char* phase) {
    std::fprintf(stderr, "[direct-register-payload] %s\n", phase);
    std::fflush(stderr);
}


struct Modes {
    ProducerMappingIncarnationTests::NativeMode native;
    Coherence::Mode leases = Coherence::InputLeasesMode();
    Modes() { Coherence::SetInputLeasesModeForTests(Coherence::Mode::New); }
    ~Modes() { Coherence::SetInputLeasesModeForTests(leases); }
};

struct Drain {
    Recorder& recorder;
    ~Drain() { recorder.Sync(); }
};

std::uint64_t DescriptorAddress(const ProducerMappingIncarnationTests::Mapping& mapping) {
    return mapping.Address() + 256;
}

void Initialize(const ProducerMappingIncarnationTests::Mapping& mapping) {
    const auto address = DescriptorAddress(mapping);
    Memory::StoreOwnBytes(address + SnapshotOffset, std::as_bytes(std::span(Original)));
    Require(Memory::CollectWritesUncached(mapping.Address(), ProducerMappingIncarnationTests::Bytes) != 0, "direct register payload: shared write tracking unavailable");
}

std::shared_ptr<GuestBufferMemory> Upload(const Context& context, Recorder& recorder, std::uint64_t address, bool arm = true) {
    auto memory = std::make_shared<GuestBufferMemory>(context);
    memory->AddWritable(address, DescriptorBytes, false, false);
    memory->Upload(false);
    const auto imported = ImportedHostRange(context, address, DescriptorBytes);
    Require(imported.has_value() && !memory->HasCopiedWrites(), "direct register payload: the writable descriptor was not imported directly");
    std::uint32_t adjustment = 0;
    const auto descriptor = memory->Descriptor(address, DescriptorBytes, adjustment);
    Require(descriptor.buffer == imported->buffer, "direct register payload: the descriptor used a copy instead of its direct import");
    if (arm) memory->PrepareDirectWriteSnapshots();
    recorder.Keep(memory);
    return memory;
}

void RecordFill(const Context& context, Recorder& recorder, const GuestBufferMemory& memory, std::uint64_t address, std::uint32_t value) {
    std::uint32_t adjustment = 0;
    const auto view = memory.Descriptor(address, DescriptorBytes, adjustment);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, view.buffer, view.offset + adjustment + SnapshotOffset + sizeof(std::uint32_t), sizeof(std::uint32_t), value);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
}

std::array<std::uint32_t, 4> Resolve(const Memory::DriverStoreWatch& watch, std::array<std::uint32_t, 4> snapshot, std::uint64_t address) {
    Memory::FlushGpuWrites(address + SnapshotOffset, sizeof(snapshot));
    Memory::ApplyDriverSnapshot(watch, std::as_writable_bytes(std::span(snapshot)));
    return snapshot;
}

template<typename TLock>
void PartialAndSameValue(const Context& context, Recorder& recorder, TLock& gpu, bool sameValue, bool aliasWriter, bool pendingRead, bool faultingCpu = false, bool captureAfterUpload = false) {
    std::fprintf(stderr, "[direct-register-payload] partial begin same=%u alias=%u pending=%u fault=%u late=%u\n", unsigned(sameValue), unsigned(aliasWriter), unsigned(pendingRead), unsigned(faultingCpu), unsigned(captureAfterUpload));
    std::fflush(stderr);
    Progress("partial mapping before");
    ProducerMappingIncarnationTests::Mapping mapping;
    Progress("partial mapping after / initialize before");
    Initialize(mapping);
    Progress("partial initialize after");
    const auto address = DescriptorAddress(mapping);
    const auto output = aliasWriter ? mapping.Alias() + 256 : address;
    auto memory = captureAfterUpload ? Upload(context, recorder, output, false) : std::shared_ptr<GuestBufferMemory>{};
    std::array<std::uint32_t, 4> frozen{};
    Progress("partial capture before");
    auto watch = Memory::ReadDriverSnapshot(address + SnapshotOffset, std::as_writable_bytes(std::span(frozen)), 4);
    Progress("partial capture after");
    Require(frozen == Original, "direct register payload: unexpected captured register words");
    std::array<std::byte, 8> unrelated{};
    auto unrelatedWatch = Memory::ReadDriverSnapshot(mapping.Other() + 256 + SnapshotOffset, unrelated, 4);
    Progress("partial upload/prepare before");
    if (captureAfterUpload) memory->PrepareDirectWriteSnapshots();
    else memory = Upload(context, recorder, output);
    Progress("partial upload/prepare after");
    Drain drain{recorder};
    const auto since = Memory::CollectWritesUncached(output, DescriptorBytes);
    const auto expected = sameValue ? Original[1] : 44u;
    Progress("partial record before");
    RecordFill(context, recorder, *memory, output, expected);
    memory->RecordCopyBacks(recorder);
    recorder.NotePendingWrites(memory->Writes());
    memory->MarkDirectWrites(recorder);
    Progress("partial mark/readback after");
    auto completed = std::make_shared<std::atomic<unsigned>>(0);
    recorder.OnCompleteWithoutGuestAccess([completed] { completed->fetch_add(1, std::memory_order_release); });
    if (faultingCpu) {
        struct DisplacedOwner {
            Recorder& owner;
            std::unique_ptr<Recorder> replacement;
            ~DisplacedOwner() {
                replacement.reset();
                owner.Activate();
            }
        } displaced{recorder, std::make_unique<Recorder>(context)};
        displaced.replacement->Activate();
        Require(Recorder::Current() == displaced.replacement.get(), "direct register payload: the controlled recorder did not displace its pending predecessor");
        Progress("partial displaced owner ready / fault before");
        const auto waits = Coherence::CurrentCounters().leaseWaits;
        std::atomic<bool> entered{false}, finished{false};
        std::exception_ptr failure;
        std::thread cpu([&] {
            try {
                entered.store(true, std::memory_order_release);
                const std::uint32_t reused = 888u;
                std::memcpy(reinterpret_cast<void*>(mapping.Alias() + 256 + SnapshotOffset + 3 * sizeof(std::uint32_t)), &reused, sizeof(reused));
                finished.store(true, std::memory_order_release);
            } catch (...) { failure = std::current_exception(); }
        });
        bool locked = true;
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (Coherence::CurrentCounters().leaseWaits == waits && !finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            Require(entered.load(std::memory_order_acquire) && !finished.load(std::memory_order_acquire) && Coherence::CurrentCounters().leaseWaits > waits, "direct register payload: a native alias write did not wait on the pending producer lease");
            Require(completed->load(std::memory_order_acquire) == 0, "direct register payload: producer completed before the controlled native fault");
            Progress("partial fault blocked / unlock before");
            gpu.unlock();
            locked = false;
            Progress("partial unlock after / join before");
            cpu.join();
            Progress("partial join after / relock before");
            gpu.lock();
            Progress("partial relock after");
            locked = true;
            if (failure != nullptr) std::rethrow_exception(failure);
            Require(finished.load(std::memory_order_acquire) && completed->load(std::memory_order_acquire) == 1, "direct register payload: native fault did not drain the producer before releasing its CPU store");
            auto resolved = frozen;
            Memory::ApplyDriverSnapshot(*watch, std::as_writable_bytes(std::span(resolved)));
            Require(resolved == std::array<std::uint32_t, 4>{Original[0], expected, Original[2], Original[3]}, "direct register payload: resolving native lease lost completed GPU payload or accepted the later CPU word");
        } catch (...) {
            if (locked) gpu.unlock();
            if (cpu.joinable()) cpu.join();
            gpu.lock();
            throw;
        }
    } else if (pendingRead) {
        bool refused = false;
        auto premature = frozen;
        try { Memory::ApplyDriverSnapshot(*watch, std::as_writable_bytes(std::span(premature))); }
        catch (const std::runtime_error& error) {
            if (std::string_view(error.what()).find("still pending") == std::string_view::npos) throw;
            refused = true;
        }
        Require(refused && completed->load(std::memory_order_acquire) == 0, "direct register payload: incomplete GPU output was accepted before its producer completed");
        Progress("partial flush before");
        const auto resolved = Resolve(*watch, frozen, address);
        Progress("partial flush after");
        Require(completed->load(std::memory_order_acquire) == 1 && resolved == std::array<std::uint32_t, 4>{Original[0], expected, Original[2], Original[3]}, "direct register payload: consumption did not finish and apply the actual pending producer");
    } else {
        Progress("partial submit before");
        recorder.Submit();
        Progress("partial submit after / sync before");
        recorder.Sync();
        Progress("partial sync after");
        Require(completed->load(std::memory_order_acquire) == 1, "direct register payload: the actual GPU producer did not finish");
    }
    Progress("partial WriteBack before");
    memory->WriteBack();
    Progress("partial WriteBack after / checks before");
    Require(Memory::StoresSince(output, DescriptorBytes, since) == Memory::StoreProvenance::Unknown, "direct register payload: a partial direct descriptor became a full-byte overwrite proof");
    std::vector<std::byte> bytes(ProducerMappingIncarnationTests::Bytes);
    Memory::Read(mapping.Address(), bytes);
    for (std::size_t i = 0; i != bytes.size(); ++i) {
        if (i >= 256 + SnapshotOffset && i < 256 + SnapshotOffset + sizeof(Original)) {
            const auto at = i - 256 - SnapshotOffset;
            const auto words = std::array<std::uint32_t, 4>{Original[0], expected, Original[2], faultingCpu ? 888u : Original[3]};
            Require(bytes[i] == std::as_bytes(std::span(words))[at], "direct register payload: GPU changed an untouched register byte");
        } else Require(bytes[i] == std::byte{0x31}, "direct register payload: GPU changed descriptor padding");
    }
    Require(ProducerMappingIncarnationTests::Read(mapping.Alias()) == bytes, "direct register payload: shared aliases disagree after the partial GPU write");
    Require(ProducerMappingIncarnationTests::Read(mapping.Other()) == std::vector<std::byte>(ProducerMappingIncarnationTests::Bytes, std::byte{0x59}), "direct register payload: GPU changed an unrelated allocation byte");
    const std::array<std::uint32_t, 4> reused{0x99u, 777u, 0x88u, 888u};
    std::memcpy(reinterpret_cast<void*>(address + SnapshotOffset), reused.data(), sizeof(reused));
    Memory::ApplyDriverSnapshot(*watch, std::as_writable_bytes(std::span(frozen)));
    Require(frozen == std::array<std::uint32_t, 4>{Original[0], expected, Original[2], Original[3]}, "direct register payload: completed payload reread CPU-reused written or untouched register words");
    Require(unrelatedWatch->State() == Memory::DriverStoredSince::No, "direct register payload: unrelated backing received a GPU snapshot update");
    std::array<std::byte, 8> unrelatedAfter{};
    Memory::Read(mapping.Other() + 256 + SnapshotOffset, unrelatedAfter);
    Require(unrelatedAfter == unrelated, "direct register payload: GPU changed unrelated backing");
    Progress("partial checks after / teardown before");
}

void PreArmRefusal(const Context& context, Recorder& recorder, bool unavailableGuard) {
    ProducerMappingIncarnationTests::Mapping mapping;
    Initialize(mapping);
    const auto address = DescriptorAddress(mapping);
    std::array<std::uint32_t, 4> frozen{};
    auto watch = Memory::ReadDriverSnapshot(address + SnapshotOffset, std::as_writable_bytes(std::span(frozen)), 4);
    if (!unavailableGuard) {
        const std::uint32_t reused = 777u;
        std::memcpy(reinterpret_cast<void*>(address + SnapshotOffset + sizeof(std::uint32_t)), &reused, sizeof(reused));
    }
    const auto backend = Coherence::CurrentProtectionBackend();
    struct Restore {
        Coherence::ProtectionBackend backend;
        ~Restore() { Coherence::SetProtectionBackend(backend); }
    } restore{backend};
    if (unavailableGuard) Coherence::SetProtectionBackend({});
    auto memory = std::make_shared<GuestBufferMemory>(context);
    memory->AddWritable(address, DescriptorBytes, false, false);
    bool refused = false;
    try {
        memory->Upload(false);
        memory->PrepareDirectWriteSnapshots();
    }
    catch (const std::runtime_error& error) {
        const auto reason = unavailableGuard ? "requires complete native CPU exclusion" : "baseline was reused by the CPU";
        if (std::string_view(error.what()).find(reason) == std::string_view::npos) throw;
        refused = true;
    }
    Require(refused, unavailableGuard ? "direct register payload: missing native protection silently accepted a watched direct writer" : "direct register payload: CPU reuse before arming was accepted as the GPU baseline");
    Require(frozen == Original && !recorder.PendingWriteOverlaps(address, DescriptorBytes), "direct register payload: refused preparation changed the frozen snapshot or published a pending writer");
    Require(!GuestArena::GuestArenaMappingProtected_nid_postfix(address, DescriptorBytes), "direct register payload: failed preparation leaked a native output/read lease");
    static_cast<void>(watch);
}

void SequentialReuse(const Context& context, Recorder& recorder) {
    ProducerMappingIncarnationTests::Mapping mapping;
    Initialize(mapping);
    const auto address = DescriptorAddress(mapping);
    std::array<std::uint32_t, 4> first{};
    auto firstWatch = Memory::ReadDriverSnapshot(address + SnapshotOffset, std::as_writable_bytes(std::span(first)), 4);
    std::array<std::byte, 8> unrelated{};
    const auto other = mapping.Other() + 256 + SnapshotOffset;
    auto otherWatch = Memory::ReadDriverSnapshot(other, unrelated, 4);
    auto memory = Upload(context, recorder, address);
    Drain drain{recorder};
    RecordFill(context, recorder, *memory, address, 44u);
    recorder.NotePendingWrites(memory->Writes());
    memory->MarkDirectWrites(recorder);
    const std::uint64_t unrelatedValue = 0x123456789abcdef0ull;
    Memory::StoreOwnBytes(other, std::as_bytes(std::span(&unrelatedValue, 1)));
    Memory::ApplyDriverSnapshot(*otherWatch, unrelated);
    Require(std::memcmp(unrelated.data(), &unrelatedValue, sizeof(unrelatedValue)) == 0, "direct register payload: a pending GPU ticket suppressed an unrelated actual driver payload");
    recorder.Submit();
    recorder.Sync();
    Memory::ApplyDriverSnapshot(*firstWatch, std::as_writable_bytes(std::span(first)));
    Require(first == std::array<std::uint32_t, 4>{Original[0], 44u, Original[2], Original[3]}, "direct register payload: first use lost its actual GPU payload");
    std::array<std::uint32_t, 4> second{};
    auto secondWatch = Memory::ReadDriverSnapshot(address + SnapshotOffset, std::as_writable_bytes(std::span(second)), 4);
    Require(second == first, "direct register payload: second snapshot missed the first producer");
    memory->PrepareDirectWriteSnapshots();
    recorder.Keep(memory);
    RecordFill(context, recorder, *memory, address, 55u);
    recorder.NotePendingWrites(memory->Writes());
    memory->MarkDirectWrites(recorder);
    recorder.Submit();
    recorder.Sync();
    memory->WriteBack();
    const std::array<std::uint32_t, 4> reused{99u, 777u, 88u, 888u};
    std::memcpy(reinterpret_cast<void*>(address + SnapshotOffset), reused.data(), sizeof(reused));
    Memory::ApplyDriverSnapshot(*firstWatch, std::as_writable_bytes(std::span(first)));
    Memory::ApplyDriverSnapshot(*secondWatch, std::as_writable_bytes(std::span(second)));
    Require(first == std::array<std::uint32_t, 4>{Original[0], 55u, Original[2], Original[3]} && second == first, "direct register payload: reused memory accepted a finished ticket or lost the new watch and second GPU output");
}

void UnrecordedAbandonment(const Context& context) {
    ProducerMappingIncarnationTests::Mapping mapping;
    Initialize(mapping);
    const auto address = DescriptorAddress(mapping);
    std::array<std::uint32_t, 4> frozen{};
    auto watch = Memory::ReadDriverSnapshot(address + SnapshotOffset, std::as_writable_bytes(std::span(frozen)), 4);
    {
        GuestBufferMemory memory(context);
        memory.AddWritable(address, DescriptorBytes, false, false);
        memory.Upload(false);
        memory.PrepareDirectWriteSnapshots();
        Require(GuestArena::GuestArenaMappingProtected_nid_postfix(address, DescriptorBytes), "direct register payload: unrecorded preparation did not acquire its native lease");
    }
    Require(!GuestArena::GuestArenaMappingProtected_nid_postfix(address, DescriptorBytes), "direct register payload: an abandoned unrecorded writer leaked its native lease");
    bool refused = false;
    try { Memory::ApplyDriverSnapshot(*watch, std::as_writable_bytes(std::span(frozen))); }
    catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("bounded payload") == std::string_view::npos) throw;
        refused = true;
    }
    Require(refused && frozen == Original, "direct register payload: an abandoned ticket fabricated an actual GPU payload");
}

void UnrecordedCompletion() {
    ProducerMappingIncarnationTests::Mapping mapping;
    Initialize(mapping);
    const auto address = DescriptorAddress(mapping);
    std::array<std::uint32_t, 4> frozen{};
    auto watch = Memory::ReadDriverSnapshot(address + SnapshotOffset, std::as_writable_bytes(std::span(frozen)), 4);
    auto ticket = Memory::PrepareDriverGpuWrite(address, DescriptorBytes);
    Require(ticket != nullptr && ticket->Prepared() && !ticket->Finished(), "direct register payload: direct ticket was not prepared");
    std::size_t bytes = 0;
    for (const auto& [first, last] : ticket->Ranges()) bytes += static_cast<std::size_t>(last - first);
    std::vector<std::byte> fabricated(bytes, std::byte{0xa5});
    bool refused = false;
    try { ticket->Complete(fabricated); }
    catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("was not recorded") == std::string_view::npos) throw;
        refused = true;
    }
    Require(refused && !ticket->Finished(), "direct register payload: an unrecorded ticket accepted fabricated completion bytes");
    ticket->Cancel();
    Require(!GuestArena::GuestArenaMappingProtected_nid_postfix(address, DescriptorBytes), "direct register payload: cancelled unrecorded ticket retained native protection");
    refused = false;
    try { Memory::ApplyDriverSnapshot(*watch, std::as_writable_bytes(std::span(frozen))); }
    catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("bounded payload") == std::string_view::npos) throw;
        refused = true;
    }
    Require(refused && frozen == Original, "direct register payload: cancelled unrecorded completion published fabricated bytes");
}

void PreArmMappingRefusal(const Context& context, Recorder& recorder) {
    for (const bool restore : {false, true}) {
        ProducerMappingIncarnationTests::Mapping mapping;
        Initialize(mapping);
        const auto address = DescriptorAddress(mapping);
        std::array<std::uint32_t, 4> frozen{};
        auto watch = Memory::ReadDriverSnapshot(address + SnapshotOffset, std::as_writable_bytes(std::span(frozen)), 4);
        mapping.Replace(restore);
        const auto before = ProducerMappingIncarnationTests::Read(mapping.Address());
        const auto alias = ProducerMappingIncarnationTests::Read(mapping.Alias());
        GuestBufferMemory memory(context);
        memory.AddWritable(address, DescriptorBytes, false, false);
        bool refused = false;
        try {
            memory.Upload(false);
            memory.PrepareDirectWriteSnapshots();
        }
        catch (const std::runtime_error& error) {
            if (std::string_view(error.what()).find("mapping incarnation") == std::string_view::npos) throw;
            refused = true;
        }
        Require(refused && frozen == Original && !recorder.PendingWriteOverlaps(address, DescriptorBytes), "direct register payload: changed submitted mapping was accepted before GPU preparation");
        Require(ProducerMappingIncarnationTests::Read(mapping.Address()) == before && ProducerMappingIncarnationTests::Read(mapping.Alias()) == alias, "direct register payload: mapping refusal changed replacement or original backing");
        Require(!GuestArena::GuestArenaMappingProtected_nid_postfix(address, DescriptorBytes), "direct register payload: mapping refusal leaked its native guard");
        static_cast<void>(watch);
    }
}

std::vector<GuestArena::MappingSpan> CapturePage(std::uint64_t address) {
    const auto page = address & ~std::uint64_t{4095};
    std::size_t count = 0;
    Require(GuestArena::GuestArenaCaptureMapping_nid_postfix(page, 4096, nullptr, 0, &count) && count != 0, "direct register payload: cannot size the expected native mapping witness");
    std::vector<GuestArena::MappingSpan> expected(count);
    Require(GuestArena::GuestArenaCaptureMapping_nid_postfix(page, 4096, expected.data(), expected.size(), &count) && count == expected.size(), "direct register payload: cannot capture the complete native mapping witness");
    return expected;
}

bool LeaseRefused(std::uint64_t address, std::span<const GuestArena::MappingSpan> expected) {
    try {
        const auto lease = Coherence::AcquireReadLease({address, address + 8}, 1, expected);
        if (lease == 0) return true;
        Coherence::RetireReadLease(lease);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("mapping incarnation") == std::string_view::npos) throw;
        return true;
    }
    return false;
}

void MappingWitnessRefusals() {
    {
        ProducerMappingIncarnationTests::Mapping mapping;
        const auto address = DescriptorAddress(mapping) + SnapshotOffset;
        const auto expected = CapturePage(address);
        ProducerMappingIncarnationTests::Mapping unrelated;
        const auto lease = Coherence::AcquireReadLease({address, address + 8}, 1, expected);
        Require(lease != 0, "direct register payload: an unrelated mapping invalidated a current read-lease witness");
        Coherence::RetireReadLease(lease);
        auto partial = expected;
        ++partial.front().begin;
        Require(LeaseRefused(address, partial), "direct register payload: a partial page witness installed a native read lease");
        static_cast<void>(unrelated);
    }
    for (const bool restore : {false, true}) {
        ProducerMappingIncarnationTests::Mapping mapping;
        const auto address = DescriptorAddress(mapping) + SnapshotOffset;
        const auto expected = CapturePage(address);
        mapping.Replace(restore);
        const auto before = ProducerMappingIncarnationTests::Read(mapping.Address());
        const auto aliasBefore = ProducerMappingIncarnationTests::Read(mapping.Alias());
        Require(LeaseRefused(address, expected), restore ? "direct register payload: X-to-Y-to-X mapping ABA accepted an old read-lease witness" : "direct register payload: replacement mapping accepted an old read-lease witness");
        Require(ProducerMappingIncarnationTests::Read(mapping.Address()) == before && ProducerMappingIncarnationTests::Read(mapping.Alias()) == aliasBefore, "direct register payload: failed witness validation changed either backing");
        Require(!GuestArena::GuestArenaMappingProtected_nid_postfix(address, 8), "direct register payload: a refused old witness retained a native guard");
    }
}

template<typename TLock>
bool Run(const Context& context, Recorder& recorder, TLock& gpu) {
    Require(context.hostImportAlignment != 0, "direct register payload: host imports required");
    Progress("Run modes before");
    Modes modes;
    Progress("CompileOnly constructor before");
    AgcDriver::VulkanDevice productionCoherence(AgcDriver::VulkanDevice::CompileOnly{});
    Progress("CompileOnly constructor after");
    recorder.Activate();
    Progress("import mode before");
    const auto importMode = PrepareImportWatch(context);
    SetImportWatch(context, ImportWatch::Watch);
    struct RestoreImport {
        const Context& context;
        ImportWatch mode;
        ~RestoreImport() { SetImportWatch(context, mode); }
    } restore{context, importMode};
    Progress("import mode after / cases before");
    PartialAndSameValue(context, recorder, gpu, false, false, true);
    PartialAndSameValue(context, recorder, gpu, true, false, false);
    PartialAndSameValue(context, recorder, gpu, false, true, true);
    PartialAndSameValue(context, recorder, gpu, false, false, false, true);
    PartialAndSameValue(context, recorder, gpu, false, false, false, false, true);
    Progress("sequential reuse before");
    SequentialReuse(context, recorder);
    Progress("sequential reuse after");
    Progress("unrecorded abandonment before");
    UnrecordedAbandonment(context);
    Progress("unrecorded abandonment after");
    Progress("unrecorded completion before");
    UnrecordedCompletion();
    Progress("unrecorded completion after");
    Progress("pre-arm mapping before");
    PreArmMappingRefusal(context, recorder);
    Progress("pre-arm mapping after");
    Progress("pre-arm CPU reuse before");
    PreArmRefusal(context, recorder, false);
    Progress("pre-arm CPU reuse after");
    Progress("pre-arm unavailable guard before");
    PreArmRefusal(context, recorder, true);
    Progress("pre-arm unavailable guard after");
    Progress("mapping witness before");
    MappingWitnessRefusals();
    Progress("mapping witness after");
    Progress("Run teardown before");
    return true;
}
#else
template<typename TLock>
inline bool Run(const Context&, Recorder&, TLock&) { return false; }
#endif

}

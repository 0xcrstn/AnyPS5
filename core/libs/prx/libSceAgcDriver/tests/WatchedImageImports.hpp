#pragma once

#include "ProducerMappingIncarnation.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include <cstring>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

namespace WatchedImageImportTests {

using namespace AgcDriver::Graphics;
namespace Memory = AgcDriver::GuestMemory;

#ifdef _WIN32
inline PFN_vkGetDeviceProcAddr resolver = nullptr;
inline PFN_vkDestroyBuffer destroyBuffer = nullptr;
inline PFN_vkFreeMemory freeMemory = nullptr;
inline std::size_t destroyedBuffers = 0;
inline std::size_t freedMemory = 0;
inline std::atomic<bool> unwatchReady{false};
inline std::atomic<bool> resumeUnwatch{false};
inline std::atomic<unsigned> materialized{0};

void UnwatchOwnership(const Context&) {
    namespace Coherence = AgcDriver::Coherence;
    ProducerMappingIncarnationTests::NativeMode native;
    ProducerMappingIncarnationTests::Mapping mapping;
    constexpr auto bytes = ProducerMappingIncarnationTests::Bytes;
    Memory::SetUnwatchHook([](std::uint64_t, std::size_t) {
        unwatchReady.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!resumeUnwatch.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        Require(resumeUnwatch.load(std::memory_order_acquire), "unwatch ownership: the controlled preparation was not released");
    });
    struct Restore {
        ~Restore() { Memory::SetUnwatchHook(&StorageTexture::SaveBeforeUnwatch); }
    } restore;
    Coherence::SetMaterializer([](Coherence::Interval) {
        if (Coherence::ClaimFaultService()) materialized.fetch_add(1, std::memory_order_relaxed);
    });
    unwatchReady.store(false, std::memory_order_relaxed);
    resumeUnwatch.store(false, std::memory_order_relaxed);
    materialized.store(0, std::memory_order_relaxed);
    std::exception_ptr failure;
    std::thread worker([&] {
        try { Memory::Unwatch(mapping.Address(), bytes); }
        catch (...) { failure = std::current_exception(); }
    });
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!unwatchReady.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        Require(unwatchReady.load(std::memory_order_acquire), "unwatch ownership: the exclusion race did not reach its preparation boundary");
        const auto guard = Coherence::ProtectDeviceOwned({mapping.Address(), mapping.Address() + bytes}, Coherence::Source::PendingImage, 1);
        Require(guard != 0, "unwatch ownership: a watched native owner could not be installed");
        resumeUnwatch.store(true, std::memory_order_release);
        worker.join();
        if (failure != nullptr) std::rethrow_exception(failure);
        Require(materialized.load(std::memory_order_relaxed) == 1 && !Coherence::ProtectionLive(guard), "unwatch ownership: a guard installed after preparation was not resolved exactly once");
        Require(!Memory::Watched(mapping.Address(), bytes), "unwatch ownership: write tracking was not removed");
        Require(Coherence::ProtectDeviceOwned({mapping.Address(), mapping.Address() + bytes}, Coherence::Source::ShadowUnit, 2) == 0, "unwatch ownership: an unwatched native output acquired a guard");
        Require(!GuestArena::GuestArenaMappingProtected_nid_postfix(mapping.Address(), bytes), "unwatch ownership: refused output retained native protection");
    } catch (...) {
        resumeUnwatch.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
        throw;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocation) {
    if (buffer != VK_NULL_HANDLE) ++destroyedBuffers;
    destroyBuffer(device, buffer, allocation);
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocation) {
    if (memory != VK_NULL_HANDLE) ++freedMemory;
    freeMemory(device, memory, allocation);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL MissingAddress(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkGetBufferDeviceAddressKHR") == 0) return nullptr;
    if (std::strcmp(name, "vkDestroyBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyBuffer);
    if (std::strcmp(name, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FreeMemory);
    return resolver(device, name);
}

void NativeModeRestoration() {
    namespace Coherence = AgcDriver::Coherence;
    ProducerMappingIncarnationTests::NativeMode outer;
    ProducerMappingIncarnationTests::Mapping mapping;
    constexpr auto bytes = ProducerMappingIncarnationTests::Bytes;
    const Coherence::Materializer prior = [](Coherence::Interval) { static_cast<void>(Coherence::ClaimFaultService()); };
    Coherence::SetMaterializer(prior);
    GuestArena::GuestArenaSetResolveHostWrites_nid_postfix(false);
    const auto backend = Coherence::CurrentProtectionBackend();
    const auto faults = Coherence::FaultsMode();
    const auto guard = Coherence::ProtectDeviceOwned({mapping.Address(), mapping.Address() + bytes}, Coherence::Source::PendingImage, 1);
    Require(guard != 0, "native mode restoration: the live outer guard was not installed");
    {
        ProducerMappingIncarnationTests::NativeMode inner;
        Require(Coherence::ProtectionLive(guard), "native mode restoration: entering a nested scope erased a live guard");
    }
    const auto restored = Coherence::CurrentProtectionBackend();
    Require(Coherence::ProtectionLive(guard), "native mode restoration: leaving a nested scope erased a live guard");
    Require(restored.protect == backend.protect && restored.release == backend.release && restored.open == backend.open && restored.protectMapping == backend.protectMapping && Coherence::CurrentMaterializer() == prior && Coherence::FaultsMode() == faults, "native mode restoration: a nested scope did not restore its callbacks and fault mode");
    Require(!GuestArena::GuestArenaResolveHostWritesEnabled_nid_postfix(), "native mode restoration: a nested scope changed host-write resolution");
    Coherence::ReleaseDeviceOwned(guard);
    Require(!GuestArena::GuestArenaMappingProtected_nid_postfix(mapping.Address(), bytes), "native mode restoration: the preserved guard could not be retired");
}

bool Run(const Context& context, Recorder& recorder) {
    constexpr auto bytes = ProducerMappingIncarnationTests::Bytes;
    if (context.hostImportAlignment == 0 || context.hostImportAlignment > bytes || !Memory::WriteWatched()) return false;
    static_cast<void>(PrepareImportWatch(context));
    NativeModeRestoration();
    UnwatchOwnership(context);
    {
        void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, bytes);
        struct Release {
            const Context& context;
            void* block;
            bool registered = false;
            ~Release() {
                if (registered) GuestAllocations::Mutation().Remove(block);
                HostImportFor(context, reinterpret_cast<std::uint64_t>(block), bytes);
                GuestArena::GuestArenaReset_nid_postfix(block, bytes);
                GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
            }
        } release{context, block};
        GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
        release.registered = true;
        const auto address = reinterpret_cast<std::uint64_t>(block);
        Memory::CollectWritesUncached(address, bytes);
        Require(Memory::Watched(address, bytes), "watched image imports: private mapping baseline is unwatched");
        const auto live = HostImportLiveBytes();
        Require(HostImportFor(context, address, bytes, true) == nullptr, "watched image imports: private import was accepted");
        Require(Memory::Watched(address, bytes) && HostImportLiveBytes() == live, "watched image imports: refusal removed tracking or created an import");
        const auto* ordinary = HostImportFor(context, address, bytes);
        Require(ordinary != nullptr && ordinary->unwatched, "watched image imports: default private import control changed");
        Require(HostImportFor(context, address, bytes, true) == nullptr, "watched image imports: cached unwatched import was accepted");
    }
    {
        ProducerMappingIncarnationTests::NativeMode native;
        ProducerMappingIncarnationTests::Mapping mapping;
        const auto* ordinary = HostImportFor(context, mapping.Address(), bytes);
        Require(ordinary != nullptr && !ordinary->unwatched, "watched image imports: default shared import control failed");
        Require(ordinary->mappingProof && ordinary->alias != nullptr, "watched image imports: default shared import did not capture native identity");
        const auto serial = HostImportSerial(context, mapping.Address(), bytes, false);
        const auto* required = HostImportFor(context, mapping.Address(), bytes, true);
        Require(required == ordinary && HostImportSerial(context, mapping.Address(), bytes, false) == serial, "watched image imports: a proved default import was not reused with its original buffer identity");
        Require(Memory::Watched(mapping.Address(), bytes), "watched image imports: required reuse removed shared tracking");
        TextureDetiler detiler(context);
        auto imageContext = context;
        imageContext.detiler = &detiler;
        auto image = StorageTexture::Create(imageContext, detiler, ProducerMappingIncarnationTests::Resource(mapping.Address()), 0);
        image->PrepareWrite();
        ProducerMappingIncarnationTests::PaintAndRead(imageContext, recorder, image);
        image->MarkDirty(&recorder);
        Require(GuestArena::GuestArenaMappingProtected_nid_postfix(mapping.Address(), bytes), "watched image imports: default import control did not retain a real native GPU owner");
        image->WriteBack();
        recorder.Sync();
        recorder.WaitDeferredReleases();
        ProducerMappingIncarnationTests::RequireRed(ProducerMappingIncarnationTests::Read(mapping.Address()));
        ProducerMappingIncarnationTests::RequireRed(ProducerMappingIncarnationTests::Read(mapping.Alias()));
        Require(StorageTexture::FindPending(mapping.Address(), bytes) == nullptr, "watched image imports: default-to-required reuse left actual GPU output pending");
    }
    for (const bool initialRequired : {false, true}) for (const bool restore : {false, true}) {
        ProducerMappingIncarnationTests::Mapping mapping;
        const auto* imported = HostImportFor(context, mapping.Address(), bytes, initialRequired);
        Require(imported != nullptr && imported->mappingProof && !imported->unwatched && imported->alias != nullptr, "watched image imports: shared mapping did not retain a watched native import");
        Require(HostImportFor(context, mapping.Address(), bytes, true) == imported, "watched image imports: unchanged cached native import was refused");
        const auto* retained = static_cast<const std::byte*>(imported->alias);
        Require(retained[0] == std::byte{0x31}, "watched image imports: shared import reads unexpected original backing");
        mapping.Replace(restore);
        Require(HostImportFor(context, mapping.Address(), bytes, true) == nullptr, "watched image imports: replacement or ABA reused old native import");
        Require(retained[0] == std::byte{0x31} && *reinterpret_cast<const std::byte*>(mapping.Other()) == std::byte{0x59}, "watched image imports: refused cached import changed either backing");
    }
    {
        ProducerMappingIncarnationTests::Mapping mapping;
        auto failing = context;
        resolver = context.deviceProc;
        destroyBuffer = context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer");
        freeMemory = context.Function<PFN_vkFreeMemory>("vkFreeMemory");
        destroyedBuffers = 0;
        freedMemory = 0;
        failing.deviceProc = &MissingAddress;
        const auto live = HostImportLiveBytes();
        bool refused = false;
        try { static_cast<void>(HostImportFor(failing, mapping.Address(), bytes, true)); }
        catch (const std::runtime_error& error) { refused = std::strstr(error.what(), "vkGetBufferDeviceAddressKHR") != nullptr; }
        Require(refused && destroyedBuffers == 1 && freedMemory == 1, "watched image imports: failed import leaked its Vulkan buffer or memory");
        Require(Memory::Watched(mapping.Address(), bytes) && HostImportLiveBytes() == live, "watched image imports: failed import changed watch coverage or live-byte accounting");
        Require(HostImportFor(context, mapping.Address(), bytes, true) != nullptr, "watched image imports: failed import prevented a clean retry");
    }
    {
        ProducerMappingIncarnationTests::Mapping mapping;
        Memory::Unwatch(mapping.Other(), bytes);
        bool called = false;
        Require(!Memory::ImportWatched(mapping.Other(), bytes, [&] { called = true; return true; }, true, true) && !called, "watched image imports: strict import invoked its callback over unwatched memory");
    }
    return true;
}
#else
bool Run(const Context&, Recorder&) { return false; }
#endif

}

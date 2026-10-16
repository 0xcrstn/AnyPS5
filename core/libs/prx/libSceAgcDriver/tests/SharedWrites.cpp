#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <exception>
#include <string>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

extern "C" {
int APS5_VABI sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int APS5_VABI sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
int APS5_VABI sceKernelMunmap(void*, std::size_t);
int APS5_VABI sceKernelMprotect(const void*, std::size_t, int);
}

namespace {
using namespace AgcDriver::GuestMemory;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
struct Mapping {
    std::size_t size;
    std::int64_t physical = -1;
    void* data = nullptr;
    explicit Mapping(std::size_t size) : size(size) {
        require(sceKernelAllocateDirectMemory(0, 0x10000000000ll, size, 65536, 3, &physical) == 0, "allocate direct memory failed");
        if (sceKernelMapDirectMemory(&data, size, 3, 0, physical, 65536) != 0) {
            sceKernelReleaseDirectMemory(physical, size);
            throw std::runtime_error("map direct memory failed");
        }
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() {
        if (data != nullptr) sceKernelMunmap(data, size);
        if (physical >= 0) sceKernelReleaseDirectMemory(physical, size);
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(data); }
};

struct Alias {
    void* data = nullptr;
    std::size_t bytes;
    explicit Alias(const Mapping& mapping) : bytes(mapping.size) {
        require(sceKernelMapDirectMemory(&data, bytes, 3, 0, mapping.physical, 65536) == 0, "alias mapping failed");
    }
    Alias(const Alias&) = delete;
    Alias& operator=(const Alias&) = delete;
    ~Alias() { if (data != nullptr) sceKernelMunmap(data, bytes); }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(data); }
};

#ifdef _WIN32
void directMappingOwnership() {
    constexpr std::size_t bytes = 65536;
    Mapping mapping(bytes);
    std::uintptr_t residentBegin = 0, residentEnd = 0;
    require(Watched(mapping.Address(), bytes), "direct ownership: a fresh kernel mapping is not watched");
    std::array<std::byte, bytes> initial;
    initial.fill(std::byte{0x31});
    StoreOwnBytes(mapping.Address(), initial);
    require(CollectWritesUncached(mapping.Address(), bytes) != 0, "direct ownership: initial source collection failed");
    struct Pending {
        const Mapping& mapping;
        std::array<std::byte, bytes> value;
        std::uint64_t token = 0;
        std::uint64_t stored = 0;
        unsigned resolutions = 0;
        ~Pending() { if (token != 0) GuestArena::GuestArenaReleaseAccess_nid_postfix(token); }
        void Arm(std::byte byte) {
            value.fill(byte);
            GuestArena::MappingSpan span{};
            std::size_t count = 0;
            require(GuestArena::GuestArenaCaptureMapping_nid_postfix(mapping.Address(), bytes, &span, 1, &count) && count == 1, "direct ownership: fresh mapping incarnation is unavailable");
            token = GuestArena::GuestArenaTryProtectMapping_nid_postfix(&span, 1, mapping.Address(), bytes, +[](std::uint64_t token, void* context) {
                auto& pending = *static_cast<Pending*>(context);
                ++pending.resolutions;
                StoreOwnBytes(pending.mapping.Address(), pending.value);
                pending.stored = TrackerGeneration();
                GuestArena::GuestArenaReleaseAccess_nid_postfix(token);
                pending.token = 0;
                return true;
            }, this, +[](void*) {});
            require(token != 0, "direct ownership: fresh unaliased mapping refused native ownership");
        }
    } pending{mapping};
    pending.Arm(std::byte{0x5a});
    require(!GuestArena::GuestArenaResidentSpan_nid_postfix(mapping.Address(), bytes, &residentBegin, &residentEnd), "direct ownership: a fresh kernel mapping is resident");
    auto* native = static_cast<volatile std::byte*>(mapping.data);
    native[8] = std::byte{0x31};
    require(pending.resolutions == 1 && native[8] == std::byte{0x31} && native[9] == std::byte{0x5a}, "direct ownership: same-value native store did not materialize before overwriting");
    require(CollectWritesUncached(mapping.Address(), bytes) != 0, "direct ownership: native store collection failed");
    const std::array generations{pending.stored};
    std::array<std::uint8_t, 1> changed{}, cpu{};
    require(ChangedBlocks(mapping.Address(), bytes, generations, changed, cpu) && changed[0] != 0 && cpu[0] != 0, "direct ownership: same-value native CPU intent was lost");
    pending.Arm(std::byte{0x6b});
    {
        Alias alias(mapping);
        require(pending.resolutions == 2 && pending.token == 0, "direct ownership: kernel alias creation did not resolve existing ownership first");
        require(CollectWritesUncached(mapping.Address(), bytes) != 0 && CollectWritesUncached(alias.Address(), bytes) != 0, "direct ownership: new shared alias collection failed");
        pending.Arm(std::byte{0x6c});
        require(GuestArena::GuestArenaMappingProtected_nid_postfix(alias.Address(), bytes), "direct ownership: a shared alias bypassed live ownership");
        auto* aliased = static_cast<volatile std::byte*>(alias.data);
        aliased[9] = std::byte{0x6b};
        require(pending.resolutions == 3 && native[9] == std::byte{0x6b} && aliased[10] == std::byte{0x6c}, "direct ownership: alias same-value store lost ordering or shared bytes");
        require(CollectWritesUncached(mapping.Address(), bytes) != 0 && CollectWritesUncached(alias.Address(), bytes) != 0, "direct ownership: shared alias collection failed");
        pending.Arm(std::byte{0x7c});
    }
    require(pending.resolutions == 4 && pending.token == 0, "direct ownership: alias unmap did not resolve pending ownership first");
    std::array<std::byte, bytes> result;
    CopyDriverMemory(result.data(), mapping.data, result.size());
    require(std::all_of(result.begin(), result.end(), [](std::byte value) { return value == std::byte{0x7c}; }), "direct ownership: alias teardown damaged original backing bytes");
}

void mappingWatchRestoration() {
    constexpr std::size_t blockBytes = 65536;
    constexpr std::size_t pageBytes = 16384;
    auto* data = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * blockBytes, blockBytes));
    struct Release {
        std::byte* data;
        HANDLE section = nullptr;
        ~Release() {
            GuestArena::GuestArenaReset_nid_postfix(data, 2 * blockBytes);
            GuestArena::GuestArenaRelease_nid_postfix(data, 2 * blockBytes);
            if (section != nullptr) CloseHandle(section);
        }
    } release{data};
    release.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, 2 * blockBytes, nullptr);
    require(release.section != nullptr, "mapping watch: cannot create shared backing");
    const auto address = reinterpret_cast<std::uint64_t>(data);
    GuestArena::GuestArenaCommit_nid_postfix(data, 2 * blockBytes, PAGE_READWRITE, blockBytes);
    require(CollectWritesUncached(address, 2 * blockBytes) != 0, "mapping watch: private baseline is not tracked");
    Unwatch(address, 2 * blockBytes);
    require(!Watched(address, 2 * blockBytes), "mapping watch: exclusion control did not exclude coverage");
    GuestArena::GuestArenaReset_nid_postfix(data, 2 * blockBytes);
    GuestArena::GuestArenaMap_nid_postfix(data, blockBytes, release.section, 0, PAGE_READWRITE);
    require(Watched(address, blockBytes) && !Watched(address + blockBytes, blockBytes), "mapping watch: fresh shared mapping did not restore only its own block");
    const auto beforeWrite = CollectWritesUncached(address, blockBytes);
    require(beforeWrite != 0, "mapping watch: restored shared mapping cannot be collected");
    const auto same = *reinterpret_cast<volatile std::byte*>(data);
    *reinterpret_cast<volatile std::byte*>(data) = same;
    require(CollectWritesUncached(address, blockBytes) != 0 && !UnchangedSince(address, 1, beforeWrite), "mapping watch: same-value native CPU write was lost after restoration");
    const auto firstStable = CollectWritesUncached(address, blockBytes);
    {
        struct Exclusion {
            bool held = GuestArena::GuestArenaBeginMappingExclusion_nid_postfix(2);
            ~Exclusion() { if (held) GuestArena::GuestArenaEndMappingExclusion_nid_postfix(); }
        } exclusion;
        GuestArena::GuestArenaMap_nid_postfix(data + blockBytes, blockBytes, release.section, blockBytes, PAGE_READWRITE);
        require(!Watched(address + blockBytes, blockBytes), "mapping watch: observer ran before standalone exclusion release");
    }
    require(Watched(address + blockBytes, blockBytes) && UnchangedSince(address, blockBytes, firstStable), "mapping watch: unrelated mapping failed restoration or invalidated the first block");
    Unwatch(address, blockBytes);
    {
        GuestAllocations::Mutation mutation;
        GuestAllocations::GuestAllocationsAfterMutation_nid_postfix([address] { Unwatch(address, blockBytes); });
        GuestArena::GuestArenaMap_nid_postfix(data, blockBytes, release.section, 0, PAGE_READWRITE);
    }
    require(!Watched(address, blockBytes), "mapping watch: a notification erased a same-incarnation exclusion");
    const auto beforeAba = TrackerGeneration();
    {
        GuestAllocations::Mutation mutation;
        GuestArena::GuestArenaMap_nid_postfix(data, blockBytes, release.section, 0, PAGE_READWRITE);
        GuestArena::GuestArenaMap_nid_postfix(data, blockBytes, release.section, blockBytes, PAGE_READWRITE);
        GuestArena::GuestArenaMap_nid_postfix(data, blockBytes, release.section, 0, PAGE_READWRITE);
        require(!Watched(address, blockBytes), "mapping watch: observer ran inside allocation mutation");
    }
    require(Watched(address, blockBytes) && TrackerGeneration() == beforeAba + 1, "mapping watch: stale ABA observers ran or current mapping failed restoration");
    GuestArena::MappingSpan old{};
    std::size_t count = 0;
    require(GuestArena::GuestArenaCaptureMapping_nid_postfix(address, blockBytes, &old, 1, &count) && count == 1, "mapping watch: missing current incarnation");
    Unwatch(address, blockBytes);
    GuestArena::GuestArenaMap_nid_postfix(data, pageBytes, release.section, 0, PAGE_READWRITE);
    require(!Watched(address, blockBytes), "mapping watch: a partial mapping restored excluded neighbour bytes");
    bool observed = false;
    require(!GuestArena::GuestArenaObserveSharedMapping_nid_postfix(address, blockBytes, old.identity, +[](void* context) { *static_cast<bool*>(context) = true; }, &observed) && !observed,
            "mapping watch: partial replacement accepted an old full-block incarnation");
    GuestArena::GuestArenaMap_nid_postfix(data + pageBytes, blockBytes - pageBytes, release.section, pageBytes, PAGE_READWRITE);
    require(!Watched(address, blockBytes), "mapping watch: separate partial notifications restored a whole excluded block");
    GuestArena::GuestArenaMap_nid_postfix(data, blockBytes, release.section, 0, PAGE_READWRITE);
    require(Watched(address, blockBytes), "mapping watch: full replacement did not recover partial exclusions");
    Unwatch(address, blockBytes);
    GuestArena::GuestArenaMap_nid_postfix(data, blockBytes, release.section, 0, PAGE_WRITECOPY);
    require(!Watched(address, blockBytes), "mapping watch: COW mapping incorrectly restored shared tracking proof");
}

class StoreMappings {
public:
    static constexpr std::size_t Page = 16384;
    static constexpr std::size_t Stride = 65536;
    StoreMappings() {
        data = GuestArena::GuestArenaAllocate_nid_postfix(3 * Stride, Stride);
        try {
            for (auto& section : sections) {
                section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Page, nullptr);
                require(section != nullptr, "cannot create the driver-store mapping section");
            }
            FirstBacking();
            GuestArena::GuestArenaMap_nid_postfix(reinterpret_cast<void*>(Alias()), Page, sections[0], 0, PAGE_READWRITE);
            GuestArena::GuestArenaMap_nid_postfix(reinterpret_cast<void*>(Other()), Page, sections[1], 0, PAGE_READWRITE);
        } catch (...) { release(); throw; }
    }
    ~StoreMappings() { release(); }
    StoreMappings(const StoreMappings&) = delete;
    StoreMappings& operator=(const StoreMappings&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(data); }
    std::uint64_t Alias() const { return Address() + Stride; }
    std::uint64_t Other() const { return Address() + 2 * Stride; }
    void FirstBacking() { GuestArena::GuestArenaMap_nid_postfix(data, Page, sections[0], 0, PAGE_READWRITE); }
    void OtherBacking() { GuestArena::GuestArenaMap_nid_postfix(data, Page, sections[1], 0, PAGE_READWRITE); }

private:
    void release() noexcept {
        if (data != nullptr) {
            try {
                GuestArena::GuestArenaReset_nid_postfix(data, 3 * Stride);
                GuestArena::GuestArenaRelease_nid_postfix(data, 3 * Stride);
            } catch (...) {}
            data = nullptr;
        }
        for (auto& section : sections) {
            if (section == nullptr) continue;
            try { GuestArena::GuestArenaForgetSection_nid_postfix(section); } catch (...) {}
            CloseHandle(section);
            section = nullptr;
        }
    }
    void* data = nullptr;
    std::array<HANDLE, 2> sections{};
};

void guardMappingExclusion() {
    StoreMappings mapping;
    GuestArena::MappingSpan span{};
    std::size_t count = 0;
    require(GuestArena::GuestArenaCaptureMapping_nid_postfix(mapping.Address(), 4096, &span, 1, &count) && count == 1, "guard exclusion: missing shared incarnation");
    const auto resolve = +[](std::uint64_t, void*) { return true; };
    for (const bool witnessed : {false, true}) {
        for (const std::uint8_t kind : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}}) {
            std::uint64_t token = 0;
            bool refused = false;
            {
                struct Exclusion {
                    bool held;
                    explicit Exclusion(std::uint8_t kind) : held(GuestArena::GuestArenaBeginMappingExclusion_nid_postfix(kind)) {}
                    ~Exclusion() { if (held) GuestArena::GuestArenaEndMappingExclusion_nid_postfix(); }
                } exclusion(kind);
                struct Release {
                    std::uint64_t& token;
                    ~Release() { if (token != 0) GuestArena::GuestArenaReleaseAccess_nid_postfix(token); }
                } release{token};
                try {
                    token = witnessed ? GuestArena::GuestArenaTryProtectMapping_nid_postfix(&span, 1, mapping.Address(), 4096, resolve, nullptr, nullptr)
                                      : GuestArena::GuestArenaTryProtectAccess_nid_postfix(mapping.Address(), 4096, resolve, nullptr, nullptr);
                } catch (const std::runtime_error&) { refused = true; }
                require((kind == 2 && !refused && token != 0) || (kind != 2 && refused && token == 0), "guard exclusion: standalone borrow or store/registry refusal failed");
                require(GuestArena::GuestArenaMappingExclusionHeld_nid_postfix(), "guard exclusion: nested guard released the caller's exclusion");
                require(GuestArena::GuestArenaMappingProtected_nid_postfix(mapping.Address(), 4096) == (token != 0), "guard exclusion: refusal left native protection or successful borrow installed none");
            }
            require(!GuestArena::GuestArenaMappingExclusionHeld_nid_postfix() && !GuestArena::GuestArenaMappingProtected_nid_postfix(mapping.Address(), 4096), "guard exclusion: completed test leaked exclusion or protection");
        }
    }
}

void storeMappingExclusion() {
    StoreMappings mapping;
    std::array<std::byte, 32> first, other, source, saved, aliasSaved, otherSaved;
    first.fill(std::byte{0x31});
    other.fill(std::byte{0x59});
    source.fill(std::byte{0x44});
    StoreOwnBytes(mapping.Address(), first);
    StoreOwnBytes(mapping.Other(), other);
    CollectWritesUncached(mapping.Address(), first.size());
    CollectWritesUncached(mapping.Alias(), first.size());
    CollectWritesUncached(mapping.Other(), other.size());
    const auto controlGeneration = TrackerGeneration();
    auto originalWatch = ReadDriverSnapshot(mapping.Address(), saved);
    auto aliasWatch = ReadDriverSnapshot(mapping.Alias(), aliasSaved);
    auto otherWatch = ReadDriverSnapshot(mapping.Other(), otherSaved);
    StoreOwnBytes(mapping.Address(), source);
    ApplyDriverSnapshot(*originalWatch, saved);
    ApplyDriverSnapshot(*aliasWatch, aliasSaved);
    require(saved == source && aliasSaved == source && otherWatch->State() == DriverStoredSince::No, "normal exact stores lost alias payloads or fabricated an unrelated backing write");
    require(ProvenStoredOver(mapping.Address(), source.size(), controlGeneration) && ProvenStoredOver(mapping.Alias(), source.size(), controlGeneration) && UnchangedSince(mapping.Other(), other.size(), controlGeneration), "normal copy exclusion lost exact overwrite coverage or invalidated untouched backing");

    originalWatch = ReadDriverSnapshot(mapping.Address(), saved);
    aliasWatch = ReadDriverSnapshot(mapping.Alias(), aliasSaved);
    otherWatch = ReadDriverSnapshot(mapping.Other(), otherSaved);
    source.fill(std::byte{0x77});
    std::atomic<bool> attempted{false}, completed{false}, publishedBeforeRemap{false};
    std::exception_ptr remapFailure;
    std::thread remap;
    try {
        StoreOwnBytes(mapping.Address(), source, [&](std::span<const std::byte> bytes) {
            require(GuestArena::GuestArenaMappingExclusionHeld_nid_postfix(), "the driver store did not hold native mapping exclusion");
            CopyDriverMemory(reinterpret_cast<void*>(mapping.Address()), bytes.data(), bytes.size());
            remap = std::thread([&] {
                attempted.store(true, std::memory_order_release);
                try {
                    mapping.OtherBacking();
                    publishedBeforeRemap.store(aliasWatch->State() == DriverStoredSince::Stored, std::memory_order_release);
                } catch (...) { remapFailure = std::current_exception(); }
                completed.store(true, std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!attempted.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            require(attempted.load(std::memory_order_acquire), "the concurrent remap worker did not start");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            require(!completed.load(std::memory_order_acquire), "a remap changed the copy target before alias evidence was published");
        });
    } catch (...) {
        if (remap.joinable()) remap.join();
        throw;
    }
    remap.join();
    if (remapFailure != nullptr) std::rethrow_exception(remapFailure);
    require(completed.load(std::memory_order_acquire) && publishedBeforeRemap.load(std::memory_order_acquire), "the real remap completed before the old backing's alias payload was published");
    ApplyDriverSnapshot(*originalWatch, saved);
    ApplyDriverSnapshot(*aliasWatch, aliasSaved);
    require(saved == source && aliasSaved == source, "a real remap replaced the actual copy's retained source payload");
    require(otherWatch->State() == DriverStoredSince::No, "an untouched backing inherited the previous backing's copy payload");
    std::array<std::byte, 32> actual{};
    GuestArena::GuestArenaCopyDriverMemory_nid_postfix(actual.data(), reinterpret_cast<void*>(mapping.Other()), actual.size());
    require(actual == other, "the concurrent remap changed the untouched backing bytes");
    GuestArena::GuestArenaCopyDriverMemory_nid_postfix(actual.data(), reinterpret_cast<void*>(mapping.Alias()), actual.size());
    require(actual == source, "the actual copy did not reach the old backing alias");

    mapping.FirstBacking();
    originalWatch = ReadDriverSnapshot(mapping.Address(), saved);
    aliasWatch = ReadDriverSnapshot(mapping.Alias(), aliasSaved);
    otherWatch = ReadDriverSnapshot(mapping.Other(), otherSaved);
    source.fill(std::byte{0x99});
    bool refused = false;
    try {
        StoreOwnBytes(mapping.Address(), source, [&](std::span<const std::byte> bytes) {
            CopyDriverMemory(reinterpret_cast<void*>(mapping.Address()), bytes.data(), bytes.size());
            mapping.OtherBacking();
        });
    } catch (const std::runtime_error&) { refused = true; }
    require(refused, "a store callback physically remapped its destination while publishing evidence");
    require(originalWatch->State() == DriverStoredSince::Unknown && aliasWatch->State() == DriverStoredSince::Unknown && otherWatch->State() == DriverStoredSince::No, "refused callback mutation retained exact evidence or poisoned untouched backing");
    GuestArena::GuestArenaCopyDriverMemory_nid_postfix(actual.data(), reinterpret_cast<void*>(mapping.Address()), actual.size());
    require(actual == source, "callback mutation was not refused before replacing the actual copy target");
    GuestArena::GuestArenaCopyDriverMemory_nid_postfix(actual.data(), reinterpret_cast<void*>(mapping.Other()), actual.size());
    require(actual == other, "refused callback mutation wrote the untouched backing");
    refused = false;
    try { StoreOwnBytes(mapping.Address(), source, [&](std::span<const std::byte>) { StoreOwnBytes(mapping.Address(), first); }); }
    catch (const std::runtime_error&) { refused = true; }
    require(refused, "nested driver stores entered the tracker recursively");
    require(!GuestArena::GuestArenaMappingExclusionHeld_nid_postfix(), "a failed or nested driver store leaked native mapping exclusion");
}
#endif

void benchmark(bool labels = false, bool backing = true) {
    require(WriteWatched(), "write tracking unavailable for benchmark");
    std::thread([] {}).join();
    Mapping mapping(2u << 20u);
#ifndef _WIN32
    if (!backing) GuestArena::GuestArenaSetSharedBackingWriter_nid_no_patch(nullptr);
#else
    static_cast<void>(backing);
#endif
    std::vector<std::byte> source(1u << 20u, std::byte{17});
    const auto sizes = labels ? std::array<std::size_t, 3>{4, 8, 16} : std::array<std::size_t, 3>{4096, 65536, 1048576};
    for (const auto bytes : sizes) {
        std::vector<double> times;
        std::uint64_t faults = 0;
        for (unsigned pass = 0; pass < 10; ++pass) {
#ifndef _WIN32
            rusage before{}, after{};
            getrusage(RUSAGE_THREAD, &before);
#endif
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 256; ++i) {
                source[0] = static_cast<std::byte>(i);
                BumpCollectEpoch();
                if (labels) {
                    CheckRange(mapping.data, bytes, 4, true);
                    StoreOwnBytes(mapping.Address(), std::span(source).first(bytes));
                } else Write(mapping.Address(), std::span(source).first(bytes), 1);
            }
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 256;
#ifndef _WIN32
            getrusage(RUSAGE_THREAD, &after);
            if (pass != 0) faults += after.ru_minflt - before.ru_minflt;
#endif
            if (pass != 0) times.push_back(us);
            require(std::memcmp(mapping.data, source.data(), bytes) == 0, "copied data differs");
        }
        std::sort(times.begin(), times.end());
        std::cout << "bytes=" << bytes << " median_us=" << times[4] << " faults=" << faults << " operations=2304\n";
    }
}

void smallStores() {
    Mapping mapping(2u << 16u);
    Alias alias(mapping);
    auto* data = static_cast<std::byte*>(mapping.data);
    std::memset(data, 19, mapping.size);
    std::array<std::byte, 16> source;
    source.fill(std::byte{83});
    for (const auto offset : {4u, 4092u, 65532u}) for (const auto size : {4u, 8u, 16u}) {
        const auto address = mapping.Address() + offset;
        const auto before = CollectWritesUncached(address, size);
        const auto stored = StoreOwnBytes(address, std::span(source).first(size));
        require(std::memcmp(data + offset, source.data(), size) == 0, "small backing store differs");
        require(data[offset - 1] == std::byte{19} && data[offset + 16] == std::byte{19}, "small store changed neighboring bytes");
        if (before != 0) {
            require(stored > before && StoredOver(address, size, before), "small driver store was not stamped");
            require(UnchangedSinceCollected(address, size, before), "driver store was classified as a guest write");
            data[offset] = std::byte{97};
            StoreOwnBytes(address, std::span(source).first(size));
            require(!UnchangedSinceCollected(address, size, stored), "guest write before a driver store was lost");
            const auto after = CollectWritesUncached(address, size);
            data[offset] = std::byte{101};
            require(!UnchangedSinceCollected(address, size, after), "guest write after a driver store was lost");
        }
    }
    std::array<std::byte, 16> ordinary{};
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(ordinary.data()), source);
    require(ordinary == source, "small ordinary store fallback differs");
    require(StoreOwnBytes(0, std::span<const std::byte>{}) == 0, "empty store produced a version");
}

void storeProvenance() {
#ifdef _WIN32
    Mapping mapping(65536);
    Alias alias(mapping);
    auto* data = static_cast<volatile std::byte*>(mapping.data);
    std::memset(mapping.data, 19, mapping.size);
    std::array<std::byte, 8> source;
    source.fill(std::byte{83});
    const auto edge = mapping.Address() + 128;
    const auto otherEdge = mapping.Address() + 4096 + 128;
    const auto copied = alias.Address() + 4092;
    const auto beforeDriver = CollectWritesUncached(mapping.Address(), mapping.size);
    require(beforeDriver != 0, "provenance test range is not watched");
    StoreOwnBytes(copied, source);
    require(StoresSince(edge, 16, beforeDriver) == StoreProvenance::Unchanged && !StoredOver(edge, 16, beforeDriver), "an unrelated same-page driver store proved a CPU overwrite");
    require(UnchangedSinceCollected(edge, 16, beforeDriver), "an unrelated driver store invalidated a CPU-only proof");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;
    require(DriverStoresSince(mapping.Address() + 4092, source.size(), beforeDriver, pieces) && pieces == std::vector{std::pair{mapping.Address() + 4092, mapping.Address() + 4100}}, "a proven alias driver store lost its exact pieces");
    require(ProvenStoredOver(mapping.Address() + 4092, source.size(), beforeDriver), "an exact full-range driver copy did not prove full overwrite coverage");
    bool partialRefused = false;
    try { static_cast<void>(ProvenStoredOver(mapping.Address() + 4092, 16, beforeDriver)); }
    catch (const std::runtime_error&) { partialRefused = true; }
    require(partialRefused, "a partial exact driver copy authorized discarding untouched GPU bytes");
    StoreOwnBytes(copied + 8, source);
    require(ProvenStoredOver(mapping.Address() + 4092, 16, beforeDriver), "the union of exact newer driver stores did not prove full overwrite coverage");

    const auto beforeCpu = CollectWritesUncached(mapping.Address(), mapping.size);
    StoreOwnBytes(copied, source.size(), [&] {
        std::thread([&] { data[128] = std::byte{19}; }).join();
        CopyDriverMemory(reinterpret_cast<void*>(copied), source.data(), source.size());
    });
    require(data[128] == std::byte{19}, "the same-value CPU write changed its fixture value");
    require(StoresSince(mapping.Address(), 4096, beforeCpu) == StoreProvenance::Written, "a same-value CPU write during a driver copy lost its overwrite intent");
    bool cpuRefused = false;
    try { static_cast<void>(ProvenStoredOver(mapping.Address(), 4096, beforeCpu)); }
    catch (const std::runtime_error&) { cpuRefused = true; }
    require(cpuRefused, "a one-byte CPU event authorized discarding the rest of a GPU page");
    require(StoresSince(edge, 16, beforeCpu) == StoreProvenance::Unknown, "page-level native evidence falsely proved the CPU store's byte extent");
    require(!DriverStoresSince(edge, 16, beforeCpu, pieces) && !UnchangedSinceCollected(edge, 16, beforeCpu), "a CPU write during a driver copy retained a driver-only or unchanged proof");
    require(StoresSince(otherEdge, 16, beforeCpu) == StoreProvenance::Unchanged, "a CPU fault on another page classified a driver-only page as CPU written");
    const std::array generations{beforeCpu};
    std::array<std::uint8_t, 1> changed{}, cpu{};
    require(ChangedBlocks(mapping.Address(), mapping.size, generations, changed, cpu) && changed[0] == BlockWritten && cpu[0] == BlockWritten, "the native CPU write did not update block CPU evidence");

    const auto beforeRaw = CollectWritesUncached(mapping.Address(), mapping.size);
    StoreOwnBytes(copied, source.size(), [&] {
        data[128] = std::byte{19};
        CopyDriverMemory(reinterpret_cast<void*>(copied), source.data(), source.size());
    });
    require(StoresSince(edge, 16, beforeRaw) == StoreProvenance::Unknown && StoredOver(edge, 16, beforeRaw), "a raw callback fault was treated as definite CPU or driver provenance");
    require(StoresSince(mapping.Address(), mapping.size, beforeRaw) == StoreProvenance::Unknown, "a known driver piece masked unknown provenance elsewhere in the same GPU unit");
    require(!DriverStoresSince(edge, 16, beforeRaw, pieces) && !UnchangedSinceCollected(edge, 16, beforeRaw) && !UnchangedSince(edge, 16, beforeRaw), "an unknown store retained a cache or source proof");
    bool refused = false;
    try { static_cast<void>(ProvenStoredOver(edge, 16, beforeRaw)); }
    catch (const std::runtime_error& error) { refused = std::string_view(error.what()).find("unknown provenance") != std::string_view::npos; }
    require(refused && data[128] == std::byte{19}, "unknown provenance did not refuse before a GPU-result discard decision");
    const auto settled = CollectWritesUncached(mapping.Address(), mapping.size);
    require(StoresSince(edge, 16, settled) == StoreProvenance::Unchanged, "settled unknown evidence invalidated a newer baseline");

    void* resident = GuestArena::GuestArenaAllocate_nid_postfix(65536, 65536);
    require(resident != nullptr, "resident provenance allocation failed");
    struct ReleaseResident {
        void* data;
        ~ReleaseResident() {
            GuestArena::GuestArenaReset_nid_postfix(data, 65536);
            GuestArena::GuestArenaRelease_nid_postfix(data, 65536);
        }
    } releaseResident{resident};
    GuestArena::GuestArenaCommit_nid_postfix(resident, 65536, PAGE_READWRITE, 65536);
    const auto residentAddress = reinterpret_cast<std::uint64_t>(resident);
    const auto residentBefore = CollectWritesUncached(residentAddress, 65536);
    require(residentBefore != 0, "resident provenance range is not watched");
    StoreOwnBytes(residentAddress + 2048, source);
    require(StoresSince(residentAddress + 128, 16, residentBefore) == StoreProvenance::Unknown && !DriverStoresSince(residentAddress + 128, 16, residentBefore, pieces), "resident dirty pages acquired definite driver or CPU provenance");
#endif
}

void aliasSnapshotWatches() {
    Mapping mapping(65536);
    std::memset(mapping.data, 19, mapping.size);
    std::array<std::byte, 8> saved{};
    const auto watch = ReadDriverSnapshot(mapping.Address() + 4092, saved, 4);
    void* alias = nullptr;
    require(sceKernelMapDirectMemory(&alias, mapping.size, 3, 0, mapping.physical, 65536) == 0, "snapshot alias mapping failed");
    struct Unmap {
        void* view;
        std::size_t bytes;
        ~Unmap() { sceKernelMunmap(view, bytes); }
    } unmap{alias, mapping.size};
    std::array<std::byte, 8> source;
    source.fill(std::byte{83});
    const bool dirtySet = EpochDirtySetEnabled();
    SetEpochDirtySet(true);
    BumpCollectEpoch();
    const auto generation = CollectWrites(mapping.Address(), mapping.size);
    const auto dirtySerial = DirtySerial();
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(alias) + 4092, source);
    require(watch->Stored(), "a store through an alias created after the snapshot did not invalidate its watch");
    std::memset(static_cast<std::byte*>(mapping.data) + 4092, 37, saved.size());
    ApplyDriverSnapshot(*watch, saved);
    require(saved == source, "a later alias payload reread CPU-reused source words");
    require(!UnchangedSince(mapping.Address() + 4092, source.size(), generation), "an alias driver store retained the original view's generation proof");
    require(!UnchangedSinceDirtySerial(mapping.Address() + 4092, source.size(), dirtySerial), "an alias driver store retained the original view's dirty-set proof");
    require(CollectWrites(mapping.Address(), mapping.size) > generation, "a memoized collect did not see an alias driver stamp in the same epoch");
    require(StoredOver(mapping.Address() + 4092, source.size(), generation), "an alias driver store lost its precise original-view driver piece");
    const auto cpuBefore = CollectWritesUncached(mapping.Address(), mapping.size);
    static_cast<std::byte*>(mapping.data)[128] = std::byte{97};
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(alias) + 4092, source);
    const std::array generations{cpuBefore};
    std::array<std::uint8_t, 1> changed{}, cpu{};
    require(ChangedBlocks(mapping.Address(), mapping.size, generations, changed, cpu) && changed[0] == BlockWritten && cpu[0] == BlockWritten, "an alias driver store hid an earlier CPU write in the original view's block");
    const std::array ranges{std::pair{mapping.Address() + 4092, mapping.Address() + 4100}};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> logged;
    LogDriverStores(&logged);
    struct RestoreLog {
        ~RestoreLog() { LogDriverStores(nullptr); }
    } restoreLog;
    std::uint64_t beforeCopy = 0;
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(alias) + 4092, source.size(), [&] {
        beforeCopy = DriverStoreSequence();
        CopyDriverMemory(static_cast<std::byte*>(alias) + 4092, source.data(), source.size());
    });
    LogDriverStores(nullptr);
    require(DriverStoresSinceSequence(beforeCopy, ranges) == DriverStoredSince::Stored, "the driver sequence log did not publish an alias range after its actual copy");
    require(std::find(logged.begin(), logged.end(), ranges.front()) != logged.end(), "the record-effects store log omitted a driver-written alias range");
    SetEpochDirtySet(dirtySet);
    require(saved == source, "a later alias store changed the retained driver payload");
    std::array<std::byte, 8> current{};
    Read(mapping.Address() + 4092, current, 4);
    require(current == source, "the original view did not see the alias's driver store");
    const auto reverse = WatchDriverStores(reinterpret_cast<std::uintptr_t>(alias) + 16, 8);
    const auto unrelated = WatchDriverStores(reinterpret_cast<std::uintptr_t>(alias) + 32, 8);
    StoreOwnBytes(mapping.Address() + 16, source);
    require(reverse->Stored() && !unrelated->Stored(), "alias invalidation missed a reverse store or invalidated an unrelated range");
}

void possibleWriteExtents() {
    Mapping mapping(65536);
    Alias alias(mapping);
    std::array<std::byte, 4096> page;
    page.fill(std::byte{0x31});
    StoreOwnBytes(mapping.Address(), page);
    const auto base = mapping.Address();
    const auto before = CollectWritesUncached(base, page.size());
    require(before != 0, "possible-write test page is not watched");
    std::array<std::byte, 8> saved{};
    const auto tiny = ReadDriverSnapshot(base + 128, saved, 4);
    const auto sequence = DriverStoreSequence();
    const std::uint32_t changed = 0x74747474u;
    CopyDriverMemory(mapping.data, &changed, sizeof(changed));
    const auto possible = MarkWritten(base, page.size(), WriteExtent::Possible);
    require(possible > before && !UnchangedSince(base, page.size(), before), "a possible page write retained a cached CPU proof");
    require(tiny->State() == DriverStoredSince::Unknown && DriverStoresSinceSequence(sequence, std::array{std::pair{base + 128, base + 136}}) == DriverStoredSince::Unknown, "a page-bound shader write falsely proved overlap with tiny register words");
    require(std::all_of(saved.begin(), saved.end(), [](auto value) { return value == std::byte{0x31}; }), "a page-bound shader stamp changed frozen register bytes");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;
    require(StoresSince(base, page.size(), before) == StoreProvenance::Unknown && !DriverStoresSince(base, page.size(), before, pieces), "possible page evidence fabricated exact driver coverage");
    bool refused = false;
    try { static_cast<void>(ProvenStoredOver(base, page.size(), before)); }
    catch (const std::runtime_error&) { refused = true; }
    require(refused, "a one-word shader store authorized dropping the rest of its page");
    const auto beforeBroad = CollectWritesUncached(base, page.size());
    const std::array broad{std::pair{base, base + page.size()}};
    const auto marked = MarkWrittenUnlessStored(broad, beforeBroad, WriteExtent::Possible);
    require(marked.generation > beforeBroad && StoresSince(base, page.size(), beforeBroad) == StoreProvenance::Unknown, "a broad writable descriptor fabricated exact overwrite coverage");
    const auto beforeExact = CollectWritesUncached(base, page.size());
    StoreOwnBytes(base, page);
    require(ProvenStoredOver(base, page.size(), beforeExact), "a complete actual copy lost exact full-range coverage");

    std::array<std::byte, 64> original;
    original.fill(std::byte{0x31});
    StoreOwnBytes(base + 8192, original);
    const auto beforeNoop = CollectWritesUncached(base + 8192, original.size());
    const auto noop = WatchDriverStores(base + 8192 + 16, 8);
    const auto noopSequence = DriverStoreSequence();
    WriteChanged(base + 8192, original, original);
    require(noop->State() == DriverStoredSince::No && DriverStoreSequence() == noopSequence && StoresSince(base + 8192, original.size(), beforeNoop) == StoreProvenance::Unchanged, "an unchanged masked copy fabricated a mutation or poisoned a register watch");
    auto sparse = original;
    sparse.front() = std::byte{0x42};
    sparse.back() = std::byte{0x43};
    StoreOwnBytes(base + 8192, original);
    const auto beforeSparse = CollectWritesUncached(base + 8192, original.size());
    const auto interior = WatchDriverStores(base + 8192 + 16, 8);
    WriteChanged(base + 8192, sparse, original);
    std::array<std::byte, 64> actual{};
    Read(base + 8192, actual);
    require(actual == sparse && interior->State() == DriverStoredSince::Unknown, "sparse changed-copy holes became exact watched stores");
    require(StoresSince(base + 8192 + 16, 8, beforeSparse) == StoreProvenance::Unknown && !DriverStoresSince(base + 8192 + 16, 8, beforeSparse, pieces), "a sparse bounding interval proved untouched interior bytes overwritten");
    refused = false;
    try { static_cast<void>(ProvenStoredOver(base + 8192, original.size(), beforeSparse)); }
    catch (const std::runtime_error&) { refused = true; }
    require(refused, "two changed endpoint bytes authorized discarding the untouched GPU interior");
}

void failedStoreEvidence() {
    Mapping mapping(65536);
    Alias alias(mapping);
    std::array<std::byte, 32> original;
    original.fill(std::byte{0x31});
    StoreOwnBytes(mapping.Address(), original);
    const auto before = CollectWritesUncached(mapping.Address(), original.size());
    const auto watched = WatchDriverStores(mapping.Address() + 16, 8);
    const auto aliasWatch = WatchDriverStores(alias.Address() + 16, 8);
    bool failed = false;
    const std::uint32_t changed = 0x74747474u;
    try {
        StoreOwnBytes(mapping.Address(), original, [&](std::span<const std::byte>) {
            CopyDriverMemory(mapping.data, &changed, sizeof(changed));
            throw std::runtime_error("partial driver store fixture");
        });
    } catch (const std::runtime_error& error) { failed = std::string_view(error.what()) == "partial driver store fixture"; }
    require(failed && watched->State() == DriverStoredSince::Unknown && aliasWatch->State() == DriverStoredSince::Unknown, "a partial throwing driver callback lost its error or retained an exact snapshot proof");
    require(!UnchangedSince(mapping.Address(), original.size(), before) && StoresSince(mapping.Address(), original.size(), before) == StoreProvenance::Unknown, "a partial throwing driver callback retained cached or exact overwrite evidence");
    std::array<std::byte, 32> actual{};
    Read(mapping.Address(), actual);
    require(std::memcmp(actual.data(), &changed, sizeof(changed)) == 0 && std::equal(actual.begin() + sizeof(changed), actual.end(), original.begin() + sizeof(changed)), "throwing driver cleanup changed the actual partial-store bytes");
}

void partialCollect() {
    constexpr std::size_t page = 4096;
    Mapping mapping(page * 4);
    auto* data = static_cast<volatile std::byte*>(mapping.data);
#ifdef _WIN32
    void* alias = nullptr;
    require(sceKernelMapDirectMemory(&alias, mapping.size, 3, 0, mapping.physical, 65536) == 0, "partial collect alias failed");
#endif
    require(WriteWatched(), "write tracking unavailable for partial collect");
    const auto before = CollectWritesUncached(mapping.Address(), mapping.size);
    require(before != 0, "partial collect range is not watched");
    data[page * 3] = std::byte{91};
    require(CollectWritesUncached(mapping.Address(), page) != 0, "prefix collect failed");
    require(!UnchangedSinceCollected(mapping.Address() + page * 3, page, before), "partial collect lost a neighboring guest write");
    const auto settled = CollectWritesUncached(mapping.Address(), mapping.size);
    require(UnchangedSinceCollected(mapping.Address(), mapping.size, settled), "fully collected shared range remained dirty");
#ifdef _WIN32
    const auto collect = [&](void* view, std::size_t offset, std::size_t bytes, std::size_t capacity, bool clear) {
        std::array<void*, 4> pages{};
        std::size_t count = capacity;
        const auto address = reinterpret_cast<std::uintptr_t>(view);
        require(GuestArena::GuestArenaCollectWrites_nid_postfix(address + offset, bytes, pages.data(), &count, clear), "shared subpage collection failed");
        std::vector<std::size_t> offsets;
        for (std::size_t i = 0; i < count; ++i) offsets.push_back(reinterpret_cast<std::uintptr_t>(pages[i]) - address);
        return offsets;
    };
    const std::vector<std::size_t> none;
    collect(alias, 0, mapping.size, 4, true);
    data[page * 3] = std::byte{92};
    require(collect(mapping.data, 0, page * 3, 4, false) == none, "a shared write was reported for the pages next to it");
    require(collect(mapping.data, 0, mapping.size, 4, false) == std::vector<std::size_t>{page * 3}, "probe missed a shared write");
    require(collect(mapping.data, 0, mapping.size, 4, false) == std::vector<std::size_t>{page * 3}, "probe consumed a shared write");
    require(collect(mapping.data, 0, page, 4, true) == none, "prefix reset reported an unwritten page");
    require(collect(alias, 0, mapping.size, 4, false) == std::vector<std::size_t>{page * 3}, "prefix reset consumed an alias write");
    require(collect(mapping.data, page * 3, page, 4, true) == std::vector<std::size_t>{page * 3}, "prefix reset consumed a neighboring write");
    require(collect(mapping.data, page * 3, page, 4, false).empty(), "reset subpage remained dirty");
    require(collect(mapping.data, 0, mapping.size, 4, false).empty(), "collected subpages remained dirty");
    require(collect(alias, 0, mapping.size, 4, true) == std::vector<std::size_t>{page * 3}, "collecting one view consumed an alias write");
    static_cast<volatile std::byte*>(alias)[page] = std::byte{93};
    require(collect(mapping.data, page * 2, page, 4, true) == none, "an alias write was reported for another page");
    require(collect(mapping.data, 0, mapping.size, 4, true) == std::vector<std::size_t>{page}, "alias write did not invalidate the other view");
    require(collect(mapping.data, 0, mapping.size, 4, false).empty(), "full collect left pending subpages");
    require(collect(alias, 0, mapping.size, 4, true) == std::vector<std::size_t>{page}, "alias write was consumed for its own view");
    for (std::size_t at = 0; at < mapping.size; at += page) static_cast<volatile std::byte*>(alias)[at + 8] = std::byte{99};
    require(collect(mapping.data, page, page * 2, 4, true) == std::vector<std::size_t>{page, page * 2}, "partial resets consumed intervening writes");
    require(collect(mapping.data, page, page * 2, 4, false).empty(), "interior reset left its subpages dirty");
    require(collect(mapping.data, 0, mapping.size, 4, true) == std::vector<std::size_t>{0, page * 3}, "full collect after an interior reset did not preserve pending subpages");
    require(collect(alias, 0, mapping.size, 4, true) == std::vector<std::size_t>{0, page, page * 2, page * 3}, "an interior reset consumed the alias's writes");
    static_cast<volatile std::byte*>(alias)[0] = std::byte{94};
    static_cast<volatile std::byte*>(alias)[page * 2] = std::byte{95};
    static_cast<volatile std::byte*>(alias)[page * 3] = std::byte{96};
    require(collect(mapping.data, 0, mapping.size, 1, true) == std::vector<std::size_t>{0}, "limited collect returned the wrong prefix");
    require(collect(mapping.data, 0, mapping.size, 4, false) == std::vector<std::size_t>{page * 2, page * 3}, "limited collect consumed unreported writes");
    require(collect(mapping.data, page, page * 3, 1, true) == std::vector<std::size_t>{page * 2}, "limited collect continuation lost writes");
    require(collect(mapping.data, page * 3, page, 4, true) == std::vector<std::size_t>{page * 3}, "limited collect consumed a write after its last report");
    require(collect(mapping.data, 0, mapping.size, 4, false).empty(), "limited collection did not settle");
    require(collect(alias, 0, mapping.size, 4, true) == std::vector<std::size_t>{0, page * 2, page * 3}, "limited collection consumed the alias's writes");
    data[page] = std::byte{97};
    data[page * 2] = std::byte{98};
    require(collect(alias, 0, mapping.size, 4, true) == std::vector<std::size_t>{page, page * 2}, "writes to two pages were not reported apart");
    require(collect(mapping.data, 0, mapping.size, 4, true) == std::vector<std::size_t>{page, page * 2}, "writes to two pages were not reported for their own view");
    require(collect(mapping.data, 0, mapping.size, 4, false).empty() && collect(alias, 0, mapping.size, 4, false).empty(), "page tracking did not settle");
    require(sceKernelMunmap(alias, mapping.size) == 0, "partial collect alias unmap failed");
#endif
    std::cout << "Partial shared write collection tests passed\n";
}

template<class TAction> void fails(TAction&& fn) {
    bool failed = false;
    try { fn(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "invalid write was accepted");
}

#ifndef _WIN32
std::size_t directMappings() {
    std::ifstream maps("/proc/self/maps");
    require(maps.is_open(), "cannot inspect mapping lifetimes");
    std::size_t count = 0;
    for (std::string line; std::getline(maps, line);) if (line.find("/memfd:direct memory") != std::string::npos) ++count;
    return count;
}
#endif

void run() {
    constexpr std::size_t part = 65536;
    Mapping mapping(2u << 20u);
    auto* bytes = static_cast<std::byte*>(mapping.data);
    std::memset(bytes, 19, mapping.size);
    std::vector<std::byte> source(part, std::byte{73});
    const auto before = CollectWritesUncached(mapping.Address(), mapping.size);
    Write(mapping.Address() + 123, source, 1);
    require(std::memcmp(bytes + 123, source.data(), source.size()) == 0, "shared write differs");
    require(bytes[122] == std::byte{19} && bytes[123 + part] == std::byte{19}, "write changed neighboring bytes");
    if (before != 0) {
        require(!UnchangedSince(mapping.Address(), mapping.size, before), "driver write did not advance versions");
        require(Watched(mapping.Address(), mapping.size), "host view disabled guest tracking");
        const auto after = CollectWritesUncached(mapping.Address(), mapping.size);
        require(UnchangedSince(mapping.Address(), mapping.size, after), "fresh write version is stale");
        bytes[part + 456] = std::byte{91};
        CollectWritesUncached(mapping.Address(), mapping.size);
        require(!UnchangedSince(mapping.Address(), mapping.size, after), "subsequent guest write was lost");
#ifndef _WIN32
        std::size_t dirty = 0;
        const auto collect = [&] {
            return GuestWriteWatch::GuestWriteWatchCollect_nid_postfix(mapping.Address(), mapping.size,
                [](void* p, std::uintptr_t begin, std::uintptr_t end) { *static_cast<std::size_t*>(p) += end - begin; }, &dirty);
        };
        require(collect(), "guest mapping is no longer watched");
        dirty = 0;
        require(GuestArena::GuestArenaWriteSharedBacking_nid_no_patch(mapping.Address(), source.data(), source.size()), "shared writer rejected direct backing");
        require(collect() && dirty == 0, "host write dirtied the tracked guest mapping");
        bytes[part * 4] = std::byte{31};
        require(collect() && dirty == 4096, "guest write tracking was disarmed");
#endif
    }
    require(sceKernelMprotect(mapping.data, part, 1) == 0, "read-only protection failed");
    fails([&] { Write(mapping.Address(), source, 1); });
    require(sceKernelMprotect(mapping.data, part, 3) == 0, "write protection restore failed");
    std::vector<std::byte> ordinary(part);
    Write(reinterpret_cast<std::uint64_t>(ordinary.data()), source, 1);
    require(ordinary == source, "ordinary memory fallback failed");
    {
        Mapping replacement(part);
        void* originalAlias = nullptr;
        require(sceKernelMapDirectMemory(&originalAlias, part, 3, 0, mapping.physical + part, part) == 0, "physical alias failed");
        std::memset(originalAlias, 29, part);
        void* middle = bytes + part;
        require(sceKernelMapDirectMemory(&middle, part, 3, 0x10, replacement.physical, part) == 0, "fixed backing replacement failed");
        std::vector<std::byte> across(part * 3);
        for (std::size_t i = 0; i < across.size(); ++i) across[i] = static_cast<std::byte>((i / part) + 41);
        Write(mapping.Address(), across, 1);
        require(std::memcmp(mapping.data, across.data(), across.size()) == 0, "write across backing boundaries differs");
        require(std::memcmp(replacement.data, across.data() + part, part) == 0, "physical alias did not see the write");
        require(std::all_of(static_cast<std::byte*>(originalAlias), static_cast<std::byte*>(originalAlias) + part, [](auto x) { return x == std::byte{29}; }), "replaced virtual range wrote the old backing");
#ifndef _WIN32
        require(sceKernelMunmap(middle, part) == 0, "partial unmap failed");
        const auto saved = bytes[0];
        across[0] = std::byte{99};
        require(!GuestArena::GuestArenaWriteSharedBacking_nid_no_patch(mapping.Address(), across.data(), across.size()), "writer accepted an unmapped gap");
        require(bytes[0] == saved, "failed shared write changed its prefix");
#endif
        require(sceKernelMapDirectMemory(&middle, part, 3, 0x10, mapping.physical + part, part) == 0, "original backing restore failed");
        require(sceKernelMunmap(originalAlias, part) == 0, "alias unmap failed");
    }
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&, i] {
        std::vector<std::byte> input(part, static_cast<std::byte>(i + 57));
        for (unsigned repeat = 0; repeat < 32; ++repeat) Write(mapping.Address() + i * part, input, 1);
    });
    for (auto& thread : threads) thread.join();
    for (unsigned i = 0; i < 8; ++i) require(std::all_of(bytes + i * part, bytes + (i + 1) * part, [i](auto x) { return x == static_cast<std::byte>(i + 57); }), "concurrent write differs");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--direct-mapping-ownership") {
#ifdef _WIN32
            if (!WriteWatched()) return 77;
            directMappingOwnership();
            std::cout << "Kernel direct mappings preserve native ownership, same-value writes and alias teardown\n";
#else
            return 77;
#endif
        }
        else if (argc == 2 && std::string_view(argv[1]) == "--guard-mapping-exclusion") {
#ifdef _WIN32
            if (!WriteWatched()) return 77;
            guardMappingExclusion();
            std::cout << "Native guards borrow standalone exclusion and refuse registry/store modes\n";
#else
            return 77;
#endif
        }
        else if (argc == 2 && std::string_view(argv[1]) == "--mapping-watch") {
#ifdef _WIN32
            if (!WriteWatched()) return 77;
            mappingWatchRestoration();
            std::cout << "Fresh shared mappings restored incarnation-checked write tracking\n";
#else
            return 77;
#endif
        }
        else if (argc == 2 && std::string_view(argv[1]) == "--mapping-exclusion") {
#ifdef _WIN32
            if (!WriteWatched()) return 77;
            storeMappingExclusion();
            std::cout << "Driver stores preserved real copy targets across remaps\n";
#else
            return 77;
#endif
        }
        else if (argc == 2 && std::string_view(argv[1]) == "--partial-collect") {
            if (!WriteWatched()) {
                std::cout << "Partial shared write collection skipped: write tracking unavailable\n";
                return 77;
            }
            partialCollect();
        }
        else if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmark();
        else if (argc == 2 && std::string_view(argv[1]) == "--benchmark-labels") benchmark(true);
        else if (argc == 2 && std::string_view(argv[1]) == "--benchmark-fallback") benchmark(false, false);
        else if (argc == 2 && std::string_view(argv[1]) == "--benchmark-labels-fallback") benchmark(true, false);
        else {
#ifndef _WIN32
            const auto before = directMappings();
#endif
            run();
            smallStores();
            aliasSnapshotWatches();
            storeProvenance();
            possibleWriteExtents();
            failedStoreEvidence();
#ifndef _WIN32
            require(directMappings() == before, "physical backing write views leaked");
#endif
            std::cout << "Shared backing write tests passed\n";
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

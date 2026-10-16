#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
std::atomic<bool> failNextAllocation{false};
std::atomic<unsigned> failedAllocations{0};
std::array<std::byte, 3 * 65536> dirtyRegions{};
}

void* operator new(std::size_t bytes) {
    if (failNextAllocation.exchange(false, std::memory_order_relaxed)) {
        failedAllocations.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace AgcDriver::GuestMemory;

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<class Query>
void expectRefusal(Query&& query) {
    try { query(); }
    catch (const std::runtime_error& error) {
        require(std::string_view(error.what()).find("driver store evidence was lost") != std::string_view::npos, "proof query threw the wrong failure");
        return;
    }
    throw std::runtime_error("a proof query succeeded after terminal evidence loss");
}

void checkDirtyLogLiveness() {
    require(EpochDirtySetEnabled(), "dirty-log liveness requires the dirty set enabled before its first allocation");
    const auto address = (reinterpret_cast<std::uintptr_t>(dirtyRegions.data()) + 65535u) & ~std::uintptr_t{65535u};
    CheckRange(reinterpret_cast<const void*>(address), 32, 1, true);
    const auto before = DirtySerial();
    require(UnchangedSinceDirtySerial(address, 32, before), "the fixture did not establish a clean initial dirty serial");
    failNextAllocation.store(true, std::memory_order_relaxed);
    bool failed = false;
    try { GuestAllocations::GuestAllocationsInvalidate_nid_postfix(address, 32); }
    catch (const std::bad_alloc&) { failed = true; }
    require(failed && failedAllocations.load(std::memory_order_relaxed) == 1 && !failNextAllocation.load(std::memory_order_relaxed), "the first dirty-log allocation did not consume the injected failure");
    require(DirtySerial() == before + 1, "a failed dirty-log publication retained an old serial proof");
    std::atomic<bool> completed{false};
    std::exception_ptr secondFailure;
    std::thread second([&] {
        try { GuestAllocations::GuestAllocationsInvalidate_nid_postfix(address + 65536, 32); }
        catch (...) { secondFailure = std::current_exception(); }
        completed.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!completed.load(std::memory_order_acquire)) {
        std::fputs("a second dirty-log caller remained blocked after allocation failure\n", stderr);
        std::_Exit(1);
    }
    second.join();
    if (secondFailure) std::rethrow_exception(secondFailure);
    require(DirtySerial() == before + 2 && !DriverStoreEvidenceLost(), "a successful dirty-log retry did not publish exactly one new entry");
    require(!UnchangedSinceDirtySerial(address, 32, before), "an unpublished dirty-log gap was treated as clean for the lost range");
    const auto after = DirtySerial();
    require(UnchangedSinceDirtySerial(address, 32, after), "a completed retry prevented a newly captured serial from proving an unchanged range");
}

#ifdef _WIN32
class SharedAliases {
public:
    static constexpr std::size_t Page = 16384;
    SharedAliases() {
        data = GuestArena::GuestArenaAllocate_nid_postfix(2 * Page, Page);
        try {
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Page, nullptr);
            require(section != nullptr, "cannot create the shared evidence-loss section");
            GuestArena::GuestArenaMap_nid_postfix(data, Page, section, 0, PAGE_READWRITE);
            GuestArena::GuestArenaMap_nid_postfix(static_cast<std::byte*>(data) + Page, Page, section, 0, PAGE_READWRITE);
        } catch (...) { release(); throw; }
    }
    ~SharedAliases() { release(); }
    SharedAliases(const SharedAliases&) = delete;
    SharedAliases& operator=(const SharedAliases&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(data); }
    std::uint64_t Alias() const { return Address() + Page; }

private:
    void release() noexcept {
        if (data != nullptr) {
            try {
                GuestArena::GuestArenaReset_nid_postfix(data, 2 * Page);
                GuestArena::GuestArenaRelease_nid_postfix(data, 2 * Page);
            } catch (...) {}
            data = nullptr;
        }
        if (section != nullptr) {
            try { GuestArena::GuestArenaForgetSection_nid_postfix(section); } catch (...) {}
            CloseHandle(section);
            section = nullptr;
        }
    }
    void* data = nullptr;
    HANDLE section = nullptr;
};

void checkCleanupFailure() {
    SharedAliases mapping;
    const auto address = mapping.Address();
    const auto alias = mapping.Alias();
    std::array<std::byte, 32> initial;
    initial.fill(std::byte{0x31});
    StoreOwnBytes(address, initial);
    CollectWritesUncached(address, initial.size());
    CollectWritesUncached(alias, initial.size());
    const auto generation = TrackerGeneration();
    const auto dirty = DirtySerial();
    const auto sequence = DriverStoreSequence();
    const auto originalWatch = WatchDriverStores(address, initial.size());
    const auto aliasWatch = WatchDriverStores(alias, initial.size());
    require(UnchangedSince(address, initial.size(), generation) && UnchangedSince(alias, initial.size(), generation), "the fixture did not establish valid pre-failure cache proofs");
    require(originalWatch->State() == DriverStoredSince::No && aliasWatch->State() == DriverStoredSince::No, "fresh pre-failure watches already contain a store");
    std::array<std::uintptr_t, 2> begins{}, ends{};
    require(GuestArena::GuestArenaAliasRanges_nid_postfix(address, initial.size(), begins.data(), ends.data(), begins.size()) == 1, "the cleanup fixture needs one real backing alias");
    const auto original = std::make_exception_ptr(std::runtime_error("partial store before cleanup failure"));
    const std::array<std::byte, 4> changed{std::byte{0x72}, std::byte{0x72}, std::byte{0x72}, std::byte{0x72}};
    bool originalPreserved = false;
    try {
        StoreOwnBytes(address, initial.size(), [&] {
            CopyDriverMemory(reinterpret_cast<void*>(address), changed.data(), changed.size());
            failNextAllocation.store(true, std::memory_order_relaxed);
            std::rethrow_exception(original);
        });
    } catch (const std::runtime_error& error) {
        originalPreserved = std::string_view(error.what()) == "partial store before cleanup failure";
    }
    require(originalPreserved, "cleanup failure replaced the original store exception");
    require(failedAllocations.load(std::memory_order_relaxed) == 1 && !failNextAllocation.load(std::memory_order_relaxed), "cleanup did not consume exactly one injected allocation failure");
    require(DriverStoreEvidenceLost(), "cleanup failure did not latch terminal evidence loss");
    require(originalWatch->State() == DriverStoredSince::Unknown && aliasWatch->State() == DriverStoredSince::Unknown, "existing alias watches retained exact evidence after cleanup failed");
    const auto laterWatch = WatchDriverStores(alias + 128, 4);
    require(laterWatch->State() == DriverStoredSince::Unknown, "a newly captured watch recovered after terminal evidence loss");
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> ranges{{{address, address + initial.size()}, {alias, alias + initial.size()}}};
    require(DriverStoresSinceSequence(sequence, ranges) == DriverStoredSince::Unknown, "the sequence query retained exact evidence after cleanup failed");
    for (unsigned repeat = 0; repeat != 2; ++repeat) {
        expectRefusal([] { static_cast<void>(DriverStoreSequence()); });
        expectRefusal([] { static_cast<void>(DirtySerial()); });
        expectRefusal([] { static_cast<void>(UnwatchSerial()); });
        expectRefusal([] { static_cast<void>(TrackerGeneration()); });
        expectRefusal([] { static_cast<void>(CollectEpoch()); });
        expectRefusal([] { static_cast<void>(ThreadCollectEpoch()); });
    }
    expectRefusal([&] { static_cast<void>(NewestStamp(address, initial.size())); });
    expectRefusal([&] { static_cast<void>(CollectWrites(address, initial.size())); });
    expectRefusal([&] { static_cast<void>(CollectWritesUncached(alias, initial.size())); });
    expectRefusal([&] { static_cast<void>(UntrackedHint(alias, initial.size())); });
    expectRefusal([&] { static_cast<void>(UnchangedSince(address, initial.size(), generation)); });
    expectRefusal([&] { static_cast<void>(UnchangedSinceCollected(alias, initial.size(), generation)); });
    expectRefusal([&] { static_cast<void>(UnchangedSinceDirtySerial(address, initial.size(), dirty)); });
    expectRefusal([&] { static_cast<void>(StoresSince(address, initial.size(), generation)); });
    expectRefusal([&] { static_cast<void>(StoredOver(alias, initial.size(), generation)); });
    expectRefusal([&] { static_cast<void>(ProvenStoredOver(address, initial.size(), generation)); });
    std::vector<std::pair<std::uint64_t, std::uint64_t>> stores;
    expectRefusal([&] { static_cast<void>(DriverStoresSince(address, initial.size(), generation, stores)); });
    const std::array<std::uint64_t, 1> generations{generation};
    std::array<std::uint8_t, 1> blocks{}, cpu{};
    expectRefusal([&] { static_cast<void>(ChangedBlocks(address, initial.size(), generations, blocks, cpu)); });
    require(blocks[0] == 0 && cpu[0] == 0, "terminal refusal fabricated CPU write evidence");
    expectRefusal([&] { static_cast<void>(EqualsCommittedUnsynced(address, initial)); });
    expectRefusal([&] { static_cast<void>(CompareMapped(alias, initial)); });
    std::array<std::byte, 32> copied{};
    expectRefusal([&] { static_cast<void>(CopyMapped(alias, copied)); });
    GuestArena::GuestArenaCopyDriverMemory_nid_postfix(copied.data(), reinterpret_cast<void*>(alias), copied.size());
    for (std::size_t index = 0; index < copied.size(); ++index) require(copied[index] == (index < changed.size() ? std::byte{0x72} : std::byte{0x31}), "the fixture did not preserve the actual partial backing copy");
}
#endif
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "dirty-log") {
            checkDirtyLogLiveness();
            std::puts("dirty-log allocation failure: lock release, conservative serial gap and second caller passed");
            return 0;
        }
#ifdef _WIN32
        if (!GuestArena::GuestArenaAvailable_nid_postfix() || !WriteWatched()) return 77;
        SetEpochDirtySet(true);
        EnableDriverStoreLog();
        checkCleanupFailure();
        std::puts("terminal store evidence loss: original exception, aliases, repeated serial capture and proof refusal passed");
        return 0;
#else
        return 77;
#endif
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

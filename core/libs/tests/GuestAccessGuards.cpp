#include "prx/libc/include/GuestArena.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <semaphore>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace GuestArena;
using namespace std::chrono_literals;

static constexpr std::size_t pageBytes = 0x4000;
static constexpr std::size_t subpageBytes = 0x1000;

static void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Guest access guard check failed at %s:%u\n", location.file_name(), static_cast<unsigned>(location.line()));
        std::abort();
    }
}

template<class Action>
static void RequireRefusal(Action action) {
    bool refused = false;
    try {
        action();
    } catch (const std::runtime_error&) {
        refused = true;
    }
    Require(refused);
}

struct ResolverContext {
    std::function<bool(std::uint64_t)> resolve;
    std::function<void()> release;

    static bool Resolve(std::uint64_t token, void* context) {
        return static_cast<ResolverContext*>(context)->resolve(token);
    }

    static void Release(void* context) {
        std::unique_ptr<ResolverContext> owned(static_cast<ResolverContext*>(context));
        if (owned->release) owned->release();
    }
};

class SharedFixture {
public:
    SharedFixture() {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, pageBytes, nullptr);
        Require(section != nullptr);
        base = static_cast<unsigned char*>(GuestArenaAllocate_nid_postfix(pageBytes * 3, 0x10000));
        Require(base != nullptr);
        Map(0);
        Map(1);
        alias = static_cast<unsigned char*>(GuestArenaMapAlias_nid_postfix(Address(0), pageBytes));
        Require(alias != nullptr);
        std::memset(alias, 0x11, pageBytes);
    }

    ~SharedFixture() {
        for (const auto token : tokens) GuestArenaReleaseAccess_nid_postfix(token);
        GuestArenaUnmapAlias_nid_postfix(alias);
        GuestArenaReset_nid_postfix(base, pageBytes * 3);
        GuestArenaRelease_nid_postfix(base, pageBytes * 3);
        GuestArenaForgetSection_nid_postfix(section);
        if (duplicate != nullptr) {
            GuestArenaForgetSection_nid_postfix(duplicate);
            CloseHandle(duplicate);
        }
        CloseHandle(section);
    }

    SharedFixture(const SharedFixture&) = delete;
    SharedFixture& operator=(const SharedFixture&) = delete;

    std::uintptr_t Address(std::size_t view, std::size_t offset = 0) const {
        return reinterpret_cast<std::uintptr_t>(base + view * pageBytes + offset);
    }

    unsigned char* Pointer(std::size_t view, std::size_t offset = 0) const {
        return base + view * pageBytes + offset;
    }

    void Map(std::size_t view, DWORD protection = PAGE_READWRITE, bool duplicated = false) {
        if (duplicated && duplicate == nullptr) Require(DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) != 0);
        GuestArenaMap_nid_postfix(Pointer(view), pageBytes, duplicated ? duplicate : section, 0, protection);
    }

    std::uint64_t Protect(std::size_t view, std::size_t offset, std::size_t bytes, std::function<bool(std::uint64_t)> resolve, std::function<void()> release = {}) {
        auto context = std::make_unique<ResolverContext>(ResolverContext{std::move(resolve), std::move(release)});
        const auto token = GuestArenaTryProtectAccess_nid_postfix(Address(view, offset), bytes, ResolverContext::Resolve, context.get(), ResolverContext::Release);
        if (token != 0) {
            context.release();
            tokens.push_back(token);
        }
        return token;
    }

    std::vector<std::size_t> Collect(std::size_t view) const {
        std::array<void*, pageBytes / subpageBytes> pages{};
        std::size_t count = pages.size();
        Require(GuestArenaCollectWrites_nid_postfix(Address(view), pageBytes, pages.data(), &count, true));
        std::vector<std::size_t> offsets;
        for (std::size_t index = 0; index < count; ++index) offsets.push_back(reinterpret_cast<std::uintptr_t>(pages[index]) - Address(view));
        return offsets;
    }

    DWORD Protection(std::size_t view, std::size_t offset) const {
        MEMORY_BASIC_INFORMATION memory{};
        Require(VirtualQuery(Pointer(view, offset), &memory, sizeof(memory)) == sizeof(memory));
        return memory.Protect;
    }

    DWORD State(std::size_t view) const {
        MEMORY_BASIC_INFORMATION memory{};
        Require(VirtualQuery(Pointer(view), &memory, sizeof(memory)) == sizeof(memory));
        return memory.State;
    }

    void Fill(std::size_t offset, unsigned char value) const {
        std::memset(alias + offset, value, subpageBytes);
    }

private:
    HANDLE section = nullptr;
    HANDLE duplicate = nullptr;
    unsigned char* base = nullptr;
    unsigned char* alias = nullptr;
    std::vector<std::uint64_t> tokens;
};

static bool ReleaseResolved(std::uint64_t token) {
    GuestArenaReleaseAccess_nid_postfix(token);
    return true;
}

static void CheckWriteTrackingAndAliases() {
    SharedFixture fixture;
    fixture.Collect(0);
    fixture.Collect(1);
    Require(fixture.Collect(0).empty() && fixture.Collect(1).empty());
    const auto token = fixture.Protect(0, subpageBytes, subpageBytes, ReleaseResolved);
    Require(token != 0);
    for (const auto view : {0u, 1u}) {
        Require(fixture.Protection(view, subpageBytes) == PAGE_NOACCESS);
        Require(fixture.Protection(view, 0) == PAGE_READONLY);
    }
    fixture.Collect(0);
    fixture.Collect(1);
    RequireRefusal([&] { GuestArenaPinWritable_nid_postfix(fixture.Pointer(0), subpageBytes); });
    RequireRefusal([&] { GuestArenaSetProtection_nid_postfix(fixture.Address(0), subpageBytes, PAGE_READWRITE); });
    RequireRefusal([&] { GuestArenaCommit_nid_postfix(fixture.Pointer(0), subpageBytes, PAGE_READWRITE, pageBytes); });
    RequireRefusal([&] { GuestArenaReset_nid_postfix(fixture.Pointer(0), pageBytes); });
    RequireRefusal([&] { GuestArenaBeginHostWrite_nid_postfix(fixture.Pointer(1, subpageBytes), 1); });
    Require(fixture.Protection(0, subpageBytes) == PAGE_NOACCESS && fixture.Protection(1, subpageBytes) == PAGE_NOACCESS);
    Require(GuestArenaBeginHostWrite_nid_postfix(fixture.Pointer(0), 1));
    *static_cast<volatile unsigned char*>(fixture.Pointer(0)) = 0x21;
    GuestArenaEndHostWrite_nid_postfix(fixture.Pointer(0), 1);
    Require(fixture.Protection(0, subpageBytes) == PAGE_NOACCESS && fixture.Protection(1, subpageBytes) == PAGE_NOACCESS);
    fixture.Map(2);
    Require(fixture.Protection(2, subpageBytes) == PAGE_NOACCESS && fixture.Protection(2, 0) == PAGE_READWRITE);
    GuestArenaReleaseAccess_nid_postfix(token);
    Require(fixture.Protection(0, subpageBytes) == PAGE_READONLY && fixture.Protection(1, subpageBytes) == PAGE_READONLY);
    fixture.Collect(0);
    fixture.Collect(1);
    fixture.Collect(2);
    *static_cast<volatile unsigned char*>(fixture.Pointer(1, subpageBytes + 7)) = 0x42;
    Require(*static_cast<volatile unsigned char*>(fixture.Pointer(0, subpageBytes + 7)) == 0x42);
    const std::vector<std::size_t> changed{subpageBytes};
    Require(fixture.Collect(0) == changed && fixture.Collect(1) == changed && fixture.Collect(2) == changed);
    Require(fixture.Collect(0).empty());
}

static void CheckFaultPublication() {
    std::atomic<unsigned> calls{0};
    SharedFixture fixture;
    Require(fixture.Protect(0, subpageBytes, subpageBytes, [&](std::uint64_t token) {
        ++calls;
        fixture.Fill(subpageBytes, 0xa1);
        return ReleaseResolved(token);
    }) != 0);
    Require(*static_cast<volatile unsigned char*>(fixture.Pointer(1, subpageBytes + 9)) == 0xa1);
    Require(calls == 1 && fixture.Protection(0, subpageBytes) != PAGE_NOACCESS);
    Require(fixture.Protect(1, subpageBytes, subpageBytes, [&](std::uint64_t token) {
        ++calls;
        fixture.Fill(subpageBytes, 0xb2);
        return ReleaseResolved(token);
    }) != 0);
    *static_cast<volatile unsigned char*>(fixture.Pointer(0, subpageBytes + 9)) = 0xc3;
    Require(calls == 2);
    Require(*static_cast<volatile unsigned char*>(fixture.Pointer(1, subpageBytes + 9)) == 0xc3);
    Require(*static_cast<volatile unsigned char*>(fixture.Pointer(1, subpageBytes + 10)) == 0xb2);
}

static void CheckPrivateDriverAccess() {
    SharedFixture fixture;
    fixture.Collect(0);
    fixture.Collect(1);
    unsigned resolved = 0;
    const auto token = fixture.Protect(0, subpageBytes, subpageBytes * 2, [&](std::uint64_t value) {
        ++resolved;
        return ReleaseResolved(value);
    });
    Require(token != 0);
    std::array<unsigned char, 16> copied{};
    GuestArenaCopyDriverMemory_nid_postfix(copied.data(), fixture.Pointer(1, subpageBytes), copied.size());
    Require(std::all_of(copied.begin(), copied.end(), [](unsigned char value) { return value == 0x11; }));
    Require(GuestArenaCompareDriverMemory_nid_postfix(copied.data(), fixture.Pointer(0, subpageBytes), copied.size()));
    Require(fixture.Protection(0, subpageBytes * 2) == PAGE_NOACCESS && fixture.Protection(1, subpageBytes * 2) == PAGE_NOACCESS && resolved == 0);
    std::array<unsigned char, 257> baseline, current;
    baseline.fill(0x11);
    current = baseline;
    for (std::size_t at = 1; at < current.size(); at += 2) current[at] = 0x44;
    std::size_t firstChanged = 0, lastChanged = 0;
    GuestArenaCopyChangedDriverMemory_nid_postfix(fixture.Pointer(1, subpageBytes), current.data(), baseline.data(), current.size(), &firstChanged, &lastChanged);
    Require(firstChanged == 1 && lastChanged == 256);
    Require(GuestArenaCompareDriverMemory_nid_postfix(fixture.Pointer(0, subpageBytes), current.data(), current.size()));
    const std::uint64_t value = 0x8877665544332211ull;
    GuestArenaStoreDriverAtomic64_nid_postfix(fixture.Address(1, subpageBytes + 8), value);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(0, subpageBytes + 8), 8) == value);
    Require(fixture.Protection(0, subpageBytes) == PAGE_NOACCESS && fixture.Protection(1, subpageBytes) == PAGE_NOACCESS && resolved == 0);
    const std::vector<std::size_t> changed{subpageBytes};
    Require(fixture.Collect(0) == changed && fixture.Collect(1) == changed);
    fixture.Map(2);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(2, subpageBytes + 8), 8) == value);
    Require(fixture.Protection(2, subpageBytes) == PAGE_NOACCESS && resolved == 0);
    GuestArenaReleaseAccess_nid_postfix(token);
    Require(*reinterpret_cast<const volatile std::uint64_t*>(fixture.Pointer(0, subpageBytes + 8)) == value);
}

static void CheckPrivateDriverAccessAfterRemap() {
    SharedFixture fixture;
    std::array<unsigned char, 8> copied{};
    GuestArenaCopyDriverMemory_nid_postfix(copied.data(), fixture.Pointer(1), copied.size());
    const std::uint64_t original = 0x2222222222222222ull;
    GuestArenaStoreDriverAtomic64_nid_postfix(fixture.Address(1), original);
    GuestArenaReset_nid_postfix(fixture.Pointer(1), pageBytes);
    HANDLE replacement = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, pageBytes, nullptr);
    Require(replacement != nullptr);
    GuestArenaMap_nid_postfix(fixture.Pointer(1), pageBytes, replacement, 0, PAGE_READWRITE);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(1), 8) == 0);
    const std::uint64_t current = 0x3333333333333333ull;
    GuestArenaStoreDriverAtomic64_nid_postfix(fixture.Address(1), current);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(0), 8) == original);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(1), 8) == current);
    GuestArenaReset_nid_postfix(fixture.Pointer(1), pageBytes);
    GuestArenaForgetSection_nid_postfix(replacement);
    CloseHandle(replacement);
    fixture.Map(1);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(1), 8) == original);
}

static void CheckPrivateDriverProtection() {
    SharedFixture fixture;
    std::array<unsigned char, 8> copied{};
    GuestArenaSetProtection_nid_postfix(fixture.Address(1), pageBytes, PAGE_NOACCESS);
    RequireRefusal([&] { GuestArenaCopyDriverMemory_nid_postfix(copied.data(), fixture.Pointer(1), copied.size()); });
    GuestArenaSetProtection_nid_postfix(fixture.Address(1), pageBytes, PAGE_READWRITE | PAGE_GUARD);
    RequireRefusal([&] { GuestArenaCopyDriverMemory_nid_postfix(copied.data(), fixture.Pointer(1), copied.size()); });
    RequireRefusal([&] { GuestArenaStoreDriverAtomic64_nid_postfix(fixture.Address(1), 1); });
    GuestArenaSetProtection_nid_postfix(fixture.Address(1), pageBytes, PAGE_WRITECOPY);
    DWORD previous = 0;
    Require(VirtualProtect(fixture.Pointer(1), pageBytes, PAGE_WRITECOPY, &previous) != 0);
    Require(fixture.Protection(1, 0) == PAGE_WRITECOPY);
    const std::uint64_t privateValue = 0x6666666666666666ull;
    GuestArenaCopyDriverMemory_nid_postfix(fixture.Pointer(1), &privateValue, sizeof(privateValue));
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(1), 8) == privateValue);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(0), 4) == 0x11111111u);
    *static_cast<volatile unsigned char*>(fixture.Pointer(1)) = 0x77;
    GuestArenaCopyDriverMemory_nid_postfix(copied.data(), fixture.Pointer(1), copied.size());
    Require(copied.front() == 0x77);
    GuestArenaSetProtection_nid_postfix(fixture.Address(1), pageBytes, PAGE_READWRITE);
    Require(VirtualProtect(fixture.Pointer(1), pageBytes, PAGE_READWRITE, &previous) != 0);
    Require(fixture.Protection(1, 0) == PAGE_READWRITE);
    fixture.Collect(0);
    fixture.Collect(1);
    Require(fixture.Protect(0, 0, subpageBytes, ReleaseResolved) == 0);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(1), 4) == 0x66666677u);
    GuestArenaStoreDriverAtomic64_nid_postfix(fixture.Address(1), 0x8888888888888888ull);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(1), 8) == 0x8888888888888888ull);
    Require(GuestArenaLoadDriverAtomic_nid_postfix(fixture.Address(0), 4) == 0x11111111u);
}

static std::uint64_t BackingOf(std::uintptr_t address, std::size_t bytes) {
    std::array<std::uintptr_t, 1> begins{}, ends{};
    std::array<std::uint64_t, 1> canonicals{};
    std::size_t count = 0;
    Require(GuestArenaTryBackingRuns_nid_postfix(address, bytes, begins.data(), ends.data(), canonicals.data(), begins.size(), &count));
    Require(count == 1 && begins[0] == address && ends[0] == address + bytes);
    return canonicals[0];
}

static void CheckBackingProofs() {
    SharedFixture fixture;
    const auto original = BackingOf(fixture.Address(0), pageBytes);
    Require(BackingOf(fixture.Address(1, 17), 31) == original + 17);
    fixture.Map(2, PAGE_READWRITE, true);
    Require(BackingOf(fixture.Address(2), pageBytes) == original);
    std::array<std::uintptr_t, 1> begins{}, ends{};
    std::array<std::uint64_t, 1> canonicals{};
    std::size_t count = 0;
    Require(GuestArenaTryBackingRuns_nid_postfix(fixture.Address(0), pageBytes * 3, begins.data(), ends.data(), canonicals.data(), begins.size(), &count) && count == 3);
    GuestArenaReset_nid_postfix(fixture.Pointer(0), pageBytes * 3);
    fixture.Map(0);
    fixture.Map(1);
    Require(BackingOf(fixture.Address(0), pageBytes) == original && BackingOf(fixture.Address(1), pageBytes) == original);
    std::array<unsigned char, 16> privateMemory{};
    count = 99;
    Require(!GuestArenaTryBackingRuns_nid_postfix(reinterpret_cast<std::uintptr_t>(privateMemory.data()), privateMemory.size(), begins.data(), ends.data(), canonicals.data(), begins.size(), &count) && count == 0);
    GuestArenaSetProtection_nid_postfix(fixture.Address(1), pageBytes, PAGE_WRITECOPY);
    Require(!GuestArenaTryBackingRuns_nid_postfix(fixture.Address(1), 1, begins.data(), ends.data(), canonicals.data(), begins.size(), &count) && count == 0);
    GuestArenaSetProtection_nid_postfix(fixture.Address(1), pageBytes, PAGE_READWRITE);
    Require(!GuestArenaTryBackingRuns_nid_postfix(fixture.Address(1), 1, begins.data(), ends.data(), canonicals.data(), begins.size(), &count));
    GuestArenaReset_nid_postfix(fixture.Pointer(1), pageBytes);
    fixture.Map(1);
    Require(BackingOf(fixture.Address(1), pageBytes) == original);
    HANDLE replacement = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, pageBytes * 2, nullptr);
    Require(replacement != nullptr);
    GuestArenaReset_nid_postfix(fixture.Pointer(0), pageBytes * 3);
    GuestArenaMap_nid_postfix(fixture.Pointer(1), pageBytes * 2, replacement, 0, PAGE_READWRITE);
    const auto next = BackingOf(fixture.Address(1), pageBytes * 2);
    Require(next != original);
    GuestArenaMap_nid_postfix(fixture.Pointer(0), pageBytes, replacement, pageBytes, PAGE_READWRITE);
    Require(BackingOf(fixture.Address(0), pageBytes) == next + pageBytes);
    GuestArenaForgetSection_nid_postfix(replacement);
    Require(BackingOf(fixture.Address(0), pageBytes) == next + pageBytes);
    GuestArenaReset_nid_postfix(fixture.Pointer(0), pageBytes * 3);
    GuestArenaMapDirect_nid_postfix(fixture.Pointer(2), pageBytes, replacement, 0, PAGE_READWRITE);
    const auto resident = BackingOf(fixture.Address(2), pageBytes);
    void* alias = GuestArenaMapAlias_nid_postfix(fixture.Address(2), pageBytes);
    Require(alias != nullptr && BackingOf(fixture.Address(2), pageBytes) == resident);
    GuestArenaUnmapAlias_nid_postfix(alias);
    GuestArenaReset_nid_postfix(fixture.Pointer(2), pageBytes);
    GuestArenaForgetSection_nid_postfix(replacement);
    CloseHandle(replacement);
    fixture.Map(0);
    fixture.Map(1);
    Require(BackingOf(fixture.Address(0), pageBytes) == original);
}

static void CheckPartialOffsetBacking() {
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, pageBytes * 2, nullptr);
    Require(section != nullptr);
    auto* reserved = static_cast<unsigned char*>(GuestArenaAllocate_nid_postfix(pageBytes * 2, 0x10000));
    Require(reserved != nullptr);
    const auto address = reinterpret_cast<std::uintptr_t>(reserved);
    GuestArenaMap_nid_postfix(reserved, pageBytes, section, 0, PAGE_READWRITE);
    const auto backing = BackingOf(address, pageBytes);
    const std::uint64_t value = 0x5566778899aabbccull;
    GuestArenaCopyDriverMemory_nid_postfix(reserved + subpageBytes, &value, sizeof(value));
    const auto token = GuestArenaTryProtectAccess_nid_postfix(address, subpageBytes, [](std::uint64_t token, void*) { GuestArenaReleaseAccess_nid_postfix(token); return true; }, nullptr, nullptr);
    Require(token != 0);
    RequireRefusal([&] { GuestArenaMap_nid_postfix(reserved + pageBytes, pageBytes, section, subpageBytes, PAGE_READWRITE); });
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(reserved + pageBytes, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_RESERVE);
    Require(VirtualQuery(reserved, &memory, sizeof(memory)) == sizeof(memory) && memory.Protect == PAGE_NOACCESS);
    Require(BackingOf(address, pageBytes) == backing);
    GuestArenaReleaseAccess_nid_postfix(token);
    RequireRefusal([&] { GuestArenaMap_nid_postfix(reserved + pageBytes, pageBytes, section, subpageBytes, PAGE_READWRITE); });
    Require(BackingOf(address, pageBytes) == backing && GuestArenaLoadDriverAtomic_nid_postfix(address + subpageBytes, 8) == value);
    Require(VirtualQuery(reserved + pageBytes, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_RESERVE);
    GuestArenaMap_nid_postfix(reserved + pageBytes, pageBytes, section, pageBytes, PAGE_READWRITE);
    Require(BackingOf(address + pageBytes, pageBytes) != backing && GuestArenaLoadDriverAtomic_nid_postfix(address + pageBytes, 8) == 0);
    GuestArenaReset_nid_postfix(reserved, pageBytes * 2);
    GuestArenaRelease_nid_postfix(reserved, pageBytes * 2);
    GuestArenaForgetSection_nid_postfix(section);
    CloseHandle(section);
}

static void CheckSectionHandleReuse() {
    HANDLE original = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, pageBytes, nullptr);
    Require(original != nullptr);
    auto* memory = static_cast<unsigned char*>(GuestArenaAllocate_nid_postfix(pageBytes * 2, 0x10000));
    Require(memory != nullptr);
    GuestArenaMap_nid_postfix(memory, pageBytes, original, 0, PAGE_READWRITE);
    const std::uint64_t oldValue = 0x5555555555555555ull;
    GuestArenaCopyDriverMemory_nid_postfix(memory, &oldValue, sizeof(oldValue));
    const auto oldBacking = BackingOf(reinterpret_cast<std::uintptr_t>(memory), pageBytes);
    CloseHandle(original);
    unsigned reused = 0;
    constexpr unsigned attempts = 32;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        HANDLE current = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, pageBytes, nullptr);
        Require(current != nullptr);
        reused += current == original ? 1u : 0u;
        GuestArenaMap_nid_postfix(memory + pageBytes, pageBytes, current, 0, PAGE_READWRITE);
        Require(GuestArenaLoadDriverAtomic_nid_postfix(reinterpret_cast<std::uintptr_t>(memory + pageBytes), 8) == 0);
        const std::uint64_t newValue = 0x7777777700000000ull + attempt;
        GuestArenaCopyDriverMemory_nid_postfix(memory + pageBytes, &newValue, sizeof(newValue));
        Require(GuestArenaLoadDriverAtomic_nid_postfix(reinterpret_cast<std::uintptr_t>(memory), 8) == oldValue);
        Require(GuestArenaLoadDriverAtomic_nid_postfix(reinterpret_cast<std::uintptr_t>(memory + pageBytes), 8) == newValue);
        Require(BackingOf(reinterpret_cast<std::uintptr_t>(memory), pageBytes) == oldBacking);
        Require(BackingOf(reinterpret_cast<std::uintptr_t>(memory + pageBytes), pageBytes) != oldBacking);
        GuestArenaReset_nid_postfix(memory + pageBytes, pageBytes);
        GuestArenaForgetSection_nid_postfix(current);
        CloseHandle(current);
    }
    GuestArenaReset_nid_postfix(memory, pageBytes * 2);
    GuestArenaRelease_nid_postfix(memory, pageBytes * 2);
    GuestArenaForgetSection_nid_postfix(original);
    std::printf("Section handle reuse: %u actual reuses in %u attempts%s\n", reused, attempts, reused == 0 ? " (reuse branch not covered)" : "");
}

static void CheckCollectedWriteProvenance() {
    SharedFixture fixture;
    fixture.Map(2, PAGE_READWRITE, true);
    const auto collect = [&](std::size_t view) {
        std::array<void*, pageBytes / subpageBytes> pages{};
        std::array<std::uint8_t, pageBytes / subpageBytes> sources{};
        std::size_t count = pages.size();
        Require(GuestArenaCollectDriverWrites_nid_postfix(fixture.Address(view), pageBytes, pages.data(), sources.data(), &count, true));
        std::vector<std::pair<std::size_t, std::uint8_t>> result;
        for (std::size_t index = 0; index < count; ++index) result.emplace_back(reinterpret_cast<std::uintptr_t>(pages[index]) - fixture.Address(view), sources[index]);
        return result;
    };
    fixture.Collect(0);
    fixture.Collect(1);
    fixture.Collect(2);
    const std::uint64_t value = 0x4444444444444444ull;
    GuestArenaCopyDriverMemory_nid_postfix(fixture.Pointer(1, 8), &value, sizeof(value));
    Require(collect(0) == std::vector<std::pair<std::size_t, std::uint8_t>>{{0, 0}});
    fixture.Collect(1);
    fixture.Collect(2);
    std::thread native([&] { *static_cast<volatile unsigned char*>(fixture.Pointer(2, 128)) = 0x11; });
    native.join();
    GuestArenaCopyDriverMemory_nid_postfix(fixture.Pointer(1, subpageBytes + 8), &value, sizeof(value));
    Require(collect(0) == std::vector<std::pair<std::size_t, std::uint8_t>>{{0, 1}, {subpageBytes, 0}});
    fixture.Collect(1);
    fixture.Collect(2);
    GuestArenaCopyDriverMemory_nid_postfix(fixture.Pointer(1, 8), &value, sizeof(value));
    Require(collect(0) == std::vector<std::pair<std::size_t, std::uint8_t>>{{0, 0}});
    fixture.Collect(1);
    fixture.Collect(2);
    *static_cast<volatile unsigned char*>(fixture.Pointer(2, 128)) = 0x11;
    Require(collect(0) == std::vector<std::pair<std::size_t, std::uint8_t>>{{0, 2}});
    fixture.Collect(1);
    fixture.Collect(2);
    *static_cast<volatile unsigned char*>(fixture.Pointer(2, 128)) = 0x11;
    fixture.Collect(2);
    std::thread mixed([&] { *static_cast<volatile unsigned char*>(fixture.Pointer(2, 128)) = 0x11; });
    mixed.join();
    Require(collect(0) == std::vector<std::pair<std::size_t, std::uint8_t>>{{0, 2}});
    fixture.Collect(1);
    fixture.Collect(2);
    GuestArenaSetProtection_nid_postfix(fixture.Address(2), pageBytes, PAGE_WRITECOPY);
    GuestArenaCopyDriverMemory_nid_postfix(fixture.Pointer(1, 8), &value, sizeof(value));
    const auto cow = collect(0);
    Require(!cow.empty() && std::all_of(cow.begin(), cow.end(), [](const auto& item) { return item.second == 2; }));
}

static void CheckReplacementAndRefusals() {
    SharedFixture fixture;
    unsigned calls = 0;
    const auto first = fixture.Protect(0, 0, subpageBytes, ReleaseResolved);
    const auto second = fixture.Protect(1, 0, subpageBytes, [&](std::uint64_t token) {
        ++calls;
        Require(fixture.Protect(0, 0, subpageBytes, [&](std::uint64_t replacement) {
            ++calls;
            fixture.Fill(0, 0x6a);
            return ReleaseResolved(replacement);
        }) != 0);
        return ReleaseResolved(token);
    });
    Require(first != 0 && second != 0);
    GuestArenaReleaseAccess_nid_postfix(first);
    Require(fixture.Protection(0, 0) == PAGE_NOACCESS && fixture.Protection(1, 0) == PAGE_NOACCESS);
    Require(*static_cast<volatile unsigned char*>(fixture.Pointer(0)) == 0x6a && calls == 2);
    for (const auto mode : {0u, 1u, 2u}) {
        const auto token = fixture.Protect(0, 0, subpageBytes, [mode](std::uint64_t) -> bool {
            if (mode == 1) throw std::runtime_error("publication failed");
            return mode == 2;
        });
        Require(token != 0);
        RequireRefusal([&] { GuestArenaResolveAccess_nid_postfix(fixture.Address(1), 1); });
        Require(fixture.Protection(0, 0) == PAGE_NOACCESS && fixture.Protection(1, 0) == PAGE_NOACCESS);
        GuestArenaReleaseAccess_nid_postfix(token);
    }
}

static void CheckResolverLifetime() {
    std::binary_semaphore entered(0);
    std::binary_semaphore resume(0);
    std::binary_semaphore releaseFinished(0);
    std::binary_semaphore resolveFinished(0);
    std::atomic<unsigned> destroyed{0};
    std::atomic<unsigned> calls{0};
    std::atomic<bool> resolved{false};
    SharedFixture fixture;
    const auto token = fixture.Protect(0, 0, subpageBytes, [&](std::uint64_t) {
        ++calls;
        entered.release();
        Require(resume.try_acquire_for(4s));
        Require(destroyed == 0);
        return true;
    }, [&] { ++destroyed; });
    Require(token != 0);
    std::thread resolver([&] {
        resolved = GuestArenaResolveAccess_nid_postfix(fixture.Address(0), 1);
        resolveFinished.release();
    });
    Require(entered.try_acquire_for(2s));
    std::thread releaser([&] {
        GuestArenaReleaseAccess_nid_postfix(token);
        releaseFinished.release();
    });
    Require(releaseFinished.try_acquire_for(2s));
    releaser.join();
    Require(destroyed == 0);
    Require(GuestArenaResolveAccess_nid_postfix(fixture.Address(1), 1) && calls == 1);
    resume.release();
    Require(resolveFinished.try_acquire_for(2s));
    resolver.join();
    Require(resolved && destroyed == 1 && calls == 1);
}

static void CheckNestedResolutionRefusal() {
    std::barrier rendezvous(2);
    std::counting_semaphore<2> finished(0);
    std::atomic<unsigned> refused{0};
    std::atomic<unsigned> completed{0};
    SharedFixture fixture;
    for (const auto offset : {std::size_t{0}, subpageBytes}) {
        Require(fixture.Protect(0, offset, subpageBytes, [&, offset](std::uint64_t token) {
            rendezvous.arrive_and_wait();
            RequireRefusal([&] { GuestArenaResolveAccess_nid_postfix(fixture.Address(0, subpageBytes - offset), 1); });
            ++refused;
            return ReleaseResolved(token);
        }) != 0);
    }
    std::thread first([&] {
        if (GuestArenaResolveAccess_nid_postfix(fixture.Address(0), 1)) ++completed;
        finished.release();
    });
    std::thread second([&] {
        if (GuestArenaResolveAccess_nid_postfix(fixture.Address(1, subpageBytes), 1)) ++completed;
        finished.release();
    });
    Require(finished.try_acquire_for(2s) && finished.try_acquire_for(2s));
    first.join();
    second.join();
    Require(refused == 2 && completed == 2);
}

static void CheckMappingIdentityAndEligibility() {
    SharedFixture fixture;
    const auto token = fixture.Protect(0, 0, subpageBytes, ReleaseResolved);
    Require(token != 0 && fixture.State(2) == MEM_RESERVE);
    RequireRefusal([&] { fixture.Map(2, PAGE_EXECUTE_READWRITE); });
    RequireRefusal([&] { fixture.Map(2, PAGE_WRITECOPY); });
    Require(fixture.State(2) == MEM_RESERVE && fixture.Protection(0, 0) == PAGE_NOACCESS && fixture.Protection(1, 0) == PAGE_NOACCESS);
    fixture.Map(2, PAGE_READWRITE, true);
    Require(fixture.Protection(2, 0) == PAGE_NOACCESS);
    GuestArenaReleaseAccess_nid_postfix(token);
    const auto duplicated = fixture.Protect(0, 0, subpageBytes, ReleaseResolved);
    Require(duplicated != 0 && fixture.Protection(2, 0) == PAGE_NOACCESS);
    GuestArenaReleaseAccess_nid_postfix(duplicated);
    std::array<std::uintptr_t, 3> aliases{}, aliasEnds{};
    const auto aliasCount = GuestArenaAliasRanges_nid_postfix(fixture.Address(0), subpageBytes, aliases.data(), aliasEnds.data(), aliases.size());
    Require(aliasCount == 2 && std::find(aliases.begin(), aliases.begin() + aliasCount, fixture.Address(2)) != aliases.begin() + aliasCount);
    GuestArenaReset_nid_postfix(fixture.Pointer(2), pageBytes);
    Require(fixture.Protect(0, 1, subpageBytes, ReleaseResolved) == 0);
    Require(fixture.Protect(0, 0, subpageBytes - 1, ReleaseResolved) == 0);
    GuestArenaPinWritable_nid_postfix(fixture.Pointer(0), subpageBytes);
    Require(fixture.Protect(1, 0, subpageBytes, ReleaseResolved) == 0);
    GuestArenaUnpinWritable_nid_postfix(fixture.Pointer(0), subpageBytes);
    Require(GuestArenaBeginHostWrite_nid_postfix(fixture.Pointer(1), 1));
    Require(fixture.Protect(0, 0, subpageBytes, ReleaseResolved) == 0);
    GuestArenaEndHostWrite_nid_postfix(fixture.Pointer(1), 1);
    GuestArenaSetProtection_nid_postfix(fixture.Address(1), pageBytes, PAGE_WRITECOPY);
    Require(fixture.Protect(0, 0, subpageBytes, ReleaseResolved) == 0);
    GuestArenaSetProtection_nid_postfix(fixture.Address(1), pageBytes, PAGE_READWRITE);
    Require(fixture.Protect(0, 0, subpageBytes, ReleaseResolved) == 0);
    GuestArenaReset_nid_postfix(fixture.Pointer(1), pageBytes);
    fixture.Map(1);
    std::atomic<unsigned> released{0};
    Require(fixture.Protect(0, 1, subpageBytes, ReleaseResolved, [&] { ++released; }) == 0);
    Require(released == 0);
    const auto owned = fixture.Protect(0, 0, subpageBytes, ReleaseResolved, [&] { ++released; });
    Require(owned != 0);
    GuestArenaReleaseAccess_nid_postfix(owned);
    Require(released == 1);
}

static void CheckMappingAddedDuringResolution() {
    SharedFixture fixture;
    std::uint64_t added = 0;
    Require(fixture.Protect(0, 0, subpageBytes, [&](std::uint64_t token) {
        fixture.Map(2);
        added = fixture.Protect(2, 0, subpageBytes, ReleaseResolved);
        Require(added != 0);
        return ReleaseResolved(token);
    }) != 0);
    Require(!GuestArenaResolveAccess_nid_postfix(fixture.Address(0), pageBytes * 3));
    Require(added != 0 && fixture.Protection(2, 0) == PAGE_NOACCESS);
    Require(GuestArenaResolveAccess_nid_postfix(fixture.Address(2), 1));
    Require(fixture.Protection(2, 0) == PAGE_READWRITE);
}

int main() {
    if (GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "CompareObjectHandles") == nullptr) {
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, pageBytes, nullptr);
        Require(section != nullptr);
        auto* reserved = GuestArenaAllocate_nid_postfix(pageBytes, 0x10000);
        Require(reserved != nullptr);
        RequireRefusal([&] { GuestArenaMap_nid_postfix(reserved, pageBytes, section, 0, PAGE_READWRITE); });
        MEMORY_BASIC_INFORMATION memory{};
        Require(VirtualQuery(reserved, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_RESERVE);
        GuestArenaRelease_nid_postfix(reserved, pageBytes);
        CloseHandle(section);
        std::puts("Shared guest mappings declined without section identity support");
        return 0;
    }
    CheckWriteTrackingAndAliases();
    CheckFaultPublication();
    CheckPrivateDriverAccess();
    CheckPrivateDriverAccessAfterRemap();
    CheckPrivateDriverProtection();
    CheckBackingProofs();
    CheckPartialOffsetBacking();
    CheckSectionHandleReuse();
    CheckCollectedWriteProvenance();
    CheckReplacementAndRefusals();
    CheckResolverLifetime();
    CheckNestedResolutionRefusal();
    CheckMappingIdentityAndEligibility();
    CheckMappingAddedDuringResolution();
    std::puts("Guest access guard checks passed");
}

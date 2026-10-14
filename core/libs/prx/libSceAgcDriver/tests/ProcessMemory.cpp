#include "prx/libSceAgcDriver/Execution/include/ProcessMemory.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#endif

namespace {

using namespace AgcDriver::ProcessMemory;

constexpr std::uint64_t MiB = 1u << 20u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef _WIN32
double milliseconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

void checkArenaSkipped() {
    constexpr std::uint64_t arenaBytes = 256 * MiB;
    constexpr std::uint64_t pageBytes = 4096;
    constexpr std::uint64_t pages = 20000;
    auto* arena = static_cast<std::byte*>(VirtualAlloc(nullptr, arenaBytes, MEM_RESERVE, PAGE_NOACCESS));
    require(arena != nullptr, "reserve the test arena");
    for (std::uint64_t page = 0; page < pages; ++page) require(VirtualAlloc(arena + page * 2 * pageBytes, pageBytes, MEM_COMMIT, PAGE_READWRITE) != nullptr, "commit a page of the test arena");
    const auto base = reinterpret_cast<std::uintptr_t>(arena);
    const auto whole = WalkAddressSpace(0, 0, ThreadStackAllocations());
    const auto stacks = ThreadStackAllocations();
    AddressSpaceUse skipped;
    auto skippedMs = 1e9;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto walked = std::chrono::steady_clock::now();
        skipped = WalkAddressSpace(base, arenaBytes, stacks);
        skippedMs = std::min(skippedMs, milliseconds(walked));
    }
    const auto started = std::chrono::steady_clock::now();
    static_cast<void>(ReadProcessCounters());
    const auto countersMs = milliseconds(started);
    require(whole.regions >= skipped.regions + 2 * pages - 1, "the walk does not skip the arena's regions: " + std::to_string(whole.regions) + " regions with it, " + std::to_string(skipped.regions) + " without");
    require(whole.hostPrivate >= skipped.hostPrivate + pages * pageBytes, "the arena's commit is counted as host memory when the arena is skipped");
    require(skippedMs + countersMs < 50.0, "a report walk over " + std::to_string(skipped.regions) + " regions took " + std::to_string(skippedMs + countersMs) + " ms");
    std::printf("walk without the %llu-region arena: %llu regions in %.1f ms, process counters in %.2f ms\n", static_cast<unsigned long long>(whole.regions - skipped.regions), static_cast<unsigned long long>(skipped.regions), skippedMs, countersMs);
    VirtualFree(arena, 0, MEM_RELEASE);
}

void checkHostCommit() {
    const auto before = WalkAddressSpace(0, 0, ThreadStackAllocations());
    const auto counters = ReadProcessCounters();
    auto* block = static_cast<std::byte*>(VirtualAlloc(nullptr, 48 * MiB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(block != nullptr, "commit a host block");
    const auto after = WalkAddressSpace(0, 0, ThreadStackAllocations(), 8);
    const auto grown = ReadProcessCounters();
    require(after.hostPrivate >= before.hostPrivate + 48 * MiB, "a committed host block is counted as host private memory");
    require(std::ranges::any_of(after.largest, [&](const auto& entry) { return entry.first == reinterpret_cast<std::uintptr_t>(block) && entry.second == 48 * MiB; }), "the committed host block is among the largest host allocations");
    require(grown.commit >= counters.commit + 48 * MiB, "the process commit grows with the committed block");
    require(after.image != 0 && after.regions != 0, "the walk sees the executable image");
    VirtualFree(block, 0, MEM_RELEASE);
}

void checkGpuMapped() {
    const auto before = WalkAddressSpace(0, 0, ThreadStackAllocations());
    auto* combined = static_cast<std::byte*>(VirtualAlloc(nullptr, 32 * MiB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE));
    require(combined != nullptr, "commit a write-combined block");
    const auto after = WalkAddressSpace(0, 0, ThreadStackAllocations());
    require(after.gpuMapped >= before.gpuMapped + 32 * MiB && after.gpuMappedCount == before.gpuMappedCount + 1, "a write-combined allocation is not counted as GPU memory the driver mapped");
    require(std::ranges::binary_search(after.gpuMappedBases, reinterpret_cast<std::uintptr_t>(combined)), "the write-combined allocation's base is not listed");
    require(after.hostPrivate >= before.hostPrivate + 32 * MiB, "a write-combined allocation left host private memory");
    VirtualFree(combined, 0, MEM_RELEASE);
}

void checkDelta() {
    auto* large = static_cast<std::byte*>(VirtualAlloc(nullptr, 48 * MiB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* medium = static_cast<std::byte*>(VirtualAlloc(nullptr, 300 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* growing = static_cast<std::byte*>(VirtualAlloc(nullptr, 8 * MiB, MEM_RESERVE, PAGE_READWRITE));
    require(large != nullptr && medium != nullptr && growing != nullptr && VirtualAlloc(growing, MiB, MEM_COMMIT, PAGE_READWRITE) != nullptr, "allocate the delta fixtures");
    const auto before = WalkAddressSpace(0, 0, ThreadStackAllocations());
    auto* small = static_cast<std::byte*>(VirtualAlloc(nullptr, 20 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(small != nullptr && VirtualAlloc(growing + MiB, 2 * MiB, MEM_COMMIT, PAGE_READWRITE) != nullptr, "change the delta fixtures");
    VirtualFree(medium, 0, MEM_RELEASE);
    auto* held = VirtualAlloc(medium, 64 * 1024, MEM_RESERVE, PAGE_NOACCESS);
    const auto after = WalkAddressSpace(0, 0, ThreadStackAllocations());
    const auto delta = DiffAllocations(before.allocations, after.allocations, 4);
    require(delta.newCount[0] >= 1 && delta.newBytes[0] >= 20 * 1024, "a new small allocation was not counted in its size class");
    require(delta.vanishedCount >= 1 && delta.vanishedBytes >= 300 * 1024, "a released allocation was not counted as gone");
    require(delta.grownBytes >= 2 * MiB, "a reservation committing more was not counted as grown");
    require(!delta.largest.empty() && std::ranges::any_of(delta.largest, [&](const AllocationChange& change) { return change.base == reinterpret_cast<std::uintptr_t>(growing) && change.before == MiB && change.after == 3 * MiB; }), "the grown reservation is not among the largest changes");
    require(std::ranges::none_of(delta.largest, [&](const AllocationChange& change) { return change.base == reinterpret_cast<std::uintptr_t>(large); }), "an unchanged allocation is listed as a change");
    require(AllocationSizeClass(1000) == 0 && AllocationSizeClass(64 * 1024) == 1 && AllocationSizeClass(MiB) == 2 && AllocationSizeClass(16 * MiB) == 3, "size classes are misassigned");
    VirtualFree(large, 0, MEM_RELEASE);
    VirtualFree(small, 0, MEM_RELEASE);
    VirtualFree(growing, 0, MEM_RELEASE);
    if (held != nullptr) VirtualFree(held, 0, MEM_RELEASE);
}

unsigned __stdcall parkedThread(void* argument) {
    WaitForSingleObject(static_cast<HANDLE>(argument), INFINITE);
    return 0;
}

void checkThreadStacks() {
    const auto before = WalkAddressSpace(0, 0, ThreadStackAllocations());
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(release != nullptr, "create the release event");
    const auto thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, static_cast<unsigned>(16 * MiB), parkedThread, release, 0, nullptr));
    require(thread != nullptr, "start a thread with a committed 16 MiB stack");
    const auto during = WalkAddressSpace(0, 0, ThreadStackAllocations(), 64);
    SetEvent(release);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(release);
    require(during.threads == before.threads + 1, "the new thread is counted");
    require(during.stacks >= before.stacks + 16 * MiB, "the committed stack is counted as a thread stack, got " + std::to_string((during.stacks - before.stacks) / MiB) + " MiB more");
    require(std::ranges::none_of(during.largest, [](const auto& entry) { return entry.second >= 16 * MiB; }), "a thread stack is not listed among host allocations");
}
#endif

}

int main() {
    try {
#ifdef _WIN32
        checkArenaSkipped();
        checkHostCommit();
        checkThreadStacks();
        checkDelta();
        checkGpuMapped();
#endif
        require(ReadProcessCounters().commit != 0, "the process commit is read");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_process_memory_tests: %s\n", error.what());
        return 1;
    }
    std::puts("agc_process_memory_tests passed");
    return 0;
}

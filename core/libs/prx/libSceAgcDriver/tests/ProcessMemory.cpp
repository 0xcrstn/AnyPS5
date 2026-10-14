#include "prx/libSceAgcDriver/Execution/include/ProcessMemory.hpp"
#include <algorithm>
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
void checkArenaCommit() {
    constexpr std::uint64_t arenaBytes = 256 * MiB;
    auto* arena = static_cast<std::byte*>(VirtualAlloc(nullptr, arenaBytes, MEM_RESERVE, PAGE_NOACCESS));
    require(arena != nullptr, "reserve the test arena");
    const auto base = reinterpret_cast<std::uintptr_t>(arena);
    require(WalkAddressSpace(base, arenaBytes).arenaPrivate == 0, "a reserved arena commits nothing");
    require(VirtualAlloc(arena + 64 * MiB, 32 * MiB, MEM_COMMIT, PAGE_READWRITE) != nullptr, "commit inside the test arena");
    const auto inside = WalkAddressSpace(base, arenaBytes);
    require(inside.arenaPrivate == 32 * MiB, "the arena's committed bytes are counted as guest arena private memory, got " + std::to_string(inside.arenaPrivate));
    require(std::ranges::none_of(inside.largest, [&](const auto& entry) { return entry.first == base; }), "the arena is not listed among host allocations");
    VirtualFree(arena, 0, MEM_RELEASE);
}

void checkHostCommit() {
    const auto before = WalkAddressSpace(0, 0);
    const auto counters = ReadProcessCounters();
    auto* block = static_cast<std::byte*>(VirtualAlloc(nullptr, 48 * MiB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(block != nullptr, "commit a host block");
    const auto after = WalkAddressSpace(0, 0, 8);
    const auto grown = ReadProcessCounters();
    require(after.hostPrivate >= before.hostPrivate + 48 * MiB, "a committed host block is counted as host private memory");
    require(std::ranges::any_of(after.largest, [&](const auto& entry) { return entry.first == reinterpret_cast<std::uintptr_t>(block) && entry.second == 48 * MiB; }), "the committed host block is among the largest host allocations");
    require(grown.commit >= counters.commit + 48 * MiB, "the process commit grows with the committed block");
    require(after.image != 0 && after.regions != 0, "the walk sees the executable image");
    VirtualFree(block, 0, MEM_RELEASE);
}

unsigned __stdcall parkedThread(void* argument) {
    WaitForSingleObject(static_cast<HANDLE>(argument), INFINITE);
    return 0;
}

void checkThreadStacks() {
    const auto before = WalkAddressSpace(0, 0);
    const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(release != nullptr, "create the release event");
    const auto thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, static_cast<unsigned>(16 * MiB), parkedThread, release, 0, nullptr));
    require(thread != nullptr, "start a thread with a committed 16 MiB stack");
    const auto during = WalkAddressSpace(0, 0, 64);
    SetEvent(release);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(release);
    require(during.threads == before.threads + 1, "the new thread is counted");
    require(during.stacks >= before.stacks + 16 * MiB, "the committed stack is counted as a thread stack, got " + std::to_string((during.stacks - before.stacks) / MiB) + " MiB more");
    require(std::ranges::none_of(during.largest, [](const auto& entry) { return entry.second >= 16 * MiB; }), "a thread stack is not listed among host allocations");
}

void checkHeaps() {
    const auto before = ReadHostHeaps();
    const HANDLE heap = HeapCreate(0, 0, 0);
    require(heap != nullptr, "create a heap");
    void* block = HeapAlloc(heap, 0, 256 * 1024);
    require(block != nullptr, "allocate from the heap");
    const auto after = ReadHostHeaps();
    require(after.heaps == before.heaps + 1, "the new heap is counted");
    require(after.allocated >= before.allocated + 256 * 1024, "the heap allocation is counted");
    require(after.committed >= after.allocated, "a heap commits at least what it allocated");
    require(after.processHeapCommitted != 0 && after.processHeapCommitted <= after.committed, "the process heap is summarized");
    HeapDestroy(heap);
}
#endif

}

int main() {
    try {
#ifdef _WIN32
        checkArenaCommit();
        checkHostCommit();
        checkThreadStacks();
        checkHeaps();
#endif
        require(ReadProcessCounters().commit != 0, "the process commit is read");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_process_memory_tests: %s\n", error.what());
        return 1;
    }
    std::puts("agc_process_memory_tests passed");
    return 0;
}

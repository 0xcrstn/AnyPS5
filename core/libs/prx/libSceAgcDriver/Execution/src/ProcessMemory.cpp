#include "prx/libSceAgcDriver/Execution/include/ProcessMemory.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace AgcDriver::ProcessMemory {
namespace {

#ifdef _WIN32
std::unordered_set<std::uintptr_t> threadStacks(std::uint64_t& threads) {
    using QueryThread = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static const auto query = reinterpret_cast<QueryThread>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    std::unordered_set<std::uintptr_t> stacks;
    threads = 0;
    if (query == nullptr) return stacks;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return stacks;
    const auto process = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (bool more = Thread32First(snapshot, &entry) != 0; more; more = Thread32Next(snapshot, &entry) != 0) {
        if (entry.th32OwnerProcessID != process) continue;
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        struct {
            LONG exitStatus;
            void* teb;
            void* clientId[2];
            ULONG_PTR affinity;
            LONG priority;
            LONG basePriority;
        } basic{};
        NT_TIB tib{};
        SIZE_T read = 0;
        if (query(thread, 0, &basic, sizeof(basic), nullptr) == 0 && basic.teb != nullptr && ReadProcessMemory(GetCurrentProcess(), basic.teb, &tib, sizeof(tib), &read) != 0 && read == sizeof(tib) && tib.StackBase != nullptr) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(static_cast<const std::byte*>(tib.StackBase) - 1, &memory, sizeof(memory)) == sizeof(memory)) {
                stacks.insert(reinterpret_cast<std::uintptr_t>(memory.AllocationBase));
                ++threads;
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return stacks;
}
#endif

}

ProcessCounters ReadProcessCounters() {
    ProcessCounters counters;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != 0) {
        counters.commit = memory.PrivateUsage;
        counters.peakCommit = memory.PeakPagefileUsage;
        counters.workingSet = memory.WorkingSetSize;
    }
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t size = 0;
    std::uint64_t resident = 0;
    if (statm >> size >> resident) {
        const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
        counters.commit = size * page;
        counters.workingSet = resident * page;
    }
#endif
    return counters;
}

AddressSpaceUse WalkAddressSpace(std::uintptr_t arenaBase, std::uint64_t arenaBytes, std::size_t largestCount) {
    AddressSpaceUse use;
#ifdef _WIN32
    const auto stacks = threadStacks(use.threads);
    std::unordered_map<std::uintptr_t, std::uint64_t> allocations;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto cursor = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto limit = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION memory{};
    while (cursor < limit && VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == sizeof(memory)) {
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto bytes = static_cast<std::uint64_t>(memory.RegionSize);
        ++use.regions;
        if (memory.State == MEM_COMMIT) {
            const bool arena = arenaBytes != 0 && base >= arenaBase && base - arenaBase < arenaBytes;
            if (memory.Type == MEM_IMAGE) {
                use.image += bytes;
            } else if (memory.Type == MEM_MAPPED) {
                (arena ? use.arenaMapped : use.hostMapped) += bytes;
            } else if (arena) {
                use.arenaPrivate += bytes;
            } else {
                use.hostPrivate += bytes;
                const auto allocation = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
                if (stacks.contains(allocation)) use.stacks += bytes;
                else allocations[allocation] += bytes;
            }
        }
        if (base + bytes <= cursor) break;
        cursor = base + bytes;
    }
    use.largest.assign(allocations.begin(), allocations.end());
    const auto count = std::min(largestCount, use.largest.size());
    std::partial_sort(use.largest.begin(), use.largest.begin() + static_cast<std::ptrdiff_t>(count), use.largest.end(), [](const auto& left, const auto& right) { return left.second > right.second; });
    use.largest.resize(count);
#else
    static_cast<void>(arenaBase);
    static_cast<void>(arenaBytes);
    static_cast<void>(largestCount);
#endif
    return use;
}

HeapUse ReadHostHeaps() {
    HeapUse use;
#ifdef _WIN32
    struct Summary {
        DWORD cb;
        SIZE_T allocated;
        SIZE_T committed;
        SIZE_T reserved;
        SIZE_T maximumReserve;
    };
    using SummarizeHeap = BOOL(WINAPI*)(HANDLE, DWORD, Summary*);
    static const auto summarize = reinterpret_cast<SummarizeHeap>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary")));
    std::vector<HANDLE> heaps(64);
    for (;;) {
        const auto count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
        if (count <= heaps.size()) {
            heaps.resize(count);
            break;
        }
        heaps.resize(count);
    }
    use.heaps = heaps.size();
    if (summarize == nullptr) return use;
    for (const auto heap : heaps) {
        Summary summary{};
        summary.cb = sizeof(summary);
        if (summarize(heap, 0, &summary) == 0) continue;
        use.committed += summary.committed;
        use.allocated += summary.allocated;
        if (heap == GetProcessHeap()) use.processHeapCommitted = summary.committed;
    }
#endif
    return use;
}

}

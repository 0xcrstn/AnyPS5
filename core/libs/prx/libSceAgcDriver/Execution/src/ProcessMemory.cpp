#include "prx/libSceAgcDriver/Execution/include/ProcessMemory.hpp"
#include <algorithm>
#include <unordered_map>
#include <vector>
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

std::vector<std::uintptr_t> ThreadStackAllocations() {
#ifdef _WIN32
    using QueryThread = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static const auto query = reinterpret_cast<QueryThread>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    std::vector<std::uintptr_t> stacks;
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
                stacks.push_back(reinterpret_cast<std::uintptr_t>(memory.AllocationBase));
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    std::ranges::sort(stacks);
    return stacks;
#else
    return {};
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

AddressSpaceUse WalkAddressSpace(std::uintptr_t arenaBase, std::uint64_t arenaBytes, std::span<const std::uintptr_t> stacks, std::size_t largestCount) {
    AddressSpaceUse use;
#ifdef _WIN32
    std::unordered_map<std::uintptr_t, std::uint64_t> allocations;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto cursor = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto limit = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION memory{};
    while (cursor < limit) {
        if (arenaBytes != 0 && cursor >= arenaBase && cursor - arenaBase < arenaBytes) {
            cursor = arenaBase + arenaBytes;
            continue;
        }
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) break;
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto bytes = static_cast<std::uint64_t>(memory.RegionSize);
        ++use.regions;
        if (memory.State == MEM_COMMIT) {
            if (memory.Type == MEM_IMAGE) {
                use.image += bytes;
            } else if (memory.Type == MEM_MAPPED) {
                use.hostMapped += bytes;
            } else {
                use.hostPrivate += bytes;
                const auto allocation = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
                if (std::ranges::binary_search(stacks, allocation)) use.stacks += bytes;
                else allocations[allocation] += bytes;
            }
        }
        if (base + bytes <= cursor) break;
        cursor = base + bytes;
    }
    use.threads = stacks.size();
    use.largest.assign(allocations.begin(), allocations.end());
    const auto count = std::min(largestCount, use.largest.size());
    std::partial_sort(use.largest.begin(), use.largest.begin() + static_cast<std::ptrdiff_t>(count), use.largest.end(), [](const auto& left, const auto& right) { return left.second > right.second; });
    use.largest.resize(count);
#else
    static_cast<void>(arenaBase);
    static_cast<void>(arenaBytes);
    static_cast<void>(stacks);
    static_cast<void>(largestCount);
#endif
    return use;
}

}

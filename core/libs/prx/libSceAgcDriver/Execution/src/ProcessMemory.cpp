#include "prx/libSceAgcDriver/Execution/include/ProcessMemory.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
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
    std::unordered_set<std::uintptr_t> gpuMapped;
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
                if (std::ranges::binary_search(stacks, allocation)) {
                    use.stacks += bytes;
                } else {
                    allocations[allocation] += bytes;
                    if ((memory.Protect & PAGE_WRITECOMBINE) != 0) {
                        use.gpuMapped += bytes;
                        gpuMapped.insert(allocation);
                    }
                }
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
    use.allocations.assign(allocations.begin(), allocations.end());
    std::sort(use.allocations.begin(), use.allocations.end());
    use.gpuMappedBases.assign(gpuMapped.begin(), gpuMapped.end());
    std::sort(use.gpuMappedBases.begin(), use.gpuMappedBases.end());
    use.gpuMappedCount = use.gpuMappedBases.size();
#else
    static_cast<void>(arenaBase);
    static_cast<void>(arenaBytes);
    static_cast<void>(stacks);
    static_cast<void>(largestCount);
#endif
    return use;
}

std::size_t AllocationSizeClass(std::uint64_t bytes) {
    std::size_t sizeClass = 0;
    while (sizeClass < AllocationSizeClasses.size() && bytes >= AllocationSizeClasses[sizeClass]) ++sizeClass;
    return sizeClass;
}

AddressSpaceDelta DiffAllocations(std::span<const std::pair<std::uintptr_t, std::uint64_t>> before, std::span<const std::pair<std::uintptr_t, std::uint64_t>> after, std::size_t largestCount) {
    AddressSpaceDelta delta;
    std::vector<AllocationChange> changes;
    std::size_t old = 0;
    for (const auto& [base, bytes] : after) {
        while (old < before.size() && before[old].first < base) {
            delta.vanishedBytes += before[old].second;
            ++delta.vanishedCount;
            ++old;
        }
        if (old < before.size() && before[old].first == base) {
            const auto previous = before[old].second;
            ++old;
            if (bytes > previous) {
                delta.grownBytes += bytes - previous;
                changes.push_back({base, previous, bytes});
            } else if (bytes < previous) {
                delta.shrunkBytes += previous - bytes;
            }
            continue;
        }
        const auto sizeClass = AllocationSizeClass(bytes);
        ++delta.newCount[sizeClass];
        delta.newBytes[sizeClass] += bytes;
        changes.push_back({base, 0, bytes});
    }
    for (; old < before.size(); ++old) {
        delta.vanishedBytes += before[old].second;
        ++delta.vanishedCount;
    }
    const auto count = std::min(largestCount, changes.size());
    std::partial_sort(changes.begin(), changes.begin() + static_cast<std::ptrdiff_t>(count), changes.end(), [](const AllocationChange& left, const AllocationChange& right) { return left.after - left.before > right.after - right.before; });
    changes.resize(count);
    delta.largest = std::move(changes);
    return delta;
}

}

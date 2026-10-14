#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PROCESSMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PROCESSMEMORY_HPP

#include <cstddef>
#include <array>
#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::ProcessMemory {

struct ProcessCounters {
    std::uint64_t commit = 0;
    std::uint64_t peakCommit = 0;
    std::uint64_t workingSet = 0;
};

struct AddressSpaceUse {
    std::uint64_t hostPrivate = 0;
    std::uint64_t hostMapped = 0;
    std::uint64_t image = 0;
    std::uint64_t stacks = 0;
    std::uint64_t gpuMapped = 0;
    std::uint64_t gpuMappedCount = 0;
    std::uint64_t threads = 0;
    std::uint64_t regions = 0;
    std::vector<std::pair<std::uintptr_t, std::uint64_t>> largest;
    std::vector<std::pair<std::uintptr_t, std::uint64_t>> allocations;
    std::vector<std::uintptr_t> gpuMappedBases;
};

struct AllocationChange {
    std::uintptr_t base;
    std::uint64_t before;
    std::uint64_t after;
};

inline constexpr std::array<std::uint64_t, 3> AllocationSizeClasses{64u << 10u, 1u << 20u, 16u << 20u};

struct AddressSpaceDelta {
    std::array<std::uint64_t, 4> newCount{};
    std::array<std::uint64_t, 4> newBytes{};
    std::uint64_t grownBytes = 0;
    std::uint64_t shrunkBytes = 0;
    std::uint64_t vanishedCount = 0;
    std::uint64_t vanishedBytes = 0;
    std::vector<AllocationChange> largest;
};

std::size_t AllocationSizeClass(std::uint64_t bytes);
AddressSpaceDelta DiffAllocations(std::span<const std::pair<std::uintptr_t, std::uint64_t>> before, std::span<const std::pair<std::uintptr_t, std::uint64_t>> after, std::size_t largestCount);

ProcessCounters ReadProcessCounters();
std::vector<std::uintptr_t> ThreadStackAllocations();
AddressSpaceUse WalkAddressSpace(std::uintptr_t arenaBase, std::uint64_t arenaBytes, std::span<const std::uintptr_t> stacks, std::size_t largestCount = 4);

}

#endif

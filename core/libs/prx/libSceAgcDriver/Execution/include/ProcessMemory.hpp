#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PROCESSMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PROCESSMEMORY_HPP

#include <cstddef>
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
    std::uint64_t threads = 0;
    std::uint64_t regions = 0;
    std::vector<std::pair<std::uintptr_t, std::uint64_t>> largest;
};

ProcessCounters ReadProcessCounters();
std::vector<std::uintptr_t> ThreadStackAllocations();
AddressSpaceUse WalkAddressSpace(std::uintptr_t arenaBase, std::uint64_t arenaBytes, std::span<const std::uintptr_t> stacks, std::size_t largestCount = 4);

}

#endif

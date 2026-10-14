#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHRELOCATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHRELOCATION_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver::DriverDetail {

enum class DispatchKeyComponent : std::size_t { WaveSize, PartialThreads, Registers, Shader, Count };

struct DispatchKeyParts {
    std::uint64_t program = 0;
    std::uint64_t identity = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchKeyComponent::Count)> components{};
    std::span<const std::uint32_t> userData;
};

struct DispatchShape {
    std::uint64_t relocatable = 0;
    std::uint64_t read = ~std::uint64_t {0};
    std::size_t userWords = 0;
    std::vector<std::uint32_t> lastUserData;
};

struct DispatchDonor {
    std::shared_ptr<DispatchVariant> variant;
    std::vector<std::uint32_t> userData;
    std::uint64_t identity = 0;
};

enum class DispatchMiss : std::size_t { Unlearned, NoDonor, Refused, ContentDiffers, DataRefused, Gate, Count };

enum class DispatchIdentityMiss : std::size_t { FirstSight, WaveSize, PartialThreads, Registers, Shader, SameComponents, Count };

enum class DispatchWordMiss : std::size_t { ReadValue, ReadBase, Unread, None, Count };

struct DispatchRelocationCounters {
    std::uint64_t exactMisses = 0, hits = 0, patchedHits = 0, movedHits = 0, moves = 0, donorPuts = 0, donorNew = 0, donorEvictions = 0, donorEvicted = 0, shapeChanged = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchMiss::Count)> misses{};
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchIdentityMiss::Count)> identity{};
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchWordMiss::Count)> words{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrawStageMiss::Count)> content{};
    std::uint64_t wordAddressLike = 0, contentAddressLike = 0;
    std::array<std::uint64_t, 3> wordCounts{}, contentCounts{};
};

struct DispatchCacheTotals {
    std::uint64_t lookups = 0, exactHits = 0, relocatedHits = 0, patchedHits = 0, inserts = 0, relocationMisses = 0;
};

struct DispatchKeyLast {
    std::array<std::uint64_t, static_cast<std::size_t>(DispatchKeyComponent::Count)> components{};
};

}

#endif

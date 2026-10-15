#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_INDEXBOUNDS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_INDEXBOUNDS_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace AgcDriver::Graphics {

std::optional<std::uint32_t> LowestDrawIndex(std::span<const std::byte> indices, std::uint32_t indexSize, bool skipRestart);
std::optional<std::uint32_t> KnownLowestDrawIndex(std::uint64_t address, std::span<const std::byte> indices, std::uint32_t indexSize, bool skipRestart, std::uint64_t generation);

struct LowestIndexCounters {
    std::uint64_t hits = 0;
    std::uint64_t scans = 0;
};
LowestIndexCounters TakeLowestIndexCounters();

}

#endif

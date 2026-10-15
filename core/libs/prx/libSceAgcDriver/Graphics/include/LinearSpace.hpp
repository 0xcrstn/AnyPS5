#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LINEARSPACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LINEARSPACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "BdaAbi.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

inline constexpr std::uint32_t LinearPageShift = 16;
inline constexpr std::uint64_t LinearPageBytes = std::uint64_t{1} << LinearPageShift;

enum class LinearPageCode : std::uint32_t { Fallback = 0, Read = 1, ReadWrite = 2 };

struct LinearSpaceWindow {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress deviceBase = 0;
    std::uint64_t bytes = 0;
};

struct LinearPageCodes {
    std::uint64_t guestLow = 0;
    std::uint64_t guestHigh = 0;
    std::vector<std::uint32_t> words;
};

struct LinearSpaceCounters {
    std::uint64_t boundPages = 0;
    std::uint64_t usedPages = 0;
    std::uint64_t binds = 0;
    std::uint64_t unbinds = 0;
    std::uint64_t refused = 0;
    std::uint64_t tables = 0;
    std::uint64_t codedPages = 0;
};

bool ParseLinearSpace(const char* text);
bool LinearSpaceEnabled(const Context& context);
std::optional<LinearSpaceWindow> LinearSpace(const Context& context);
bool AcquireLinearRange(const Context& context, std::uint64_t begin, std::uint64_t end, std::uint64_t budget);
void ReleaseLinearRange(VkDevice device, std::uint64_t begin, std::uint64_t end);
void ClearLinearSpace(VkDevice device);
LinearSpaceCounters LinearSpaceTotals();
LinearPageCodes BuildLinearPageCodes(std::span<const ShaderRecompiler::BdaAbi::Range> ranges, VkDeviceAddress deviceBase);
void CountLinearTable(const LinearPageCodes& codes);
LinearPageCode LinearPageCodeAt(const LinearPageCodes& codes, std::uint64_t address);

}

#endif

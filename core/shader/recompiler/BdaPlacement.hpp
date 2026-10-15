#ifndef CORE_SHADER_RECOMPILER_BDAPLACEMENT_HPP
#define CORE_SHADER_RECOMPILER_BDAPLACEMENT_HPP

#include "BdaAbi.hpp"
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ShaderRecompiler::BdaPlacement {

enum class Mode : std::uint32_t { Host, Device, Verify };

inline Mode ParseMode(const char* value) {
    if (value == nullptr) return Mode::Host;
    const std::string_view text(value);
    if (text == "host") return Mode::Host;
    if (text == "device") return Mode::Device;
    if (text == "verify") return Mode::Verify;
    throw std::runtime_error("APS5_BDA_PLACEMENT must be host, device or verify, not \"" + std::string(text) + "\"");
}

enum class Pass : std::uint32_t { CopyBack, Verify };

struct Header {
    std::uint32_t count;
    std::uint32_t mismatches;
    std::uint32_t claimed;
    std::uint32_t reserved;
    std::uint64_t firstMismatch;
    std::uint64_t reserved2;
};

struct Entry {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t work;
    std::uint64_t pristine;
    std::uint64_t host;
};

struct PushConstants {
    std::uint64_t record;
    std::uint64_t table;
    std::uint32_t pass;
    std::uint32_t reserved;
};

static_assert(sizeof(Header) == 32 && sizeof(Entry) == 40 && sizeof(PushConstants) == 24);

inline constexpr std::uint32_t WorkgroupSize = 64;
inline constexpr std::uint32_t PageBytes = 1u << BdaAbi::WrittenPageShift;
inline constexpr std::uint32_t DispatchX = BdaAbi::WrittenPageSlots;
inline constexpr std::uint32_t DispatchY = BdaAbi::WrittenChunkPages;

std::vector<std::uint32_t> BuildCopyBackShader();

}

#endif

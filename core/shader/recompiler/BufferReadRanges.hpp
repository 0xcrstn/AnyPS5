#ifndef CORE_SHADER_RECOMPILER_BUFFERREADRANGES_HPP
#define CORE_SHADER_RECOMPILER_BUFFERREADRANGES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ShaderRecompiler {

struct BufferReadRange {
    std::uint32_t firstIndex = 0;
    std::uint32_t lastIndex = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    bool operator==(const BufferReadRange& other) const = default;
};

enum class BufferReadUnbound : std::uint8_t { None, Offset, Soffset, ScalarOffset, Wrap, Opcode, Alias, Count };

inline constexpr std::uint64_t FormattedElementBytes = 16;

struct BufferReadRanges {
    static constexpr std::size_t MaxRanges = 16;
    static constexpr std::uint32_t AnyIndex = 0xffffffffu;
    bool bounded = true;
    BufferReadUnbound unbound = BufferReadUnbound::None;
    std::vector<BufferReadRange> ranges;

    void Add(const BufferReadRange& range) {
        if (!bounded) return;
        for (auto& held : ranges) {
            if (held.firstIndex == range.firstIndex && held.lastIndex == range.lastIndex && range.begin <= held.end && held.begin <= range.end) {
                held.begin = std::min(held.begin, range.begin);
                held.end = std::max(held.end, range.end);
                return;
            }
        }
        if (ranges.size() < MaxRanges) {
            ranges.push_back(range);
            return;
        }
        auto& hull = ranges.front();
        for (const auto& held : ranges) {
            hull.firstIndex = std::min(hull.firstIndex, held.firstIndex);
            hull.lastIndex = std::max(hull.lastIndex, held.lastIndex);
            hull.begin = std::min(hull.begin, held.begin);
            hull.end = std::max(hull.end, held.end);
        }
        hull.firstIndex = std::min(hull.firstIndex, range.firstIndex);
        hull.lastIndex = std::max(hull.lastIndex, range.lastIndex);
        hull.begin = std::min(hull.begin, range.begin);
        hull.end = std::max(hull.end, range.end);
        ranges.resize(1);
    }
    void Unbound(BufferReadUnbound reason) {
        if (bounded) unbound = reason;
        bounded = false;
        ranges.clear();
    }
    bool operator==(const BufferReadRanges& other) const = default;
};

}

#endif

#ifndef CORE_SHADER_RECOMPILER_BUFFERREADRANGES_HPP
#define CORE_SHADER_RECOMPILER_BUFFERREADRANGES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ShaderRecompiler {

enum class BufferIndexSource : std::uint8_t { None, VertexIndex, InstanceIndex, Count };

enum class BufferIndexOrigin : std::uint8_t { Bounded, VertexIndex, InstanceIndex, Memory, UserData, Other, Count };

struct BufferReadRange {
    std::uint32_t firstIndex = 0;
    std::uint32_t lastIndex = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    BufferIndexSource indexSource = BufferIndexSource::None;
    BufferIndexOrigin indexOrigin = BufferIndexOrigin::Bounded;
    std::uint32_t indexScale = 0;

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
            if (held.firstIndex == range.firstIndex && held.lastIndex == range.lastIndex && held.indexSource == range.indexSource && held.indexScale == range.indexScale && held.indexOrigin == range.indexOrigin && range.begin <= held.end && held.begin <= range.end) {
                held.begin = std::min(held.begin, range.begin);
                held.end = std::max(held.end, range.end);
                return;
            }
        }
        if (ranges.size() < MaxRanges) {
            ranges.push_back(range);
            return;
        }
        auto hull = range;
        for (const auto& held : ranges) {
            const bool sameSource = held.indexSource == hull.indexSource && held.indexScale == hull.indexScale;
            hull.firstIndex = std::min(hull.firstIndex, held.firstIndex);
            hull.lastIndex = std::max(hull.lastIndex, held.lastIndex);
            hull.begin = std::min(hull.begin, held.begin);
            hull.end = std::max(hull.end, held.end);
            if (!sameSource) {
                hull.indexSource = BufferIndexSource::None;
                hull.indexScale = 0;
                hull.firstIndex = 0;
                hull.lastIndex = AnyIndex;
                hull.indexOrigin = BufferIndexOrigin::Other;
            }
        }
        if (hull.indexSource == BufferIndexSource::None && hull.lastIndex == AnyIndex && hull.indexOrigin == BufferIndexOrigin::Bounded) hull.indexOrigin = BufferIndexOrigin::Other;
        ranges.assign(1, hull);
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

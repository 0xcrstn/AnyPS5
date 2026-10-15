#ifndef CORE_SHADER_RECOMPILER_BUFFERREADRANGES_HPP
#define CORE_SHADER_RECOMPILER_BUFFERREADRANGES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

enum class BufferIndexSource : std::uint8_t { None, VertexIndex, InstanceIndex, Expression, Count };

enum class BufferIndexOrigin : std::uint8_t { Bounded, VertexIndex, InstanceIndex, Memory, UserData, Other, Count };

enum class BufferIndexOp : std::uint8_t { Range, VertexIndex, InstanceIndex, UserData, Add, Sub, Mul, MulHigh, ShiftLeft, ShiftRight, And, Or, Divide, Min, Max, Extract, Select, Count };

struct BufferIndexNode {
    BufferIndexOp op = BufferIndexOp::Range;
    std::uint8_t left = 0;
    std::uint8_t right = 0;
    std::uint32_t low = 0;
    std::uint32_t high = 0;

    bool operator==(const BufferIndexNode& other) const = default;
};

struct BufferIndexInputs {
    std::uint32_t firstVertex = 0;
    std::uint32_t lastVertex = 0;
    std::uint32_t firstInstance = 0;
    std::uint32_t lastInstance = 0;
    std::int32_t vertexFoldSgpr = -1;
    std::uint32_t vertexFold = 0;
    std::int32_t instanceFoldSgpr = -1;
    std::uint32_t instanceFold = 0;
    std::uint32_t firstUserSgpr = 0;
    std::span<const std::uint32_t> userData{};
};

inline std::optional<std::pair<std::uint64_t, std::uint64_t>> EvaluateBufferIndex(const std::vector<BufferIndexNode>& nodes, const BufferIndexInputs& inputs) {
    using Interval = std::pair<std::uint64_t, std::uint64_t>;
    constexpr std::uint64_t Max32 = 0xffffffffull;
    std::vector<Interval> values;
    values.reserve(nodes.size());
    const auto ones = [](std::uint64_t value) {
        std::uint64_t mask = 0;
        while (mask < value) mask = (mask << 1u) | 1u;
        return mask;
    };
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        const bool leaf = node.op == BufferIndexOp::Range || node.op == BufferIndexOp::VertexIndex || node.op == BufferIndexOp::InstanceIndex || node.op == BufferIndexOp::UserData;
        const bool binary = node.op == BufferIndexOp::Add || node.op == BufferIndexOp::Sub || node.op == BufferIndexOp::Mul || node.op == BufferIndexOp::MulHigh || node.op == BufferIndexOp::And || node.op == BufferIndexOp::Or || node.op == BufferIndexOp::Min || node.op == BufferIndexOp::Max || node.op == BufferIndexOp::Select;
        if ((!leaf && node.left >= index) || (binary && node.right >= index)) return std::nullopt;
        const auto& a = node.left < values.size() ? values[node.left] : Interval{0, 0};
        const auto& b = node.right < values.size() ? values[node.right] : Interval{0, 0};
        Interval result{0, 0};
        switch (node.op) {
        case BufferIndexOp::Range:
            result = {node.low, node.high};
            break;
        case BufferIndexOp::VertexIndex:
            result = {inputs.firstVertex, inputs.lastVertex};
            break;
        case BufferIndexOp::InstanceIndex:
            result = {inputs.firstInstance, inputs.lastInstance};
            break;
        case BufferIndexOp::UserData:
            if (inputs.vertexFoldSgpr >= 0 && node.low == static_cast<std::uint32_t>(inputs.vertexFoldSgpr)) result = {inputs.vertexFold, inputs.vertexFold};
            else if (inputs.instanceFoldSgpr >= 0 && node.low == static_cast<std::uint32_t>(inputs.instanceFoldSgpr)) result = {inputs.instanceFold, inputs.instanceFold};
            else if (node.low >= inputs.firstUserSgpr && node.low - inputs.firstUserSgpr < inputs.userData.size()) result = {inputs.userData[node.low - inputs.firstUserSgpr], inputs.userData[node.low - inputs.firstUserSgpr]};
            else return std::nullopt;
            break;
        case BufferIndexOp::Add:
            result = {a.first + b.first, a.second + b.second};
            break;
        case BufferIndexOp::Sub:
            if (a.first < b.second) return std::nullopt;
            result = {a.first - b.second, a.second - b.first};
            break;
        case BufferIndexOp::Mul:
            if (b.second != 0 && a.second > Max32 / b.second) return std::nullopt;
            result = {a.first * b.first, a.second * b.second};
            break;
        case BufferIndexOp::MulHigh:
            result = {(a.first * b.first) >> 32u, (a.second * b.second) >> 32u};
            break;
        case BufferIndexOp::ShiftLeft:
            if (node.low >= 32u) return std::nullopt;
            result = {a.first << node.low, a.second << node.low};
            break;
        case BufferIndexOp::ShiftRight:
            if (node.low >= 32u) return std::nullopt;
            result = {a.first >> node.low, a.second >> node.low};
            break;
        case BufferIndexOp::And:
            result = {0, std::min(a.second, b.second)};
            break;
        case BufferIndexOp::Or:
            result = {std::max(a.first, b.first), ones(std::max(a.second, b.second))};
            break;
        case BufferIndexOp::Divide:
            if (node.low == 0) return std::nullopt;
            result = {a.first / node.low, a.second / node.low};
            break;
        case BufferIndexOp::Min:
            result = {std::min(a.first, b.first), std::min(a.second, b.second)};
            break;
        case BufferIndexOp::Max:
            result = {std::max(a.first, b.first), std::max(a.second, b.second)};
            break;
        case BufferIndexOp::Extract: {
            if (node.low >= 32u || node.high == 0 || node.high >= 32u) return std::nullopt;
            const auto low = a.first >> node.low;
            const auto high = a.second >> node.low;
            const auto mask = (std::uint64_t{1} << node.high) - 1u;
            result = (low >> node.high) == (high >> node.high) ? Interval{low & mask, high & mask} : Interval{0, mask};
            break;
        }
        case BufferIndexOp::Select:
            result = {std::min(a.first, b.first), std::max(a.second, b.second)};
            break;
        default:
            return std::nullopt;
        }
        if (result.second > Max32 || result.first > result.second) return std::nullopt;
        values.push_back(result);
    }
    if (values.empty()) return std::nullopt;
    return values.back();
}

struct BufferReadRange {
    std::uint32_t firstIndex = 0;
    std::uint32_t lastIndex = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    BufferIndexSource indexSource = BufferIndexSource::None;
    BufferIndexOrigin indexOrigin = BufferIndexOrigin::Bounded;
    std::uint32_t indexScale = 0;
    std::vector<BufferIndexNode> indexExpression;
    std::uint16_t blockingOpcode = 0;

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
            if (held.firstIndex == range.firstIndex && held.lastIndex == range.lastIndex && held.indexSource == range.indexSource && held.indexScale == range.indexScale && held.indexOrigin == range.indexOrigin && held.indexExpression == range.indexExpression && held.blockingOpcode == range.blockingOpcode && range.begin <= held.end && held.begin <= range.end) {
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
            const bool sameSource = held.indexSource == hull.indexSource && held.indexScale == hull.indexScale && held.indexExpression == hull.indexExpression;
            hull.firstIndex = std::min(hull.firstIndex, held.firstIndex);
            hull.lastIndex = std::max(hull.lastIndex, held.lastIndex);
            hull.begin = std::min(hull.begin, held.begin);
            hull.end = std::max(hull.end, held.end);
            if (!sameSource) {
                hull.indexSource = BufferIndexSource::None;
                hull.indexScale = 0;
                hull.indexExpression.clear();
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

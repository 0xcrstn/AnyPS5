#ifndef CORE_SHADER_RECOMPILER_BUFFERREADRANGES_HPP
#define CORE_SHADER_RECOMPILER_BUFFERREADRANGES_HPP

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

enum class BufferIndexSource : std::uint8_t { None, VertexIndex, InstanceIndex, Expression, Count };

enum class BufferIndexOrigin : std::uint8_t { Bounded, VertexIndex, InstanceIndex, Memory, UserData, Other, Count };

enum class BufferIndexOp : std::uint8_t { Range, VertexIndex, InstanceIndex, UserData, Add, Sub, Mul, MulHigh, ShiftLeft, ShiftRight, And, Or, Divide, Min, Max, Extract, Select, FloatConstant, FloatFromUnsigned, FloatAdd, FloatSub, FloatMul, FloatFloor, FloatTrunc, FloatMin, FloatMax, FloatFma, UnsignedFromFloat, Count };

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

enum class BufferIndexFailure : std::uint8_t { None, UserWord, Underflow, Overflow, Malformed, Count };

inline std::optional<std::pair<std::uint64_t, std::uint64_t>> EvaluateBufferIndex(const std::vector<BufferIndexNode>& nodes, const BufferIndexInputs& inputs, BufferIndexFailure* failure = nullptr) {
    using Interval = std::pair<std::uint64_t, std::uint64_t>;
    constexpr std::uint64_t Max32 = 0xffffffffull;
    const auto fail = [&](BufferIndexFailure why) -> std::optional<Interval> {
        if (failure != nullptr) *failure = why;
        return std::nullopt;
    };
    using FloatInterval = std::pair<float, float>;
    std::vector<Interval> values;
    values.reserve(nodes.size());
    std::vector<FloatInterval> floats(nodes.size());
    std::vector<std::uint8_t> floating(nodes.size(), 0u);
    const auto isFloat = [](BufferIndexOp op) { return op >= BufferIndexOp::FloatConstant && op < BufferIndexOp::UnsignedFromFloat; };
    const auto hull = [](std::initializer_list<float> candidates) {
        FloatInterval value{std::min(candidates), std::max(candidates)};
        constexpr float smallest = 0x1p-126f;
        if (value.first > 0.0f && value.first < smallest) value.first = 0.0f;
        if (value.second < 0.0f && value.second > -smallest) value.second = 0.0f;
        return value;
    };
    const auto ones = [](std::uint64_t value) {
        std::uint64_t mask = 0;
        while (mask < value) mask = (mask << 1u) | 1u;
        return mask;
    };
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        const bool leaf = node.op == BufferIndexOp::Range || node.op == BufferIndexOp::VertexIndex || node.op == BufferIndexOp::InstanceIndex || node.op == BufferIndexOp::UserData || node.op == BufferIndexOp::FloatConstant;
        const bool binary = node.op == BufferIndexOp::Add || node.op == BufferIndexOp::Sub || node.op == BufferIndexOp::Mul || node.op == BufferIndexOp::MulHigh || node.op == BufferIndexOp::And || node.op == BufferIndexOp::Or || node.op == BufferIndexOp::Min || node.op == BufferIndexOp::Max || node.op == BufferIndexOp::Select || node.op == BufferIndexOp::FloatAdd || node.op == BufferIndexOp::FloatSub || node.op == BufferIndexOp::FloatMul || node.op == BufferIndexOp::FloatMin || node.op == BufferIndexOp::FloatMax || node.op == BufferIndexOp::FloatFma;
        if ((!leaf && node.left >= index) || (binary && node.right >= index)) return fail(BufferIndexFailure::Malformed);
        const bool floatArguments = isFloat(node.op) ? node.op != BufferIndexOp::FloatFromUnsigned : node.op == BufferIndexOp::UnsignedFromFloat;
        if ((!leaf && (floating[node.left] != 0u) != floatArguments) || (binary && (floating[node.right] != 0u) != floatArguments)) return fail(BufferIndexFailure::Malformed);
        if (isFloat(node.op)) {
            const auto& x = floats[node.left];
            const auto& y = floats[node.right];
            FloatInterval value{0.0f, 0.0f};
            switch (node.op) {
            case BufferIndexOp::FloatConstant: {
                const auto constant = std::bit_cast<float>(node.low);
                if (!std::isfinite(constant)) return fail(BufferIndexFailure::Malformed);
                value = hull({constant, constant});
                break;
            }
            case BufferIndexOp::FloatFromUnsigned:
                value = hull({static_cast<float>(values[node.left].first), static_cast<float>(values[node.left].second)});
                break;
            case BufferIndexOp::FloatAdd:
                value = hull({x.first + y.first, x.second + y.second});
                break;
            case BufferIndexOp::FloatSub:
                value = hull({x.first - y.second, x.second - y.first});
                break;
            case BufferIndexOp::FloatMul:
                value = hull({x.first * y.first, x.first * y.second, x.second * y.first, x.second * y.second});
                break;
            case BufferIndexOp::FloatFma: {
                if (node.low >= index || floating[node.low] == 0u) return fail(BufferIndexFailure::Malformed);
                const auto& z = floats[node.low];
                const auto fused = [](float a, float b, float c) { return std::fma(a, b, c); };
                const auto separate = [](float a, float b, float c) { const volatile float product = a * b; return product + c; };
                value = hull({fused(x.first, y.first, z.first), fused(x.first, y.second, z.first), fused(x.second, y.first, z.first), fused(x.second, y.second, z.first), fused(x.first, y.first, z.second), fused(x.first, y.second, z.second), fused(x.second, y.first, z.second), fused(x.second, y.second, z.second), separate(x.first, y.first, z.first), separate(x.first, y.second, z.first), separate(x.second, y.first, z.first), separate(x.second, y.second, z.first), separate(x.first, y.first, z.second), separate(x.first, y.second, z.second), separate(x.second, y.first, z.second), separate(x.second, y.second, z.second)});
                break;
            }
            case BufferIndexOp::FloatFloor:
                value = {std::floor(x.first), std::floor(x.second)};
                break;
            case BufferIndexOp::FloatTrunc:
                value = {std::trunc(x.first), std::trunc(x.second)};
                break;
            case BufferIndexOp::FloatMin:
                value = {std::min(x.first, y.first), std::min(x.second, y.second)};
                break;
            case BufferIndexOp::FloatMax:
                value = {std::max(x.first, y.first), std::max(x.second, y.second)};
                break;
            default:
                return fail(BufferIndexFailure::Malformed);
            }
            if (!std::isfinite(value.first) || !std::isfinite(value.second)) return fail(BufferIndexFailure::Overflow);
            if (value.first > value.second) return fail(BufferIndexFailure::Malformed);
            floats[index] = value;
            floating[index] = 1u;
            values.push_back({0, 0});
            continue;
        }
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
            else return fail(BufferIndexFailure::UserWord);
            break;
        case BufferIndexOp::Add:
            result = {a.first + b.first, a.second + b.second};
            break;
        case BufferIndexOp::Sub:
            if (a.first < b.second) return fail(BufferIndexFailure::Underflow);
            result = {a.first - b.second, a.second - b.first};
            break;
        case BufferIndexOp::Mul:
            if (b.second != 0 && a.second > Max32 / b.second) return fail(BufferIndexFailure::Overflow);
            result = {a.first * b.first, a.second * b.second};
            break;
        case BufferIndexOp::MulHigh:
            result = {(a.first * b.first) >> 32u, (a.second * b.second) >> 32u};
            break;
        case BufferIndexOp::ShiftLeft:
            if (node.low >= 32u) return fail(BufferIndexFailure::Malformed);
            result = {a.first << node.low, a.second << node.low};
            break;
        case BufferIndexOp::ShiftRight:
            if (node.low >= 32u) return fail(BufferIndexFailure::Malformed);
            result = {a.first >> node.low, a.second >> node.low};
            break;
        case BufferIndexOp::And:
            result = {0, std::min(a.second, b.second)};
            break;
        case BufferIndexOp::Or:
            result = {std::max(a.first, b.first), ones(std::max(a.second, b.second))};
            break;
        case BufferIndexOp::Divide:
            if (node.low == 0) return fail(BufferIndexFailure::Malformed);
            result = {a.first / node.low, a.second / node.low};
            break;
        case BufferIndexOp::Min:
            result = {std::min(a.first, b.first), std::min(a.second, b.second)};
            break;
        case BufferIndexOp::Max:
            result = {std::max(a.first, b.first), std::max(a.second, b.second)};
            break;
        case BufferIndexOp::Extract: {
            if (node.low >= 32u || node.high == 0 || node.high >= 32u) return fail(BufferIndexFailure::Malformed);
            const auto low = a.first >> node.low;
            const auto high = a.second >> node.low;
            const auto mask = (std::uint64_t{1} << node.high) - 1u;
            result = (low >> node.high) == (high >> node.high) ? Interval{low & mask, high & mask} : Interval{0, mask};
            break;
        }
        case BufferIndexOp::Select:
            result = {std::min(a.first, b.first), std::max(a.second, b.second)};
            break;
        case BufferIndexOp::UnsignedFromFloat: {
            const auto& x = floats[node.left];
            if (x.first <= -1.0) return fail(BufferIndexFailure::Underflow);
            if (x.second >= 4294967296.0) return fail(BufferIndexFailure::Overflow);
            result = {x.first <= 0.0 ? 0u : static_cast<std::uint64_t>(std::floor(x.first)), x.second <= 0.0 ? 0u : static_cast<std::uint64_t>(std::floor(x.second))};
            break;
        }
        default:
            return fail(BufferIndexFailure::Malformed);
        }
        if (result.second > Max32) return fail(BufferIndexFailure::Overflow);
        if (result.first > result.second) return fail(BufferIndexFailure::Malformed);
        values.push_back(result);
    }
    if (values.empty() || floating.back() != 0u) return fail(BufferIndexFailure::Malformed);
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

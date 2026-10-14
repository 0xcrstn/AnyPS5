#include "Optimization/BufferReadAnalysis.hpp"
#include <algorithm>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

namespace {

constexpr std::uint64_t Max32 = 0xffffffffull;

std::optional<BufferReadAnalysis::Interval> fits(BufferReadAnalysis::Interval interval) {
    return interval.high <= Max32 ? std::optional<BufferReadAnalysis::Interval>(interval) : std::nullopt;
}

std::uint64_t ones(std::uint64_t value) {
    std::uint64_t mask = 0;
    while (mask < value) mask = (mask << 1u) | 1u;
    return mask;
}

}

std::optional<BufferReadAnalysis::Interval> BufferReadAnalysis::Of(const IrValue* value) {
    if (value == nullptr) return std::nullopt;
    value = value->Resolve();
    if (value == nullptr) return std::nullopt;
    if (value->HasImmediate()) return Interval{value->ImmediateU32(), value->ImmediateU32()};
    if (const auto known = intervals.find(value); known != intervals.end()) return known->second;
    if (!visiting.insert(value).second || visiting.size() > MaxDepth) return std::nullopt;
    const auto result = compute(*value);
    visiting.erase(value);
    intervals.emplace(value, result);
    return result;
}

std::optional<BufferReadAnalysis::Interval> BufferReadAnalysis::compute(const IrValue& value) {
    const auto argument = [&](std::size_t index) { return index < value.ArgumentCount() ? Of(value.Argument(index)) : std::nullopt; };
    switch (value.Opcode()) {
    case IrOpcode::IAdd32: {
        const auto a = argument(0), b = argument(1);
        if (!a || !b) return std::nullopt;
        return fits({a->low + b->low, a->high + b->high});
    }
    case IrOpcode::IMul32: {
        const auto a = argument(0), b = argument(1);
        if (!a || !b || (b->high != 0 && a->high > Max32 / b->high)) return std::nullopt;
        return Interval{a->low * b->low, a->high * b->high};
    }
    case IrOpcode::ShiftLeftLogical32:
    case IrOpcode::IShiftLeft32: {
        const auto a = argument(0), b = argument(1);
        if (!a || !b || b->low != b->high || b->high >= 32u) return std::nullopt;
        return fits({a->low << b->high, a->high << b->high});
    }
    case IrOpcode::ShiftRightLogical32:
    case IrOpcode::IShiftRightLogical32: {
        const auto a = argument(0), b = argument(1);
        if (!b || b->low != b->high || b->high >= 32u) return std::nullopt;
        if (!a) return Interval{0, Max32 >> b->high};
        return Interval{a->low >> b->high, a->high >> b->high};
    }
    case IrOpcode::BitwiseAnd32:
    case IrOpcode::IAnd32: {
        const auto a = argument(0), b = argument(1);
        if (a && b) return Interval{0, std::min(a->high, b->high)};
        if (a) return Interval{0, a->high};
        if (b) return Interval{0, b->high};
        return std::nullopt;
    }
    case IrOpcode::BitwiseOr32:
    case IrOpcode::IOr32: {
        const auto a = argument(0), b = argument(1);
        if (!a || !b) return std::nullopt;
        return Interval{std::max(a->low, b->low), ones(std::max(a->high, b->high))};
    }
    case IrOpcode::UMin32: {
        const auto a = argument(0), b = argument(1);
        if (a && b) return Interval{std::min(a->low, b->low), std::min(a->high, b->high)};
        if (a) return Interval{0, a->high};
        if (b) return Interval{0, b->high};
        return std::nullopt;
    }
    case IrOpcode::UMax32: {
        const auto a = argument(0), b = argument(1);
        if (!a || !b) return std::nullopt;
        return Interval{std::max(a->low, b->low), std::max(a->high, b->high)};
    }
    case IrOpcode::SelectU32: {
        const auto a = argument(1), b = argument(2);
        if (!a || !b) return std::nullopt;
        return Interval{std::min(a->low, b->low), std::max(a->high, b->high)};
    }
    case IrOpcode::BitFieldUExtract: {
        const auto width = argument(2);
        if (!width || width->low != width->high) return std::nullopt;
        if (width->high < 32u) return Interval{0, (std::uint64_t{1} << width->high) - 1u};
        return std::nullopt;
    }
    case IrOpcode::Phi: {
        if (value.ArgumentCount() == 0) return std::nullopt;
        Interval result{Max32, 0};
        for (std::size_t index = 0; index < value.ArgumentCount(); ++index) {
            const auto incoming = Of(value.Argument(index));
            if (!incoming) return std::nullopt;
            result.low = std::min(result.low, incoming->low);
            result.high = std::max(result.high, incoming->high);
        }
        return result;
    }
    default:
        return std::nullopt;
    }
}

std::optional<BufferReadAnalysis::Affine> BufferReadAnalysis::AffineOf(const IrValue* value, std::uint32_t depth) {
    if (value == nullptr) return std::nullopt;
    value = value->Resolve();
    if (value == nullptr) return std::nullopt;
    if (const auto plain = Of(value)) return Affine{BufferIndexSource::None, 0u, plain->low, plain->high};
    if (depth >= MaxAffineDepth) return std::nullopt;
    const auto argument = [&](std::size_t index) { return index < value->ArgumentCount() ? AffineOf(value->Argument(index), depth + 1u) : std::nullopt; };
    const auto sourced = [](const std::optional<Affine>& affine) { return affine && affine->source != BufferIndexSource::None; };
    const auto constant = [](const std::optional<Affine>& affine) { return affine && affine->source == BufferIndexSource::None && affine->low == affine->high; };
    switch (value->Opcode()) {
    case IrOpcode::GetBuiltin: {
        if (value->ArgumentCount() != 2u) return std::nullopt;
        const auto* kind = value->Argument(0)->Resolve();
        const auto* component = value->Argument(1)->Resolve();
        if (!kind->HasImmediate() || !component->HasImmediate() || component->ImmediateU32() != 0u) return std::nullopt;
        if (kind->ImmediateU32() == static_cast<std::uint32_t>(StageInputKind::VertexIndex)) return Affine{BufferIndexSource::VertexIndex, 1u, 0u, 0u};
        if (kind->ImmediateU32() == static_cast<std::uint32_t>(StageInputKind::InstanceIndex)) return Affine{BufferIndexSource::InstanceIndex, 1u, 0u, 0u};
        return std::nullopt;
    }
    case IrOpcode::IAdd32: {
        auto a = argument(0), b = argument(1);
        if (!a || !b || (sourced(a) && sourced(b))) return std::nullopt;
        if (sourced(b)) std::swap(a, b);
        if (a->low + b->low > Max32 || a->high + b->high > Max32) return std::nullopt;
        return Affine{a->source, a->scale, a->low + b->low, a->high + b->high};
    }
    case IrOpcode::IMul32: {
        auto a = argument(0), b = argument(1);
        if (sourced(b)) std::swap(a, b);
        if (!sourced(a) || !constant(b)) return std::nullopt;
        const auto factor = b->low;
        if (factor != 0 && (a->scale > Max32 / factor || a->high > Max32 / factor)) return std::nullopt;
        return Affine{a->source, a->scale * factor, a->low * factor, a->high * factor};
    }
    case IrOpcode::ShiftLeftLogical32:
    case IrOpcode::IShiftLeft32: {
        const auto a = argument(0), b = argument(1);
        if (!sourced(a) || !constant(b) || b->low >= 32u) return std::nullopt;
        if ((a->scale << b->low) > Max32 || (a->high << b->low) > Max32) return std::nullopt;
        return Affine{a->source, a->scale << b->low, a->low << b->low, a->high << b->low};
    }
    default:
        return std::nullopt;
    }
}

BufferIndexOrigin BufferReadAnalysis::OriginOf(const IrValue* value) {
    std::vector<const IrValue*> pending{value};
    std::unordered_set<const IrValue*> seen;
    bool vertex = false, instance = false, memory = false, userData = false;
    while (!pending.empty() && seen.size() < MaxOriginValues) {
        const auto* current = pending.back();
        pending.pop_back();
        if (current == nullptr) continue;
        current = current->Resolve();
        if (current == nullptr || current->HasImmediate() || !seen.insert(current).second) continue;
        const auto op = current->Opcode();
        if (op == IrOpcode::GetBuiltin && current->ArgumentCount() == 2u && current->Argument(0)->Resolve()->HasImmediate()) {
            const auto kind = current->Argument(0)->Resolve()->ImmediateU32();
            vertex = vertex || kind == static_cast<std::uint32_t>(StageInputKind::VertexIndex);
            instance = instance || kind == static_cast<std::uint32_t>(StageInputKind::InstanceIndex);
            continue;
        }
        if (op == IrOpcode::GetUserData) {
            userData = true;
            continue;
        }
        if (BufferAccessOf(op) != BufferAccess::None || AddressOpcodeInfoOf(op).access != AddressAccess::None || SharedAccessOf(op) != SharedAccess::None || ImageOpcodeInfoOf(op).access != ImageAccess::None || op == IrOpcode::GetAttribute) {
            memory = true;
            continue;
        }
        for (std::size_t index = 0; index < current->ArgumentCount(); ++index) pending.push_back(current->Argument(index));
    }
    if (vertex) return BufferIndexOrigin::VertexIndex;
    if (instance) return BufferIndexOrigin::InstanceIndex;
    if (memory) return BufferIndexOrigin::Memory;
    if (userData) return BufferIndexOrigin::UserData;
    return BufferIndexOrigin::Other;
}

void BufferReadAnalysis::Note(BufferReadRanges& ranges, const IrValue& inst, const MemoryInfo& memory) {
    const auto op = inst.Opcode();
    if (BufferAccessOf(op) != BufferAccess::Read || !ranges.bounded) return;
    const auto plainExtent = static_cast<std::uint64_t>(std::max((memory.dataBits + 7u) / 8u, 1u)) * std::max(memory.dataDwords, 1u);
    const auto extent = memory.formatted ? std::max(plainExtent, FormattedElementBytes) : plainExtent;
    if (op == IrOpcode::ReadConstBuffer) {
        const auto offset = inst.ArgumentCount() == 2u ? Of(inst.Argument(1)) : std::nullopt;
        if (!offset || offset->high + memory.offset > Max32) {
            ranges.Unbound(offset ? BufferReadUnbound::Wrap : BufferReadUnbound::ScalarOffset);
            return;
        }
        ranges.Add({0u, 0u, (offset->low + memory.offset) & ~std::uint64_t{3}, ((offset->high + memory.offset) & ~std::uint64_t{3}) + 4u});
        return;
    }
    const bool plain = op == IrOpcode::LoadBufferU8 || op == IrOpcode::LoadBufferU16 || op == IrOpcode::LoadBufferU32 || op == IrOpcode::LoadBufferU32x2 || op == IrOpcode::LoadBufferU32x3 || op == IrOpcode::LoadBufferU32x4;
    if (!plain || inst.ArgumentCount() < 4u) {
        ranges.Unbound(BufferReadUnbound::Opcode);
        return;
    }
    const auto affine = AffineOf(inst.Argument(1));
    const auto offset = Of(inst.Argument(2));
    const auto soffset = Of(inst.Argument(3));
    if (!offset || !soffset || offset->high + memory.offset > Max32) {
        ranges.Unbound(!offset ? BufferReadUnbound::Offset : !soffset ? BufferReadUnbound::Soffset : BufferReadUnbound::Wrap);
        return;
    }
    BufferReadRange range{0u, BufferReadRanges::AnyIndex, offset->low + memory.offset + soffset->low, offset->high + memory.offset + soffset->high + extent};
    if (affine) {
        range.firstIndex = static_cast<std::uint32_t>(affine->low);
        range.lastIndex = static_cast<std::uint32_t>(affine->high);
        range.indexSource = affine->source;
        range.indexScale = static_cast<std::uint32_t>(affine->scale);
        if (affine->source == BufferIndexSource::None && affine->high == BufferReadRanges::AnyIndex) range.indexOrigin = OriginOf(inst.Argument(1));
    } else {
        range.indexOrigin = OriginOf(inst.Argument(1));
    }
    ranges.Add(range);
}

}

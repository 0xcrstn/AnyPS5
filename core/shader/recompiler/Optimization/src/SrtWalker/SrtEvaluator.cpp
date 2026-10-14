#include "Optimization/SrtWalker/SrtEvaluator.hpp"
#include "Optimization/SrtWalker/SrtAddressArithmetic.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ShaderRecompiler::Detail {

namespace {

constexpr std::size_t PooledTables = 8;
constexpr std::size_t FirstTableSlots = 64;

std::vector<EvaluatedValues::Storage>& tablePool() {
    struct TablePoolStorage {};
    return HostThreadLocal<std::vector<EvaluatedValues::Storage>, TablePoolStorage>();
}

}

EvaluatedValues::EvaluatedValues() {
    auto& pool = tablePool();
    if (pool.empty()) {
        return;
    }
    auto storage = std::move(pool.back());
    pool.pop_back();
    _slots = std::move(storage.slots);
    _generation = storage.generation + 1u;
    if (_generation == 0u) {
        std::fill(_slots.begin(), _slots.end(), Slot{});
        _generation = 1u;
    }
}

EvaluatedValues::~EvaluatedValues() {
    if (_slots.empty()) {
        return;
    }
    auto& pool = tablePool();
    if (pool.size() < PooledTables) {
        pool.push_back({std::move(_slots), _generation});
    }
}

void EvaluatedValues::Grow() {
    std::vector<Slot> previous(_slots.empty() ? FirstTableSlots : _slots.size() * 2u);
    previous.swap(_slots);
    const auto generation = _generation;
    _generation = 1u;
    _count = 0;
    for (const auto& slot : previous) {
        if (slot.generation == generation) {
            Insert(slot.key, slot.value);
        }
    }
}

bool Evaluator::Evaluate(IrValue* value, std::uint32_t& result) {
    std::uint64_t wide = 0;
    if (!EvaluateWide(value, wide)) {
        return false;
    }
    result = static_cast<std::uint32_t>(wide);
    return true;
}

bool Evaluator::EvaluateWide(IrValue* raw, std::uint64_t& result) {
    IrValue* value = raw->Resolve();
    if (value->HasImmediate()) {
        switch (value->Type()) {
            case IrType::Bool: result = value->ImmediateBool() ? 1u : 0u; return true;
            case IrType::U8: result = value->ImmediateU8(); return true;
            case IrType::U16: result = value->ImmediateU16(); return true;
            case IrType::U32: result = value->ImmediateU32(); return true;
            case IrType::U64: result = value->ImmediateU64(); return true;
            case IrType::F32: result = Float32Bits(value->ImmediateF32()); return true;
            default: return false;
        }
    }
    if (value->Opcode() == IrOpcode::Void) {
        return false;
    }
    IrValue* inst = value;
    if (_activeMask != nullptr && IsRuntimeSelect(inst->Opcode()) && inst->ArgumentCount() == 3 && inst->Argument(0)->Resolve() == _activeMask) {
        return EvaluateWide(inst->Argument(1), result);
    }
    if (_nested > 0 && _runtime.userDataUse != nullptr && (inst->Opcode() == IrOpcode::LoadAddressU32 || inst->Opcode() == IrOpcode::ReadConstBuffer) && IsRawRead(_program, *inst)) _runtime.userDataUse->operandReads.push_back(inst);
    if (_cache.Find(inst, result)) {
        return true;
    }
    if (std::find(_visiting.begin(), _visiting.end(), inst) != _visiting.end()) {
        return false;
    }
    _visiting.push_back(inst);
    std::uint64_t out = 0;
    const bool forwards = inst->Opcode() == IrOpcode::ReadConst || inst->Opcode() == IrOpcode::BitCastU32F32 || inst->Opcode() == IrOpcode::BitCastF32U32;
    if (!forwards) ++_nested;
    const bool evaluated = EvaluateInst(*inst, out);
    if (!forwards) --_nested;
    _visiting.pop_back();
    if (!evaluated) {
        static const bool debug = std::getenv("APS5_SRT_DEBUG") != nullptr;
        if (debug) std::fprintf(stderr, "[srt] cannot evaluate %s (%zu arguments)\n", std::string(IrOpcodeName(inst->Opcode())).c_str(), inst->ArgumentCount());
        return false;
    }
    _cache.Insert(inst, out);
    ++_counts.values;
    result = out;
    return true;
}

float Evaluator::Float32(std::uint64_t bits) { return std::bit_cast<float>(static_cast<std::uint32_t>(bits)); }

std::uint64_t Evaluator::Float32Bits(float value) { return std::bit_cast<std::uint32_t>(value); }

bool Evaluator::Arg(IrValue& inst, std::size_t index, std::uint64_t& result) { return EvaluateWide(inst.Argument(index), result); }

bool Evaluator::EvaluatePhi(IrValue& inst, std::uint64_t& result) {
    ++_counts.phis;
    IrValue* value = ResolveInvariantPhi(_program, &inst);
    return value != nullptr && EvaluateWide(value, result);
}

bool Evaluator::EvaluateExtract(IrValue& inst, std::uint64_t& result) {
    IrValue* index = inst.Argument(1)->Resolve();
    if (!index->HasImmediate() || index->Type() != IrType::U32) {
        return false;
    }
    const auto component = index->ImmediateU32();
    if (component >= 2u) {
        return false;
    }
    if (inst.Opcode() == IrOpcode::CompositeExtractU64) {
        std::uint64_t packed = 0;
        if (!Arg(inst, 0, packed)) {
            return false;
        }
        result = static_cast<std::uint32_t>(packed >> (component * 32u));
        return true;
    }
    IrValue* source = inst.Argument(0)->Resolve();
    if (source->Opcode() == IrOpcode::Void) {
        return false;
    }
    if (source->Opcode() == IrOpcode::CompositeConstructU32x2) {
        return EvaluateWide(source->Argument(component), result);
    }
    if (source->Opcode() == IrOpcode::IAddCarry32) {
        std::uint64_t lhs = 0;
        std::uint64_t rhs = 0;
        if (!Arg(*source, 0, lhs) || !Arg(*source, 1, rhs)) {
            return false;
        }
        const auto sum = static_cast<std::uint64_t>(static_cast<std::uint32_t>(lhs)) + static_cast<std::uint32_t>(rhs);
        result = component == 0u ? static_cast<std::uint32_t>(sum) : static_cast<std::uint32_t>(sum >> 32u);
        return true;
    }
    return false;
}

bool Evaluator::EvaluateRawRead(IrValue& inst, std::uint64_t& result) {
    const auto flags = inst.Flags<MemoryFlags>();
    if (flags.index >= _program.memoryInfo.size()) {
        return false;
    }
    const auto& mem = _program.memoryInfo[flags.index];
    IrValue* handle = inst.Argument(0)->Resolve();
    if (handle->Opcode() == IrOpcode::Void) {
        return false;
    }
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::uint64_t offset = 0;
    std::int32_t origin = -1;
    const auto baseWord = [&] {
        IrValue* source = handle->Argument(0)->Resolve();
        if (_runtime.userDataUse == nullptr || source->HasImmediate() || source->Opcode() != IrOpcode::GetUserData) return Arg(*handle, 0, low);
        const auto reg = RegIndex(static_cast<ScalarReg>(source->Argument(0)->Register().index));
        if (reg < _program.userDataBase || reg - _program.userDataBase >= _runtime.userData.size() || reg - _program.userDataBase >= SrtUserDataUse::TrackedWords) return Arg(*handle, 0, low);
        origin = static_cast<std::int32_t>(reg - _program.userDataBase);
        low = _runtime.userData[reg - _program.userDataBase];
        _runtime.userDataUse->asBase |= std::uint64_t {1} << origin;
        return true;
    };
    if (!baseWord() || !Arg(*handle, 1, high) || !Arg(inst, 1, offset)) {
        return false;
    }
    const auto base = ((high << 32u) | static_cast<std::uint32_t>(low)) & AddressMask;
    const auto immediate = static_cast<std::int64_t>(static_cast<std::int32_t>(mem.offset));
    std::uint64_t address = 0;
    if (inst.Opcode() == IrOpcode::ReadConstBuffer) {
        std::uint64_t records = 0;
        std::uint64_t word3 = 0;
        if (handle->ArgumentCount() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
            return false;
        }
        if (immediate < 0) {
            return false;
        }
        const auto byteOffset = static_cast<std::uint64_t>(immediate) + static_cast<std::uint32_t>(offset);
        const auto aligned = byteOffset & ~std::uint64_t {3};
        const auto stride = (static_cast<std::uint32_t>(high) >> 16u) & 0x3fffu;
        const auto size = stride == 0u ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(records)) : static_cast<std::uint64_t>(stride) * static_cast<std::uint32_t>(records);
        if (aligned > size || size - aligned < sizeof(std::uint32_t)) {
            result = 0;
            return true;
        }
        address = ((base & ~std::uint64_t {3}) + byteOffset) & ~std::uint64_t {3};
    } else {
        const auto relative = (immediate & ~std::int64_t {3}) + static_cast<std::int64_t>(static_cast<std::uint32_t>(offset) & ~3u);
        if (!AddSignedAddress(base & ~std::uint64_t {3}, relative, address)) {
            return false;
        }
    }
    if (_nullRoots != nullptr && base == 0 && !LoadedFromMemory(*handle)) {
        ++*_nullRoots;
        result = 0;
        return true;
    }
    if (_runtime.userDataUse != nullptr) {
        std::uint64_t baseAddress = 0;
        if (origin < 0) {
            IrValue* source = handle->Argument(0)->Resolve();
            if (source->Opcode() == IrOpcode::ReadConst && source->ArgumentCount() >= 2 && source->Argument(1) != nullptr) {
                const IrValue* slot = source->Argument(1)->Resolve();
                if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < _program.srtReads.size() && _program.srtReads[slot->ImmediateU32()].value != nullptr) source = _program.srtReads[slot->ImmediateU32()].value->Resolve();
            }
            if (!source->HasImmediate() && IsRawRead(_program, *source)) {
                const auto& instructions = _runtime.userDataUse->readInstructions;
                for (auto index = instructions.size(); index-- > 0;) {
                    if (instructions[index] != source) continue;
                    baseAddress = _runtime.userDataUse->reads[index].first;
                    break;
                }
            }
        }
        _runtime.userDataUse->reads.emplace_back(address, origin);
        _runtime.userDataUse->readBases.push_back(baseAddress);
        _runtime.userDataUse->readInstructions.push_back(&inst);
    }
    if (_inaccessible != nullptr && _runtime.accessible != nullptr && !_runtime.accessible(_runtime.userContext, address, sizeof(std::uint32_t)) && LoadedFromMemory(*handle)) {
        *_inaccessible = {&inst, address};
        return false;
    }
    if (auto* trace = _runtime.readTrace; trace != nullptr) {
        if (&inst == trace->leaf) trace->leaves.emplace_back(trace->leafSlot, address);
        else trace->otherReads.push_back(address);
    }
    std::uint32_t word = 0;
    if (_runtime.readMemory != nullptr) {
        if (!_runtime.readMemory(_runtime.userContext, address, &word)) {
            return false;
        }
    } else {
        std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
    }
    result = word;
    return true;
}

namespace {

std::optional<std::size_t> evaluatedArguments(IrOpcode opcode) {
    switch (opcode) {
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32:
        case IrOpcode::ConvertF32U32:
        case IrOpcode::ConvertU32F32:
        case IrOpcode::FPTrunc32:
        case IrOpcode::FPIsNan32:
        case IrOpcode::BitwiseNot32:
        case IrOpcode::LogicalNot: return 1u;
        case IrOpcode::CompositeConstructU64:
        case IrOpcode::IAdd32:
        case IrOpcode::IAdd64:
        case IrOpcode::ISub32:
        case IrOpcode::ISub64:
        case IrOpcode::IMul32:
        case IrOpcode::IMul64:
        case IrOpcode::SMin32:
        case IrOpcode::UMin32:
        case IrOpcode::FPMul32:
        case IrOpcode::FPOrdLessThanEqual32:
        case IrOpcode::FPOrdGreaterThanEqual32:
        case IrOpcode::BitwiseAnd32:
        case IrOpcode::BitwiseAnd64:
        case IrOpcode::BitwiseOr32:
        case IrOpcode::BitwiseXor32:
        case IrOpcode::ShiftLeftLogical32:
        case IrOpcode::ShiftLeftLogical64:
        case IrOpcode::ShiftRightLogical32:
        case IrOpcode::ShiftRightLogical64:
        case IrOpcode::ShiftRightArithmetic32:
        case IrOpcode::ShiftRightArithmetic64:
        case IrOpcode::IEqual32:
        case IrOpcode::INotEqual32:
        case IrOpcode::SLessThan32:
        case IrOpcode::ULessThan32:
        case IrOpcode::UGreaterThan32:
        case IrOpcode::LogicalAnd:
        case IrOpcode::LogicalOr:
        case IrOpcode::LogicalXor: return 2u;
        case IrOpcode::BitFieldUExtract:
        case IrOpcode::BitFieldSExtract:
        case IrOpcode::SelectU32:
        case IrOpcode::SelectU1:
        case IrOpcode::SelectF32: return 3u;
        case IrOpcode::BitFieldInsert: return 4u;
        default: return std::nullopt;
    }
}

std::optional<bool> evaluableAlone(const IrResourcePlan& program, const IrValue& value) {
    if (value.HasImmediate()) {
        switch (value.Type()) {
            case IrType::Bool:
            case IrType::U8:
            case IrType::U16:
            case IrType::U32:
            case IrType::U64:
            case IrType::F32: return true;
            default: return false;
        }
    }
    switch (value.Opcode()) {
        case IrOpcode::GetUserData:
        case IrOpcode::GetShaderBase:
        case IrOpcode::ReadFirstLane:
        case IrOpcode::ReadConst: return true;
        case IrOpcode::LoadAddressU32:
        case IrOpcode::ReadConstBuffer: return IsRawRead(program, value);
        case IrOpcode::Phi: return std::nullopt;
        case IrOpcode::CompositeExtractU64:
        case IrOpcode::CompositeExtractU32x2: {
            if (value.ArgumentCount() < 2) return true;
            const auto* index = value.Argument(1) != nullptr ? value.Argument(1)->Resolve() : nullptr;
            if (index == nullptr || !index->HasImmediate() || index->Type() != IrType::U32 || index->ImmediateU32() >= 2u) return false;
            if (value.Opcode() == IrOpcode::CompositeExtractU64) return std::nullopt;
            const auto* source = value.Argument(0) != nullptr ? value.Argument(0)->Resolve() : nullptr;
            if (source == nullptr || (source->Opcode() != IrOpcode::CompositeConstructU32x2 && source->Opcode() != IrOpcode::IAddCarry32)) return false;
            return std::nullopt;
        }
        default: break;
    }
    const auto arguments = evaluatedArguments(value.Opcode());
    if (!arguments) return false;
    if (value.ArgumentCount() < *arguments) return true;
    return std::nullopt;
}

}

namespace {

float OperationFloat(std::uint64_t bits) { return std::bit_cast<float>(static_cast<std::uint32_t>(bits)); }

std::uint64_t OperationFloatBits(float value) { return std::bit_cast<std::uint32_t>(value); }

}

bool ApplySrtOperation(IrOpcode opcode, const std::array<std::uint64_t, 4>& values, std::uint64_t& result) {
    const auto a = values[0];
    const auto b = values[1];
    const auto c = values[2];
    const auto d = values[3];
    switch (opcode) {
        case IrOpcode::CompositeConstructU64:
            result = static_cast<std::uint32_t>(a) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b)) << 32u);
            return true;
        case IrOpcode::IAdd32:
            {
                result = static_cast<std::uint32_t>(a + b);
                return true;
            }
            return false;
        case IrOpcode::IAdd64:
            {
                result = a + b;
                return true;
            }
            return false;
        case IrOpcode::ISub32:
            {
                result = static_cast<std::uint32_t>(a - b);
                return true;
            }
            return false;
        case IrOpcode::ISub64:
            {
                result = a - b;
                return true;
            }
            return false;
        case IrOpcode::IMul32:
            {
                result = static_cast<std::uint32_t>(a * b);
                return true;
            }
            return false;
        case IrOpcode::IMul64:
            {
                result = a * b;
                return true;
            }
            return false;
        case IrOpcode::SMin32:
            {
                result = static_cast<std::uint32_t>(std::min(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)), std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(b))));
                return true;
            }
            return false;
        case IrOpcode::UMin32:
            {
                result = std::min(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
                return true;
            }
            return false;
        case IrOpcode::ConvertF32U32:
            {
                result = OperationFloatBits(static_cast<float>(static_cast<std::uint32_t>(a)));
                return true;
            }
            return false;
        case IrOpcode::ConvertU32F32:
            {
                const auto value = OperationFloat(a);
                if (!std::isfinite(value) || value < 0.0f || static_cast<double>(value) > 4294967295.0) {
                    return false;
                }
                result = static_cast<std::uint32_t>(value);
                return true;
            }
            return false;
        case IrOpcode::FPMul32:
            {
                result = OperationFloatBits(OperationFloat(a) * OperationFloat(b));
                return true;
            }
            return false;
        case IrOpcode::FPTrunc32:
            {
                result = OperationFloatBits(std::trunc(OperationFloat(a)));
                return true;
            }
            return false;
        case IrOpcode::FPIsNan32:
            {
                result = std::isnan(OperationFloat(a)) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::FPOrdLessThanEqual32:
            {
                result = OperationFloat(a) <= OperationFloat(b) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::FPOrdGreaterThanEqual32:
            {
                result = OperationFloat(a) >= OperationFloat(b) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::BitwiseAnd32:
            {
                result = static_cast<std::uint32_t>(a & b);
                return true;
            }
            return false;
        case IrOpcode::BitwiseAnd64:
            {
                result = a & b;
                return true;
            }
            return false;
        case IrOpcode::BitwiseOr32:
            {
                result = static_cast<std::uint32_t>(a | b);
                return true;
            }
            return false;
        case IrOpcode::BitwiseXor32:
            {
                result = static_cast<std::uint32_t>(a ^ b);
                return true;
            }
            return false;
        case IrOpcode::BitwiseNot32:
            {
                result = ~static_cast<std::uint32_t>(a);
                return true;
            }
            return false;
        case IrOpcode::ShiftLeftLogical32:
            {
                result = static_cast<std::uint32_t>(a) << (b & 31u);
                return true;
            }
            return false;
        case IrOpcode::ShiftLeftLogical64:
            {
                result = a << (b & 63u);
                return true;
            }
            return false;
        case IrOpcode::ShiftRightLogical32:
            {
                result = static_cast<std::uint32_t>(a) >> (b & 31u);
                return true;
            }
            return false;
        case IrOpcode::ShiftRightLogical64:
            {
                result = a >> (b & 63u);
                return true;
            }
            return false;
        case IrOpcode::ShiftRightArithmetic32:
            {
                result = static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)) >> (b & 31u));
                return true;
            }
            return false;
        case IrOpcode::ShiftRightArithmetic64:
            {
                result = static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(a) >> (b & 63u));
                return true;
            }
            return false;
        case IrOpcode::BitFieldUExtract:
            {
                const auto offset = static_cast<std::uint32_t>(b);
                const auto width = static_cast<std::uint32_t>(c);
                if (offset > 32u || width > 32u - offset) {
                    return false;
                }
                const auto mask = width == 32u ? 0xffffffffu : width == 0u ? 0u : (std::uint32_t {1} << width) - 1u;
                result = width == 0u ? 0u : (static_cast<std::uint32_t>(a) >> offset) & mask;
                return true;
            }
            return false;
        case IrOpcode::BitFieldSExtract:
            {
                const auto offset = static_cast<std::uint32_t>(b);
                const auto width = static_cast<std::uint32_t>(c);
                if (offset > 32u || width > 32u - offset) {
                    return false;
                }
                if (width == 0u) {
                    result = 0;
                    return true;
                }
                const auto mask = width == 32u ? 0xffffffffu : (std::uint32_t {1} << width) - 1u;
                auto bits = (static_cast<std::uint32_t>(a) >> offset) & mask;
                if (width < 32u && (bits & (std::uint32_t {1} << (width - 1u))) != 0u) {
                    bits |= ~mask;
                }
                result = bits;
                return true;
            }
            return false;
        case IrOpcode::BitFieldInsert: {
            const auto offset = static_cast<std::uint32_t>(c);
            const auto width = static_cast<std::uint32_t>(d);
            if (offset > 32u || width > 32u - offset) {
                return false;
            }
            if (width == 0u) {
                result = static_cast<std::uint32_t>(a);
                return true;
            }
            const auto mask = width == 32u ? 0xffffffffu : ((std::uint32_t {1} << width) - 1u) << offset;
            result = (static_cast<std::uint32_t>(a) & ~mask) | ((static_cast<std::uint32_t>(b) << offset) & mask);
            return true;
        }
        case IrOpcode::SelectU32:
        case IrOpcode::SelectU1:
        case IrOpcode::SelectF32:
            {
                result = a != 0u ? b : c;
                return true;
            }
            return false;
        case IrOpcode::IEqual32:
            {
                result = static_cast<std::uint32_t>(a) == static_cast<std::uint32_t>(b) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::INotEqual32:
            {
                result = static_cast<std::uint32_t>(a) != static_cast<std::uint32_t>(b) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::SLessThan32:
            {
                result = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(a)) < std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(b)) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::ULessThan32:
            {
                result = static_cast<std::uint32_t>(a) < static_cast<std::uint32_t>(b) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::UGreaterThan32:
            {
                result = static_cast<std::uint32_t>(a) > static_cast<std::uint32_t>(b) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::LogicalAnd:
            {
                result = (a != 0u) && (b != 0u) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::LogicalOr:
            {
                result = (a != 0u) || (b != 0u) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::LogicalXor:
            {
                result = (a != 0u) != (b != 0u) ? 1u : 0u;
                return true;
            }
            return false;
        case IrOpcode::LogicalNot:
            {
                result = a == 0u ? 1u : 0u;
                return true;
            }
            return false;
        default: break;
    }
    return false;
}

std::optional<std::size_t> SrtOperationArguments(IrOpcode opcode) {
    if (opcode == IrOpcode::BitCastU32F32 || opcode == IrOpcode::BitCastF32U32) return std::nullopt;
    return evaluatedArguments(opcode);
}

bool Evaluator::MayEvaluate(const IrResourcePlan& program, IrValue* value) {
    std::unordered_map<const IrValue*, bool> known;
    const auto requirements = [&program](IrValue* current) {
        std::vector<IrValue*> required;
        if (current->IsPhi()) {
            required.push_back(ResolveInvariantPhi(program, current));
            return required;
        }
        const auto resolved = [](IrValue* value) { return value != nullptr ? value->Resolve() : nullptr; };
        if (current->Opcode() == IrOpcode::CompositeExtractU64) {
            required.push_back(resolved(current->Argument(0)));
            return required;
        }
        if (current->Opcode() == IrOpcode::CompositeExtractU32x2) {
            auto* source = current->Argument(0)->Resolve();
            if (source->Opcode() == IrOpcode::CompositeConstructU32x2) {
                required.push_back(resolved(source->Argument(current->Argument(1)->Resolve()->ImmediateU32())));
            } else {
                required.push_back(resolved(source->Argument(0)));
                required.push_back(resolved(source->Argument(1)));
            }
            return required;
        }
        const auto arguments = *evaluatedArguments(current->Opcode());
        for (std::size_t index = 0; index < arguments; ++index) {
            auto* argument = current->Argument(index);
            required.push_back(argument != nullptr ? argument->Resolve() : nullptr);
        }
        return required;
    };
    std::vector<std::pair<IrValue*, bool>> pending{{value->Resolve(), false}};
    while (!pending.empty()) {
        const auto [current, expanded] = pending.back();
        pending.pop_back();
        if (known.contains(current)) continue;
        if (const auto alone = evaluableAlone(program, *current)) {
            known.emplace(current, *alone);
            continue;
        }
        const auto required = requirements(current);
        if (!expanded) {
            pending.emplace_back(current, true);
            for (auto* argument : required) {
                if (argument != nullptr) pending.emplace_back(argument, false);
            }
            continue;
        }
        known.emplace(current, std::ranges::all_of(required, [&known](const IrValue* argument) { return argument != nullptr && known.at(argument); }));
    }
    return known.at(value->Resolve());
}

bool Evaluator::EvaluateInst(IrValue& inst, std::uint64_t& result) {
    if (const auto count = SrtOperationArguments(inst.Opcode())) {
        std::array<std::uint64_t, 4> values{};
        for (std::size_t index = 0; index < *count; ++index) {
            if (!Arg(inst, index, values[index])) {
                return false;
            }
        }
        return ApplySrtOperation(inst.Opcode(), values, result);
    }
    switch (inst.Opcode()) {
        case IrOpcode::GetUserData: {
            const auto reg = RegIndex(static_cast<ScalarReg>(inst.Argument(0)->Register().index));
            if (reg < _program.userDataBase || reg - _program.userDataBase >= _runtime.userData.size()) {
                return false;
            }
            if (_runtime.userDataUse != nullptr) _runtime.userDataUse->asValue |= reg - _program.userDataBase < SrtUserDataUse::TrackedWords ? std::uint64_t {1} << (reg - _program.userDataBase) : ~std::uint64_t {0};
            result = _runtime.userData[reg - _program.userDataBase];
            return true;
        }
        case IrOpcode::GetShaderBase: result = _runtime.shaderBase; return true;
        case IrOpcode::Phi: return EvaluatePhi(inst, result);
        case IrOpcode::ReadFirstLane: {
            Evaluator active(_program, _runtime, _cleanFlatSlots, _cleanEvaluator, inst.Argument(1));
            active._nested = _nested;
            active.ReportInaccessibleReads(_inaccessible);
            active.ReportNullRootReads(_nullRoots);
            const bool evaluated = active.EvaluateWide(inst.Argument(0), result);
            _counts.values += active._counts.values;
            _counts.phis += active._counts.phis;
            _counts.laneEvaluators += active._counts.laneEvaluators + 1u;
            return evaluated;
        }
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32: return Arg(inst, 0, result);
        case IrOpcode::CompositeExtractU64:
        case IrOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result);
        case IrOpcode::ReadConst: {
            IrValue* slot = inst.Argument(1)->Resolve();
            if (!slot->HasImmediate() || slot->Type() != IrType::U32 || slot->ImmediateU32() >= _program.srtReads.size()) {
                return false;
            }
            if (slot->ImmediateU32() < _cleanFlatSlots.size() && _cleanFlatSlots[slot->ImmediateU32()] != 0u && _cleanEvaluator != nullptr) {
                _cleanEvaluator->_nested += _nested;
                const bool clean = _cleanEvaluator->EvaluateWide(_program.srtReads[slot->ImmediateU32()].value, result);
                _cleanEvaluator->_nested -= _nested;
                return clean;
            }
            return EvaluateWide(_program.srtReads[slot->ImmediateU32()].value, result);
        }
        case IrOpcode::LoadAddressU32:
        case IrOpcode::ReadConstBuffer:
            if (IsRawRead(_program, inst)) {
                return EvaluateRawRead(inst, result);
            }
            break;
        case IrOpcode::UndefU1:
        case IrOpcode::UndefU8:
        case IrOpcode::UndefU16:
        case IrOpcode::UndefU32:
        case IrOpcode::UndefU64: return false;
        default: break;
    }
    return false;
}

}

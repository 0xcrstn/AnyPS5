#include "Optimization/DenormalFreeProof.hpp"
#include <algorithm>
#include <deque>
#include <vector>

namespace ShaderRecompiler {

namespace {

bool isImmediate(const IrValue* value, std::uint32_t bits) {
    return value->HasImmediate() && value->ImmediateU32() == bits;
}

const IrValue* nanTestedValue(const IrValue* condition) {
    if (condition->Opcode() != IrOpcode::UGreaterThan32 || condition->ArgumentCount() != 2u || !isImmediate(condition->Argument(1)->Resolve(), 0x7f800000u)) return nullptr;
    return MaskedOperand(condition->Argument(0)->Resolve(), IrOpcode::BitwiseAnd32, 0x7fffffffu);
}

bool nanOrQuietedNan(const IrValue* arm, const IrValue* tested) {
    return arm == tested || MaskedOperand(arm, IrOpcode::BitwiseOr32, 0x00400000u) == tested;
}

bool unitOrLarger(const IrValue* value) {
    return value->HasImmediate() && (value->ImmediateU32() & 0x7f800000u) >= 0x3f800000u;
}

bool mayBeDenormalFree(const IrValue& value) {
    if (value.HasImmediate() || DenormalFlushSource(value) != nullptr) return true;
    switch (value.Opcode()) {
        case IrOpcode::ConvertIToF32:
        case IrOpcode::ConvertUToF32:
        case IrOpcode::ConvertF32S32:
        case IrOpcode::ConvertF32U32:
        case IrOpcode::ConvertF32F16:
        case IrOpcode::FPFloor32:
        case IrOpcode::FPCeil32:
        case IrOpcode::FPTrunc32:
        case IrOpcode::FPRoundEven32:
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32:
        case IrOpcode::FPAbs32:
        case IrOpcode::FPNeg32:
        case IrOpcode::FPSaturate32:
        case IrOpcode::FPSqrt:
        case IrOpcode::FPRecipSqrt32:
        case IrOpcode::FPMul32:
        case IrOpcode::BitwiseXor32:
        case IrOpcode::BitwiseAnd32:
        case IrOpcode::SelectU32:
        case IrOpcode::SelectF32:
        case IrOpcode::FPMin32:
        case IrOpcode::FPMax32:
        case IrOpcode::FPMinTri32:
        case IrOpcode::FPMaxTri32:
        case IrOpcode::FPMedTri32:
        case IrOpcode::Phi:
            return true;
        default:
            return false;
    }
}

}

const IrValue* MaskedOperand(const IrValue* value, IrOpcode opcode, std::uint32_t bits) {
    if (value->Opcode() != opcode || value->ArgumentCount() != 2u) return nullptr;
    const IrValue* lhs = value->Argument(0)->Resolve();
    const IrValue* rhs = value->Argument(1)->Resolve();
    if (isImmediate(rhs, bits)) return lhs;
    if (isImmediate(lhs, bits)) return rhs;
    return nullptr;
}

IrValue* DenormalFlushSource(const IrValue& value) {
    if (value.Opcode() != IrOpcode::SelectU32 || value.ArgumentCount() != 3u) return nullptr;
    IrValue* source = value.Argument(2)->Resolve();
    const IrValue* exponent = MaskedOperand(value.Argument(0)->Resolve(), IrOpcode::IEqual32, 0u);
    if (exponent == nullptr || MaskedOperand(exponent, IrOpcode::BitwiseAnd32, 0x7f800000u) != source) return nullptr;
    return MaskedOperand(value.Argument(1)->Resolve(), IrOpcode::BitwiseAnd32, 0x80000000u) == source ? source : nullptr;
}

bool DenormalFreeConstant(std::uint32_t bits) {
    return (bits & 0x7f800000u) != 0u || (bits & 0x7fffffffu) == 0u;
}

DenormalFreeProof::DenormalFreeProof(IrProgram& program) {
    std::size_t bound = 0;
    for (auto* block : program.BlockOrder()) {
        for (auto* inst : block->Instructions()) bound = std::max(bound, static_cast<std::size_t>(inst->Id()) + 1u);
    }
    state.resize(bound);
    std::deque<const IrValue*> pending;
    for (auto* block : program.BlockOrder()) {
        for (auto* inst : block->Instructions()) {
            if (inst->Resolve() != inst) continue;
            if (mayBeDenormalFree(*inst)) {
                state[inst->Id()] = {inst, State::Free};
                pending.push_back(inst);
            } else {
                state[inst->Id()] = {inst, State::NotFree};
            }
        }
    }
    std::vector<const IrValue*> users;
    while (!pending.empty()) {
        const IrValue* value = pending.front();
        pending.pop_front();
        if (stateOf(value) != State::Free || evaluate(*value)) continue;
        state[value->Id()].state = State::NotFree;
        users.assign(value->Uses().begin(), value->Uses().end());
        while (!users.empty()) {
            const IrValue* user = users.back();
            users.pop_back();
            if (user->Resolve() != user) {
                users.insert(users.end(), user->Uses().begin(), user->Uses().end());
                continue;
            }
            if (stateOf(user) == State::Free) pending.push_back(user);
        }
    }
}

DenormalFreeProof::State DenormalFreeProof::stateOf(const IrValue* value) const {
    const auto id = value->Id();
    return id < state.size() && state[id].value == value ? state[id].state : State::Untracked;
}

bool DenormalFreeProof::Contains(const IrValue* value) const {
    value = value->Resolve();
    if (value->HasImmediate()) return DenormalFreeConstant(value->ImmediateU32());
    return stateOf(value) == State::Free;
}

bool DenormalFreeProof::evaluate(const IrValue& value) const {
    if (value.HasImmediate()) return DenormalFreeConstant(value.ImmediateU32());
    if (DenormalFlushSource(value) != nullptr) return true;
    switch (value.Opcode()) {
        case IrOpcode::ConvertIToF32:
        case IrOpcode::ConvertUToF32:
        case IrOpcode::ConvertF32S32:
        case IrOpcode::ConvertF32U32:
        case IrOpcode::ConvertF32F16:
        case IrOpcode::FPFloor32:
        case IrOpcode::FPCeil32:
        case IrOpcode::FPTrunc32:
        case IrOpcode::FPRoundEven32:
            return true;
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32:
        case IrOpcode::FPAbs32:
        case IrOpcode::FPNeg32:
        case IrOpcode::FPSaturate32:
        case IrOpcode::FPSqrt:
        case IrOpcode::FPRecipSqrt32:
            return Contains(value.Argument(0));
        case IrOpcode::FPMul32: {
            const IrValue* lhs = value.Argument(0)->Resolve();
            const IrValue* rhs = value.Argument(1)->Resolve();
            return (unitOrLarger(lhs) && Contains(rhs)) || (unitOrLarger(rhs) && Contains(lhs));
        }
        case IrOpcode::BitwiseXor32:
            if (const IrValue* operand = MaskedOperand(&value, IrOpcode::BitwiseXor32, 0x80000000u)) return Contains(operand);
            return false;
        case IrOpcode::BitwiseAnd32:
            if (const IrValue* operand = MaskedOperand(&value, IrOpcode::BitwiseAnd32, 0x7fffffffu)) return Contains(operand);
            return false;
        case IrOpcode::SelectU32:
        case IrOpcode::SelectF32: {
            const IrValue* tested = nanTestedValue(value.Argument(0)->Resolve());
            if (tested != nullptr && nanOrQuietedNan(value.Argument(1)->Resolve(), tested)) return Contains(value.Argument(2));
            return Contains(value.Argument(1)) && Contains(value.Argument(2));
        }
        case IrOpcode::FPMin32:
        case IrOpcode::FPMax32:
        case IrOpcode::FPMinTri32:
        case IrOpcode::FPMaxTri32:
        case IrOpcode::FPMedTri32:
        case IrOpcode::Phi:
            for (std::size_t index = 0; index < value.ArgumentCount(); ++index) {
                if (!Contains(value.Argument(index))) return false;
            }
            return true;
        default:
            return false;
    }
}

}

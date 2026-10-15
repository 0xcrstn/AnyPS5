#include "Optimization/DenormalFlushEliminator.hpp"
#include "Optimization/DenormalFreeProof.hpp"
#include "BdaAbi.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ShaderRecompiler {

namespace {

class DenormalFreeValues {
public:
    bool Contains(const IrValue* value) {
        value = value->Resolve();
        if (const auto found = known.find(value); found != known.end()) return found->second;
        known.emplace(value, false);
        const bool result = compute(*value);
        known[value] = result;
        return result;
    }

private:
    bool compute(const IrValue& value) {
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
                return true;
            case IrOpcode::BitCastU32F32:
            case IrOpcode::BitCastF32U32:
            case IrOpcode::FPAbs32:
            case IrOpcode::FPNeg32:
            case IrOpcode::FPSaturate32:
                return Contains(value.Argument(0));
            case IrOpcode::BitwiseXor32:
                if (const IrValue* operand = MaskedOperand(&value, IrOpcode::BitwiseXor32, 0x80000000u)) return Contains(operand);
                return false;
            case IrOpcode::BitwiseAnd32:
                if (const IrValue* operand = MaskedOperand(&value, IrOpcode::BitwiseAnd32, 0x7fffffffu)) return Contains(operand);
                return false;
            case IrOpcode::SelectU32:
            case IrOpcode::SelectF32:
                return Contains(value.Argument(1)) && Contains(value.Argument(2));
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

    std::unordered_map<const IrValue*, bool> known;
};

}

DenormalFlushEliminationStats DenormalFlushEliminator::Eliminate(IrProgram& program) const {
    return Eliminate(program, FlushProofsSetting());
}

DenormalFlushEliminationStats DenormalFlushEliminator::Eliminate(IrProgram& program, RewriteMode proofs) const {
    DenormalFlushEliminationStats stats;
    if (proofs == RewriteMode::Off) {
        DenormalFreeValues denormalFree;
        for (auto* block : program.BlockOrder()) {
            for (auto* inst : block->Instructions()) {
                if (!inst->HasUses()) continue;
                IrValue* source = DenormalFlushSource(*inst);
                if (source == nullptr || !denormalFree.Contains(source)) continue;
                inst->ReplaceAllUsesWith(source);
                ++stats.removedFlushes;
            }
        }
        return stats;
    }
    const DenormalFreeProof proven(program);
    std::vector<std::pair<IrBlock*, IrValue*>> checks;
    for (auto* block : program.BlockOrder()) {
        for (auto* inst : block->Instructions()) {
            if (!inst->HasUses()) continue;
            IrValue* source = DenormalFlushSource(*inst);
            if (source == nullptr || !proven.Contains(source)) continue;
            if (proofs == RewriteMode::Verify) {
                checks.emplace_back(block, inst);
                continue;
            }
            inst->ReplaceAllUsesWith(source);
            ++stats.removedFlushes;
        }
    }
    IrBuilder builder(program);
    for (const auto& [block, inst] : checks) {
        auto& check = program.CreateValue(IrOpcode::CheckDenormalFree, IrType::Void);
        check.AddArgument(DenormalFlushSource(*inst));
        check.AddArgument(&builder.Constant(static_cast<std::uint32_t>(BdaAbi::FaultReason::FlushProofMismatch)));
        block->InsertInstructionBefore(inst, &check);
        ++stats.checkedFlushes;
    }
    if (stats.checkedFlushes != 0u) program.Info().usesFaultBuffer = true;
    return stats;
}

}

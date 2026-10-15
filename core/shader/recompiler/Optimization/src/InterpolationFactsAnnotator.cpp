#include "Optimization/InterpolationFactsAnnotator.hpp"
#include "Optimization/DenormalFreeProof.hpp"
#include "BdaAbi.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Recompiler.hpp"
#include <stdexcept>
#include <vector>

namespace ShaderRecompiler {

InterpolationFactsStats InterpolationFactsAnnotator::Annotate(IrProgram& program) const {
    return Annotate(program, InterpolationFactsSetting());
}

InterpolationFactsStats InterpolationFactsAnnotator::Annotate(IrProgram& program, RewriteMode mode) const {
    InterpolationFactsStats stats;
    if (mode == RewriteMode::Off) return stats;
    std::vector<std::pair<IrBlock*, IrValue*>> operations;
    for (auto* block : program.BlockOrder()) {
        for (auto* inst : block->Instructions()) {
            if (inst->Opcode() == IrOpcode::FPInterpolateF32 || inst->Opcode() == IrOpcode::FPInterpolateF16) operations.emplace_back(block, inst);
        }
    }
    if (operations.empty()) return stats;
    const DenormalFreeProof proof(program);
    IrBuilder builder(program);
    for (const auto& [block, inst] : operations) {
        const IrValue* modeValue = inst->Argument(3)->Resolve();
        if (!modeValue->HasImmediate()) throw std::runtime_error("an interpolation mode is not an immediate");
        const std::uint32_t current = modeValue->ImmediateU32();
        if ((current & (InterpolationFlushedExact | (7u << InterpolationNormalShift))) != 0u) throw std::runtime_error("an interpolation already carries input facts");
        std::uint32_t facts = 0u;
        for (std::uint32_t input = 0; input < 3u; ++input) {
            IrValue* operand = inst->Argument(input)->Resolve();
            if (!proof.Contains(operand)) continue;
            if (mode == RewriteMode::On) {
                facts |= 1u << (InterpolationNormalShift + input);
                ++stats.provenInputs;
                continue;
            }
            if (operand->HasImmediate()) continue;
            auto& bits = program.CreateValue(IrOpcode::BitCastU32F32, IrType::U32);
            bits.AddArgument(operand);
            block->InsertInstructionBefore(inst, &bits);
            auto& check = program.CreateValue(IrOpcode::CheckDenormalFree, IrType::Void);
            check.AddArgument(&bits);
            check.AddArgument(&builder.Constant(static_cast<std::uint32_t>(BdaAbi::FaultReason::InterpolationFactMismatch)));
            block->InsertInstructionBefore(inst, &check);
            ++stats.checkedInputs;
        }
        if (mode != RewriteMode::On) continue;
        inst->ReplaceArgument(3, &builder.Constant(current | InterpolationFlushedExact | facts));
        ++stats.specializedOperations;
    }
    if (stats.checkedInputs != 0u) program.Info().usesFaultBuffer = true;
    return stats;
}

}

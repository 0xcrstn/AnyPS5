#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/DenormalFlushEliminator.hpp"
#include "Optimization/InterpolationFactsAnnotator.hpp"
#include "Recompiler.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>
#include <utility>

using namespace ShaderRecompiler;

namespace {

struct Builder {
    IrProgram program;
    IrBlock* block = nullptr;

    Builder() {
        block = &newBlock();
        program.SetEntryBlock(*block);
    }

    IrBlock& newBlock() {
        auto& created = program.CreateBlock();
        program.BlockOrder().push_back(&created);
        return created;
    }

    IrValue& constant(std::uint32_t value) {
        auto& created = program.CreateValue(IrOpcode::Void, IrType::U32);
        created.SetImmediateU32(value);
        return created;
    }

    IrValue& emit(IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
        auto& created = program.CreateValue(opcode, type);
        for (auto* argument : arguments) created.AddArgument(argument);
        block->AppendInstruction(&created);
        return created;
    }

    IrValue& unknown() {
        return emit(IrOpcode::LaneId, IrType::U32, {});
    }

    IrValue& flush(IrValue& bits) {
        auto& exponent = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &constant(0x7f800000u)});
        auto& denormal = emit(IrOpcode::IEqual32, IrType::Bool, {&exponent, &constant(0u)});
        auto& sign = emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &constant(0x80000000u)});
        return emit(IrOpcode::SelectU32, IrType::U32, {&denormal, &sign, &bits});
    }

    IrValue& phi(IrBlock& block, std::initializer_list<std::pair<IrBlock*, IrValue*>> incoming) {
        auto& created = program.CreateValue(IrOpcode::Phi, IrType::U32);
        block.AppendInstruction(&created);
        for (const auto& [predecessor, value] : incoming) created.AddPhiOperand(predecessor, value);
        return created;
    }

    void keep(IrValue& value) {
        (void)emit(IrOpcode::ReferenceU32, IrType::Void, {&value});
    }

    std::uint32_t eliminate() {
        return DenormalFlushEliminator{}.Eliminate(program, RewriteMode::Off).removedFlushes;
    }

    DenormalFlushEliminationStats eliminate(RewriteMode proofs) {
        return DenormalFlushEliminator{}.Eliminate(program, proofs);
    }

    IrValue& asFloat(IrValue& bits) {
        return emit(IrOpcode::BitCastF32U32, IrType::F32, {&bits});
    }

    IrValue& asBits(IrValue& value) {
        return emit(IrOpcode::BitCastU32F32, IrType::U32, {&value});
    }

    IrValue& isNan(IrValue& bits) {
        return emit(IrOpcode::UGreaterThan32, IrType::Bool, {&emit(IrOpcode::BitwiseAnd32, IrType::U32, {&bits, &constant(0x7fffffffu)}), &constant(0x7f800000u)});
    }
};

bool expect(const char* name, bool condition) {
    if (!condition) std::fprintf(stderr, "%s\n", name);
    return condition;
}

bool run(const char* name, const std::function<bool()>& test) {
    try {
        return expect(name, test());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
        return false;
    }
}

}

int main() {
    bool passed = true;

    passed &= run("a flush of a flushed value is removed and the first flush stays", [] {
        Builder b;
        auto& first = b.flush(b.unknown());
        auto& second = b.flush(first);
        b.keep(second);
        return b.eliminate() == 1u && !second.HasUses() && first.HasUses();
    });

    passed &= run("a flush of an unknown value stays", [] {
        Builder b;
        auto& flushed = b.flush(b.unknown());
        b.keep(flushed);
        return b.eliminate() == 0u && flushed.HasUses();
    });

    passed &= run("a sign change or absolute value of a flushed value needs no flush", [] {
        Builder b;
        auto& first = b.flush(b.unknown());
        auto& negated = b.flush(b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&first, &b.constant(0x80000000u)}));
        auto& absolute = b.flush(b.emit(IrOpcode::BitwiseAnd32, IrType::U32, {&b.constant(0x7fffffffu), &first}));
        b.keep(negated);
        b.keep(absolute);
        return b.eliminate() == 2u && !negated.HasUses() && !absolute.HasUses();
    });

    passed &= run("a select of flushed values needs no flush, one with an unknown arm does", [] {
        Builder b;
        auto& condition = b.emit(IrOpcode::ULessThan32, IrType::Bool, {&b.unknown(), &b.constant(16u)});
        auto& both = b.flush(b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &b.flush(b.unknown()), &b.flush(b.unknown())}));
        auto& mixed = b.flush(b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &b.flush(b.unknown()), &b.unknown()}));
        b.keep(both);
        b.keep(mixed);
        return b.eliminate() == 1u && !both.HasUses() && mixed.HasUses();
    });

    passed &= run("normal and zero constants need no flush, a denormal constant does", [] {
        Builder b;
        auto& one = b.flush(b.constant(0x3f800000u));
        auto& negativeZero = b.flush(b.constant(0x80000000u));
        auto& denormal = b.flush(b.constant(0x00000001u));
        b.keep(one);
        b.keep(negativeZero);
        b.keep(denormal);
        return b.eliminate() == 2u && !one.HasUses() && !negativeZero.HasUses() && denormal.HasUses();
    });

    passed &= run("an integer or f16 converted to f32 needs no flush", [] {
        Builder b;
        auto& integer = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::ConvertF32U32, IrType::F32, {&b.unknown()})}));
        auto& half = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::ConvertF32F16, IrType::F32, {&b.emit(IrOpcode::BitCastF16U16, IrType::F16, {&b.emit(IrOpcode::ConvertU16U32, IrType::U16, {&b.unknown()})})})}));
        b.keep(integer);
        b.keep(half);
        return b.eliminate() == 2u && !integer.HasUses() && !half.HasUses();
    });

    passed &= run("a clamped flushed value needs no flush, an arithmetic result does", [] {
        Builder b;
        auto& first = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&b.flush(b.unknown())});
        auto& clamped = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPSaturate32, IrType::F32, {&first})}));
        auto& product = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPMul32, IrType::F32, {&first, &first})}));
        b.keep(clamped);
        b.keep(product);
        return b.eliminate() == 1u && !clamped.HasUses() && product.HasUses();
    });

    passed &= run("a min, max, min3, max3 or med3 of flushed values needs no flush, one with an unknown operand does", [] {
        Builder b;
        const auto value = [&](IrValue& bits) -> IrValue& { return b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&bits}); };
        const auto flushedResult = [&](IrOpcode opcode, std::initializer_list<IrValue*> arguments) -> IrValue& {
            return b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(opcode, IrType::F32, arguments)}));
        };
        auto& x = value(b.flush(b.unknown()));
        auto& y = value(b.flush(b.unknown()));
        auto& z = value(b.flush(b.unknown()));
        auto& raw = value(b.unknown());
        IrValue* removed[] = {
            &flushedResult(IrOpcode::FPMin32, {&x, &y}),
            &flushedResult(IrOpcode::FPMax32, {&y, &x}),
            &flushedResult(IrOpcode::FPMinTri32, {&x, &y, &z}),
            &flushedResult(IrOpcode::FPMaxTri32, {&z, &y, &x}),
            &flushedResult(IrOpcode::FPMedTri32, {&x, &z, &y}),
        };
        auto& mixedPair = flushedResult(IrOpcode::FPMax32, {&x, &raw});
        auto& mixedMedian = flushedResult(IrOpcode::FPMedTri32, {&x, &y, &raw});
        for (auto* flushed : removed) b.keep(*flushed);
        b.keep(mixedPair);
        b.keep(mixedMedian);
        const bool counted = b.eliminate() == 5u;
        bool kept = false;
        for (auto* flushed : removed) kept |= flushed->HasUses();
        return counted && !kept && mixedPair.HasUses() && mixedMedian.HasUses();
    });

    passed &= run("a floor or ceil needs no flush, even of an unknown value", [] {
        Builder b;
        auto& raw = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&b.unknown()});
        auto& floored = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPFloor32, IrType::F32, {&raw})}));
        auto& ceiled = b.flush(b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPCeil32, IrType::F32, {&raw})}));
        b.keep(floored);
        b.keep(ceiled);
        return b.eliminate() == 2u && !floored.HasUses() && !ceiled.HasUses();
    });

    passed &= run("a max carried around a loop without a flush keeps its flush and the search ends", [] {
        Builder b;
        IrBlock& head = *b.block;
        IrBlock& body = b.newBlock();
        IrBlock& exit = b.newBlock();
        head.AddBranch(&body);
        body.AddBranch(&body);
        body.AddBranch(&exit);
        auto& initial = b.flush(b.unknown());
        b.block = &body;
        auto& carried = b.program.CreateValue(IrOpcode::Phi, IrType::U32);
        body.AppendInstruction(&carried);
        auto& step = b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&b.flush(b.unknown())});
        auto& maximum = b.emit(IrOpcode::BitCastU32F32, IrType::U32, {&b.emit(IrOpcode::FPMax32, IrType::F32, {&b.emit(IrOpcode::BitCastF32U32, IrType::F32, {&carried}), &step})});
        carried.AddPhiOperand(&head, &initial);
        carried.AddPhiOperand(&body, &maximum);
        auto& flushed = b.flush(maximum);
        b.block = &exit;
        b.keep(flushed);
        return b.eliminate() == 0u && flushed.HasUses();
    });

    passed &= run("a phi of flushed values needs no flush", [] {
        Builder b;
        IrBlock& head = *b.block;
        IrBlock& body = b.newBlock();
        IrBlock& merge = b.newBlock();
        head.AddBranch(&body);
        head.AddBranch(&merge);
        body.AddBranch(&merge);
        auto& fromHead = b.flush(b.unknown());
        b.block = &body;
        auto& fromBody = b.flush(b.unknown());
        b.block = &merge;
        auto& merged = b.flush(b.phi(merge, {{&head, &fromHead}, {&body, &fromBody}}));
        b.keep(merged);
        return b.eliminate() == 1u && !merged.HasUses();
    });

    passed &= run("a loop phi needs no flush when every value it takes is flushed, and keeps it otherwise", [] {
        const auto loop = [](bool flushedBackEdge) {
            Builder b;
            IrBlock& head = *b.block;
            IrBlock& body = b.newBlock();
            IrBlock& exit = b.newBlock();
            head.AddBranch(&body);
            body.AddBranch(&body);
            body.AddBranch(&exit);
            auto& initial = b.flush(b.unknown());
            b.block = &body;
            auto& carried = b.program.CreateValue(IrOpcode::Phi, IrType::U32);
            body.AppendInstruction(&carried);
            auto& next = b.flush(carried);
            auto& backEdge = flushedBackEdge ? next : b.emit(IrOpcode::BitwiseXor32, IrType::U32, {&b.unknown(), &next});
            carried.AddPhiOperand(&head, &initial);
            carried.AddPhiOperand(&body, &backEdge);
            b.block = &exit;
            b.keep(next);
            b.eliminate();
            return !next.HasUses();
        };
        return loop(true) && !loop(false);
    });

    passed &= run("with proofs, a max carried around a loop of flushed values needs no flush", [] {
        const auto removed = [](RewriteMode proofs) {
            Builder b;
            IrBlock& head = *b.block;
            IrBlock& body = b.newBlock();
            IrBlock& exit = b.newBlock();
            head.AddBranch(&body);
            body.AddBranch(&body);
            body.AddBranch(&exit);
            auto& initial = b.flush(b.unknown());
            b.block = &body;
            auto& carried = b.program.CreateValue(IrOpcode::Phi, IrType::U32);
            body.AppendInstruction(&carried);
            auto& step = b.asFloat(b.flush(b.unknown()));
            auto& maximum = b.asBits(b.emit(IrOpcode::FPMax32, IrType::F32, {&b.asFloat(carried), &step}));
            carried.AddPhiOperand(&head, &initial);
            carried.AddPhiOperand(&body, &maximum);
            auto& flushed = b.flush(maximum);
            b.block = &exit;
            b.keep(flushed);
            return proofs == RewriteMode::Off ? b.eliminate() : b.eliminate(proofs).removedFlushes;
        };
        return removed(RewriteMode::Off) == 0u && removed(RewriteMode::On) == 1u;
    });

    passed &= run("with proofs, a loop that brings in an unknown value keeps its flush", [] {
        Builder b;
        IrBlock& head = *b.block;
        IrBlock& body = b.newBlock();
        IrBlock& exit = b.newBlock();
        head.AddBranch(&body);
        body.AddBranch(&body);
        body.AddBranch(&exit);
        auto& initial = b.flush(b.unknown());
        b.block = &body;
        auto& carried = b.program.CreateValue(IrOpcode::Phi, IrType::U32);
        body.AppendInstruction(&carried);
        auto& maximum = b.asBits(b.emit(IrOpcode::FPMax32, IrType::F32, {&b.asFloat(carried), &b.asFloat(b.unknown())}));
        carried.AddPhiOperand(&head, &initial);
        carried.AddPhiOperand(&body, &maximum);
        auto& flushed = b.flush(maximum);
        b.block = &exit;
        b.keep(flushed);
        return b.eliminate(RewriteMode::On).removedFlushes == 0u && flushed.HasUses();
    });

    passed &= run("with proofs, a NaN-propagation select needs no flush when its non-NaN value is free of denormals", [] {
        const auto chain = [](bool provable) {
            Builder b;
            auto& source = b.unknown();
            auto& result = provable ? b.flush(b.unknown()) : b.unknown();
            auto& canonical = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.isNan(result), &b.constant(0xffc00000u), &result});
            auto& quieted = b.emit(IrOpcode::BitwiseOr32, IrType::U32, {&source, &b.constant(0x00400000u)});
            auto& propagated = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.isNan(source), &quieted, &canonical});
            auto& flushed = b.flush(propagated);
            b.keep(flushed);
            return b.eliminate(RewriteMode::On).removedFlushes == 1u && !flushed.HasUses();
        };
        return chain(true) && !chain(false);
    });

    passed &= run("with proofs, a select on a NaN test of another value keeps its flush", [] {
        Builder b;
        auto& source = b.unknown();
        auto& other = b.unknown();
        auto& propagated = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.isNan(other), &source, &b.flush(b.unknown())});
        auto& flushed = b.flush(propagated);
        b.keep(flushed);
        return b.eliminate(RewriteMode::On).removedFlushes == 0u && flushed.HasUses();
    });

    passed &= run("with proofs, a product with a constant of magnitude at least one needs no flush, a smaller one does", [] {
        Builder b;
        const auto product = [&](IrValue& value, std::uint32_t factor) -> IrValue& {
            auto& scale = b.program.CreateValue(IrOpcode::Void, IrType::F32);
            scale.SetImmediateU32(factor);
            return b.flush(b.asBits(b.emit(IrOpcode::FPMul32, IrType::F32, {&b.asFloat(value), &scale})));
        };
        auto& doubled = product(b.flush(b.unknown()), 0x40000000u);
        auto& negatedOne = product(b.flush(b.unknown()), 0xbf800000u);
        auto& halved = product(b.flush(b.unknown()), 0x3f000000u);
        auto& unknownDoubled = product(b.unknown(), 0x40000000u);
        for (auto* value : {&doubled, &negatedOne, &halved, &unknownDoubled}) b.keep(*value);
        return b.eliminate(RewriteMode::On).removedFlushes == 2u && !doubled.HasUses() && !negatedOne.HasUses() && halved.HasUses() && unknownDoubled.HasUses();
    });

    passed &= run("with proofs, truncation, rounding and square roots of flushed values need no flush", [] {
        Builder b;
        const auto result = [&](IrOpcode opcode, IrValue& bits) -> IrValue& { return b.flush(b.asBits(b.emit(opcode, IrType::F32, {&b.asFloat(bits)}))); };
        auto& truncated = result(IrOpcode::FPTrunc32, b.unknown());
        auto& rounded = result(IrOpcode::FPRoundEven32, b.unknown());
        auto& root = result(IrOpcode::FPSqrt, b.flush(b.unknown()));
        auto& reciprocalRoot = result(IrOpcode::FPRecipSqrt32, b.flush(b.unknown()));
        auto& unknownRoot = result(IrOpcode::FPSqrt, b.unknown());
        for (auto* value : {&truncated, &rounded, &root, &reciprocalRoot, &unknownRoot}) b.keep(*value);
        return b.eliminate(RewriteMode::On).removedFlushes == 4u && unknownRoot.HasUses() && !root.HasUses();
    });

    passed &= run("in verify mode the flush stays and a denormal check of its source is inserted", [] {
        Builder b;
        auto& source = b.asBits(b.emit(IrOpcode::FPTrunc32, IrType::F32, {&b.asFloat(b.unknown())}));
        auto& flushed = b.flush(source);
        b.keep(flushed);
        const auto stats = b.eliminate(RewriteMode::Verify);
        bool checked = false;
        for (const auto* inst : b.block->Instructions()) checked |= inst->Opcode() == IrOpcode::CheckDenormalFree && inst->Argument(0)->Resolve() == &source;
        return stats.removedFlushes == 0u && stats.checkedFlushes == 1u && flushed.HasUses() && checked && b.program.Info().usesFaultBuffer;
    });

    const auto interpolation = [](Builder& b, std::uint32_t mode) -> IrValue& {
        auto& half = b.emit(IrOpcode::ConvertF32F16, IrType::F32, {&b.unknown()});
        auto& unknown = b.asFloat(b.unknown());
        auto& one = b.program.CreateValue(IrOpcode::Void, IrType::F32);
        one.SetImmediateU32(0x3f800000u);
        auto& result = b.emit(IrOpcode::FPInterpolateF16, IrType::F32, {&half, &unknown, &one, &b.constant(mode)});
        b.keep(b.asBits(result));
        return result;
    };

    passed &= run("interpolation facts mark proven inputs and flushed inputs in the mode", [&] {
        Builder b;
        auto& result = interpolation(b, 2u << InterpolationRound16Shift);
        const auto stats = InterpolationFactsAnnotator{}.Annotate(b.program, RewriteMode::On);
        const auto mode = result.Argument(3)->Resolve()->ImmediateU32();
        return stats.specializedOperations == 1u && stats.provenInputs == 2u && mode == ((2u << InterpolationRound16Shift) | InterpolationFlushedExact | (5u << InterpolationNormalShift));
    });

    passed &= run("interpolation facts off leave the mode unchanged", [&] {
        Builder b;
        auto& result = interpolation(b, InterpolationFlush32);
        const auto stats = InterpolationFactsAnnotator{}.Annotate(b.program, RewriteMode::Off);
        return stats.specializedOperations == 0u && result.Argument(3)->Resolve()->ImmediateU32() == InterpolationFlush32;
    });

    passed &= run("interpolation facts verify keeps the mode and checks each proven input that is not a constant", [&] {
        Builder b;
        auto& result = interpolation(b, InterpolationFlush32);
        const auto stats = InterpolationFactsAnnotator{}.Annotate(b.program, RewriteMode::Verify);
        std::uint32_t checks = 0;
        for (const auto* inst : b.block->Instructions()) {
            if (inst->Opcode() != IrOpcode::CheckDenormalFree) continue;
            ++checks;
            if (inst->Argument(0)->Resolve()->Argument(0)->Resolve() != result.Argument(0)->Resolve()) return false;
        }
        return stats.checkedInputs == 1u && checks == 1u && result.Argument(3)->Resolve()->ImmediateU32() == InterpolationFlush32 && b.program.Info().usesFaultBuffer;
    });

    return passed ? 0 : 1;
}

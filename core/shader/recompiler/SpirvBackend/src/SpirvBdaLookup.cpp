#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvMemory/SpirvTypes.hpp"
#include "SpirvBackend/SpirvMemory/SpirvConstants.hpp"
#include "BdaTable.hpp"
#include <spirv/unified1/GLSL.std.450.h>
#include <array>
#include <bit>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <iterator>
#include <initializer_list>

namespace ShaderRecompiler {

namespace {

void ReturnBdaZeroIf(SpirvEmitterState& state, std::uint32_t condition) {
    const auto failed = state.module.AllocateId();
    const auto next = state.module.AllocateId();
    state.module.AddFunction(spv::OpSelectionMerge, next, spv::SelectionControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, condition, failed, next);
    EmitLabel(state, failed);
    state.module.AddFunction(spv::OpReturnValue, BdaConstant(state, 0u));
    EmitLabel(state, next);
}

enum class BdaLookupFailure { Record, Zero, Report };

std::uint32_t BdaResolvedType(SpirvEmitterState& state) {
    return state.module.Type(spv::OpTypeVector, TypeScalarU64(state), 2u);
}

struct BdaLookupBody {
    SpirvEmitterState& state;
    BdaLookupFailure failure;
    std::uint32_t address;
    std::uint32_t bytes;
    std::uint32_t instruction;

    std::uint32_t constant(std::uint32_t value) const { return ConstantU32(state, value); }

    std::uint32_t binary(std::uint32_t op, std::uint32_t type, std::uint32_t left, std::uint32_t right) const { return Binary(state, op, type, left, right); }

    void returnValue(std::uint32_t result, std::uint32_t reason) const {
        if (failure != BdaLookupFailure::Report) {
            state.module.AddFunction(spv::OpReturnValue, result);
            return;
        }
        const auto pair = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeConstruct, BdaResolvedType(state), pair, result, Unary(state, spv::OpUConvert, TypeScalarU64(state), reason));
        state.module.AddFunction(spv::OpReturnValue, pair);
    }

    void returnIf(std::uint32_t condition, std::uint32_t reason) const {
        const auto failed = state.module.AllocateId();
        const auto next = state.module.AllocateId();
        state.module.AddFunction(spv::OpSelectionMerge, next, spv::SelectionControlMaskNone);
        state.module.AddFunction(spv::OpBranchConditional, condition, failed, next);
        EmitLabel(state, failed);
        returnValue(BdaConstant(state, 0u), reason);
        EmitLabel(state, next);
    }

    void fail(std::uint32_t condition, BdaAbi::FaultReason reason) const {
        if (failure == BdaLookupFailure::Record) ReturnBdaFailureIf(state, condition, address, bytes, instruction, reason);
        else if (failure == BdaLookupFailure::Zero) ReturnBdaZeroIf(state, condition);
        else returnIf(condition, constant(static_cast<std::uint32_t>(reason)));
    }

    void failFirst(std::initializer_list<std::pair<std::uint32_t, BdaAbi::FaultReason>> checks) const {
        const auto u32 = TypeU32(state);
        auto reason = constant(0u);
        for (auto check = std::rbegin(checks); check != std::rend(checks); ++check) {
            reason = Select(state, u32, check->first, constant(static_cast<std::uint32_t>(check->second)), reason);
        }
        const auto failed = binary(spv::OpINotEqual, TypeBool(state), reason, constant(0u));
        if (failure == BdaLookupFailure::Zero) {
            ReturnBdaZeroIf(state, failed);
            return;
        }
        if (failure == BdaLookupFailure::Report) {
            returnIf(failed, reason);
            return;
        }
        const auto failedLabel = state.module.AllocateId();
        const auto next = state.module.AllocateId();
        state.module.AddFunction(spv::OpSelectionMerge, next, spv::SelectionControlMaskNone);
        state.module.AddFunction(spv::OpBranchConditional, failed, failedLabel, next);
        EmitLabel(state, failedLabel);
        RecordBdaFaultReason(state, address, bytes, instruction, reason);
        state.module.AddFunction(spv::OpReturnValue, BdaConstant(state, 0u));
        EmitLabel(state, next);
    }

    std::uint32_t end() const { return binary(spv::OpIAdd, TypeScalarU64(state), address, Unary(state, spv::OpUConvert, TypeScalarU64(state), bytes)); }

    void selectedEntry(std::uint32_t index, std::uint32_t rangeBase, std::uint32_t permission, std::uint32_t end) const {
        const auto u32 = TypeU32(state);
        const auto u64 = TypeScalarU64(state);
        const auto boolean = TypeBool(state);
        fail(binary(spv::OpIEqual, boolean, index, constant(0)), BdaAbi::FaultReason::Unmapped);
        const auto selected = binary(spv::OpIAdd, u32, constant(rangeBase), binary(spv::OpIMul, u32, binary(spv::OpISub, u32, index, constant(1)), constant(BdaTable::RangeWords)));
        const auto at = [&](std::uint32_t offset) { return binary(spv::OpIAdd, u32, selected, constant(offset)); };
        const auto begin = BdaLoadAddress(state, selected);
        const auto finish = BdaLoadAddress(state, at(2));
        const auto base = BdaLoadAddress(state, at(4));
        const auto permissions = BdaLoadWord(state, at(6));
        const auto offset = binary(spv::OpISub, u64, address, begin);
        const auto result = binary(spv::OpIAdd, u64, base, offset);
        const auto deviceEnd = binary(spv::OpIAdd, u64, result, Unary(state, spv::OpUConvert, u64, bytes));
        failFirst({
            {binary(spv::OpINotEqual, boolean, BdaLoadWord(state, at(7)), constant(0)), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpUGreaterThanEqual, boolean, begin, finish), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpUGreaterThan, boolean, end, finish), BdaAbi::FaultReason::Unmapped},
            {binary(spv::OpINotEqual, boolean, binary(spv::OpBitwiseAnd, u32, permissions, constant(permission)), constant(permission)), BdaAbi::FaultReason::Permission},
            {binary(spv::OpIEqual, boolean, base, BdaConstant(state, 0)), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpULessThan, boolean, result, base), BdaAbi::FaultReason::Overflow},
            {binary(spv::OpULessThanEqual, boolean, deviceEnd, result), BdaAbi::FaultReason::Overflow},
        });
        returnValue(result, constant(0u));
    }
};

void EmitLegacyLookup(const BdaLookupBody& body, std::uint32_t permission, bool radixLayout) {
    auto& state = body.state;
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t value) { return body.constant(value); };
    const auto binary = [&](std::uint32_t op, std::uint32_t type, std::uint32_t left, std::uint32_t right) { return body.binary(op, type, left, right); };
    const auto rangeBase = radixLayout ? BdaTable::RadixRangeWord : BdaTable::HeaderWords;
    const auto& address = body.address;
    const auto& bytes = body.bytes;
    const auto length = state.module.AllocateId();
    state.module.AddFunction(spv::OpArrayLength, u32, length, state.bdaPagetableVariable, 0u);
    body.fail(binary(spv::OpULessThan, boolean, length, constant(rangeBase)), BdaAbi::FaultReason::InvalidTable);
    const auto count = BdaLoadWord(state, constant(1));
    const auto end = body.end();
    if (radixLayout) {
        const auto rootWord = BdaLoadWord(state, constant(3));
        body.failFirst({
            {binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(0)), constant(BdaTable::RadixVersion)), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(2)), constant(sizeof(BdaAbi::Range))), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpINotEqual, boolean, rootWord, binary(spv::OpIAdd, u32, constant(rangeBase), binary(spv::OpIMul, u32, count, constant(BdaTable::RangeWords)))), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpULessThan, boolean, length, rootWord), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpIEqual, boolean, bytes, constant(0)), BdaAbi::FaultReason::Overflow},
            {binary(spv::OpULessThanEqual, boolean, end, address), BdaAbi::FaultReason::Overflow},
        });
    } else {
        const auto available = binary(spv::OpISub, u32, length, constant(4));
        body.failFirst({
            {binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(0)), constant(BdaAbi::Version)), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(2)), constant(sizeof(BdaAbi::Range))), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(3)), constant(0)), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpINotEqual, boolean, binary(spv::OpBitwiseAnd, u32, available, constant(7)), constant(0)), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpINotEqual, boolean, count, binary(spv::OpShiftRightLogical, u32, available, constant(3))), BdaAbi::FaultReason::InvalidTable},
            {binary(spv::OpIEqual, boolean, bytes, constant(0)), BdaAbi::FaultReason::Overflow},
            {binary(spv::OpULessThanEqual, boolean, end, address), BdaAbi::FaultReason::Overflow},
        });
    }
    const auto header = state.module.AllocateId();
    const auto loopBody = state.module.AllocateId();
    const auto continuation = state.module.AllocateId();
    const auto merge = state.module.AllocateId();
    const auto entryLabel = state.currentLabel;
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, header);
    const auto lower = state.module.AllocateId();
    const auto upper = state.module.AllocateId();
    const auto newLow = state.module.AllocateId();
    const auto newHigh = state.module.AllocateId();
    state.module.AddFunction(spv::OpPhi, u32, lower, constant(0), entryLabel, newLow, continuation);
    state.module.AddFunction(spv::OpPhi, u32, upper, count, entryLabel, newHigh, continuation);
    const auto search = binary(spv::OpULessThan, boolean, lower, upper);
    state.module.AddFunction(spv::OpLoopMerge, merge, continuation, spv::LoopControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, search, loopBody, merge);
    EmitLabel(state, loopBody);
    const auto midpoint = binary(spv::OpIAdd, u32, lower, binary(spv::OpShiftRightLogical, u32, binary(spv::OpISub, u32, upper, lower), constant(1)));
    const auto entry = binary(spv::OpIAdd, u32, constant(rangeBase), binary(spv::OpIMul, u32, midpoint, constant(BdaTable::RangeWords)));
    const auto before = binary(spv::OpULessThan, boolean, address, BdaLoadAddress(state, entry));
    state.module.AddFunction(spv::OpSelect, u32, newLow, before, lower, binary(spv::OpIAdd, u32, midpoint, constant(1)));
    state.module.AddFunction(spv::OpSelect, u32, newHigh, before, midpoint, upper);
    state.module.AddFunction(spv::OpBranch, continuation);
    EmitLabel(state, continuation);
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, merge);
    body.selectedEntry(lower, rangeBase, permission, end);
}

void EmitRadixLookup(const BdaLookupBody& body, std::uint32_t permission) {
    auto& state = body.state;
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t value) { return body.constant(value); };
    const auto binary = [&](std::uint32_t op, std::uint32_t type, std::uint32_t left, std::uint32_t right) { return body.binary(op, type, left, right); };
    const auto& address = body.address;
    const auto& bytes = body.bytes;
    const auto count = BdaLoadWord(state, constant(1));
    const auto rootWord = BdaLoadWord(state, constant(3));
    const auto windowBase = BdaLoadAddress(state, constant(BdaTable::WindowBaseWord));
    const auto windowLast = BdaLoadAddress(state, constant(BdaTable::WindowLastWord));
    const auto rootShift = BdaLoadWord(state, constant(BdaTable::RootShiftWord));
    const auto end = body.end();
    body.failFirst({
        {binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(0)), constant(BdaTable::RadixVersion)), BdaAbi::FaultReason::InvalidTable},
        {binary(spv::OpINotEqual, boolean, BdaLoadWord(state, constant(2)), constant(sizeof(BdaAbi::Range))), BdaAbi::FaultReason::InvalidTable},
        {binary(spv::OpIEqual, boolean, bytes, constant(0)), BdaAbi::FaultReason::Overflow},
        {binary(spv::OpULessThanEqual, boolean, end, address), BdaAbi::FaultReason::Overflow},
    });
    const auto offset = binary(spv::OpISub, u64, address, windowBase);
    const auto before = binary(spv::OpULessThan, boolean, address, windowBase);
    const auto empty = binary(spv::OpIEqual, boolean, count, constant(0));
    const auto below = binary(spv::OpLogicalOr, boolean, before, empty);
    const auto outside = binary(spv::OpLogicalOr, boolean, below, binary(spv::OpUGreaterThan, boolean, offset, windowLast));
    const auto outsideSelected = Select(state, u32, below, constant(0), count);
    const auto walkLabel = state.module.AllocateId();
    const auto skipLabel = state.module.AllocateId();
    const auto selectedMerge = state.module.AllocateId();
    state.module.AddFunction(spv::OpSelectionMerge, selectedMerge, spv::SelectionControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, outside, skipLabel, walkLabel);
    EmitLabel(state, walkLabel);
    const auto rootIndex = Unary(state, spv::OpUConvert, u32, binary(spv::OpShiftRightLogical, u64, offset, Unary(state, spv::OpUConvert, u64, rootShift)));
    const auto startWord = binary(spv::OpIAdd, u32, rootWord, binary(spv::OpIMul, u32, rootIndex, constant(BdaTable::EntryWords)));
    const auto walkExit = state.currentLabel;
    const auto header = state.module.AllocateId();
    const auto loopBody = state.module.AllocateId();
    const auto continuation = state.module.AllocateId();
    const auto loopMerge = state.module.AllocateId();
    const auto entryWord = state.module.AllocateId();
    const auto shift = state.module.AllocateId();
    const auto nextWord = state.module.AllocateId();
    const auto nextShift = state.module.AllocateId();
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, header);
    state.module.AddFunction(spv::OpPhi, u32, entryWord, startWord, walkExit, nextWord, continuation);
    state.module.AddFunction(spv::OpPhi, u32, shift, rootShift, walkExit, nextShift, continuation);
    state.module.AddFunction(spv::OpLoopMerge, loopMerge, continuation, spv::LoopControlMaskNone);
    state.module.AddFunction(spv::OpBranch, loopBody);
    EmitLabel(state, loopBody);
    const auto value = BdaLoadWord(state, entryWord);
    const auto info = BdaLoadWord(state, binary(spv::OpIAdd, u32, entryWord, constant(1)));
    const auto node = binary(spv::OpINotEqual, boolean, binary(spv::OpBitwiseAnd, u32, info, constant(BdaTable::NodeBit)), constant(0));
    state.module.AddFunction(spv::OpSelect, u32, nextShift, node, binary(spv::OpISub, u32, shift, constant(BdaTable::ChildBits)), constant(0));
    const auto child = Unary(state, spv::OpUConvert, u32, binary(spv::OpBitwiseAnd, u64, binary(spv::OpShiftRightLogical, u64, offset, Unary(state, spv::OpUConvert, u64, nextShift)), BdaConstant(state, BdaTable::ChildEntries - 1u)));
    state.module.AddFunction(spv::OpIAdd, u32, nextWord, value, binary(spv::OpIMul, u32, child, constant(BdaTable::EntryWords)));
    state.module.AddFunction(spv::OpBranchConditional, node, continuation, loopMerge);
    EmitLabel(state, continuation);
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, loopMerge);
    state.module.AddFunction(spv::OpBranch, selectedMerge);
    EmitLabel(state, skipLabel);
    state.module.AddFunction(spv::OpBranch, selectedMerge);
    EmitLabel(state, selectedMerge);
    const auto first = state.module.AllocateId();
    const auto begins = state.module.AllocateId();
    state.module.AddFunction(spv::OpPhi, u32, first, value, loopMerge, outsideSelected, skipLabel);
    state.module.AddFunction(spv::OpPhi, u32, begins, info, loopMerge, constant(0), skipLabel);
    const auto found = EmitValueOrZeroIfCondition(state, binary(spv::OpINotEqual, boolean, begins, constant(0)), [&] {
        const auto lastBegin = binary(spv::OpISub, u32, begins, constant(1));
        auto total = constant(0);
        for (std::uint32_t candidate = 0; candidate < BdaTable::SearchLimit; ++candidate) {
            const auto index = EmitGlsl<GLSLstd450UMin, IrType::U32>(state, constant(candidate), lastBegin);
            const auto word = binary(spv::OpIAdd, u32, constant(BdaTable::RadixRangeWord), binary(spv::OpIMul, u32, binary(spv::OpIAdd, u32, first, index), constant(BdaTable::RangeWords)));
            const auto reached = binary(spv::OpLogicalAnd, boolean, binary(spv::OpULessThan, boolean, constant(candidate), begins), binary(spv::OpULessThanEqual, boolean, BdaLoadAddress(state, word), address));
            total = binary(spv::OpIAdd, u32, total, Select(state, u32, reached, constant(1), constant(0)));
        }
        return total;
    });
    body.selectedEntry(binary(spv::OpIAdd, u32, first, found), BdaTable::RadixRangeWord, permission, end);
}

std::uint32_t PermissionSlot(std::uint32_t permission) {
    if (permission == 0u || permission > (BdaAbi::Read | BdaAbi::Write)) throw std::runtime_error("unsupported BDA lookup permission");
    return permission;
}

std::uint32_t DefineBdaResolve(SpirvEmitterState& state, bool radix, std::uint32_t permission) {
    auto& function = state.bdaResolveFunctions[radix ? 1u : 0u][PermissionSlot(permission)];
    if (function != 0) return function;
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto resolved = BdaResolvedType(state);
    function = state.module.AllocateId();
    state.module.AddName(function, std::string(radix ? "resolve_bda_radix_" : "resolve_bda_legacy_") + std::to_string(permission));
    state.module.AddFunction(spv::OpFunction, resolved, function, spv::FunctionControlDontInlineMask, state.module.Type(spv::OpTypeFunction, resolved, u64, u32));
    const auto address = state.module.AllocateId();
    const auto bytes = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionParameter, u64, address);
    state.module.AddFunction(spv::OpFunctionParameter, u32, bytes);
    EmitLabel(state, state.module.AllocateId());
    const BdaLookupBody body{state, BdaLookupFailure::Report, address, bytes, 0u};
    if (radix) EmitRadixLookup(body, permission);
    else EmitLegacyLookup(body, permission, true);
    state.module.AddFunction(spv::OpFunctionEnd);
    return function;
}

// (u64 address, u32 bytes, u32 instruction) -> u64 device address of the range holding all the bytes,
std::uint32_t DefineBdaLookup(SpirvEmitterState& state, const char* name, bool recordFaults, std::uint32_t permission = BdaAbi::Read) {
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto translation = state.bdaTranslation;
    std::array<std::uint32_t, 2> resolves{};
    if (translation == BdaTable::Translation::Verify) {
        resolves[0] = DefineBdaResolve(state, false, permission);
        resolves[1] = DefineBdaResolve(state, true, permission);
    }
    const auto function = state.module.AllocateId();
    state.module.AddName(function, name);
    state.module.AddFunction(spv::OpFunction, u64, function, spv::FunctionControlDontInlineMask, state.module.Type(spv::OpTypeFunction, u64, u64, u32, u32));
    const auto address = state.module.AllocateId();
    const auto bytes = state.module.AllocateId();
    const auto instruction = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionParameter, u64, address);
    state.module.AddFunction(spv::OpFunctionParameter, u32, bytes);
    state.module.AddFunction(spv::OpFunctionParameter, u32, instruction);
    EmitLabel(state, state.module.AllocateId());
    const BdaLookupBody body{state, recordFaults ? BdaLookupFailure::Record : BdaLookupFailure::Zero, address, bytes, instruction};
    if (translation == BdaTable::Translation::Legacy) {
        EmitLegacyLookup(body, permission, false);
    } else if (translation == BdaTable::Translation::Radix) {
        EmitRadixLookup(body, permission);
    } else {
        const auto boolean = TypeBool(state);
        const auto resolved = BdaResolvedType(state);
        std::array<std::uint32_t, 2> pairs{};
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            pairs[index] = state.module.AllocateId();
            state.module.AddFunction(spv::OpFunctionCall, resolved, pairs[index], resolves[index], address, bytes);
        }
        const auto component = [&](std::uint32_t pair, std::uint32_t index) {
            const auto value = state.module.AllocateId();
            state.module.AddFunction(spv::OpCompositeExtract, u64, value, pair, index);
            return value;
        };
        const auto result = component(pairs[0], 0u);
        const auto reason = component(pairs[0], 1u);
        const auto differs = Binary(state, spv::OpLogicalOr, boolean, Binary(state, spv::OpINotEqual, boolean, result, component(pairs[1], 0u)), Binary(state, spv::OpINotEqual, boolean, reason, component(pairs[1], 1u)));
        ReturnBdaFailureIf(state, differs, address, bytes, instruction, BdaAbi::FaultReason::TranslationMismatch);
        const auto failed = Binary(state, spv::OpINotEqual, boolean, reason, BdaConstant(state, 0u));
        if (recordFaults) {
            const auto failedLabel = state.module.AllocateId();
            const auto next = state.module.AllocateId();
            state.module.AddFunction(spv::OpSelectionMerge, next, spv::SelectionControlMaskNone);
            state.module.AddFunction(spv::OpBranchConditional, failed, failedLabel, next);
            EmitLabel(state, failedLabel);
            RecordBdaFaultReason(state, address, bytes, instruction, Unary(state, spv::OpUConvert, u32, reason));
            state.module.AddFunction(spv::OpReturnValue, BdaConstant(state, 0u));
            EmitLabel(state, next);
        } else {
            ReturnBdaZeroIf(state, failed);
        }
        state.module.AddFunction(spv::OpReturnValue, result);
    }
    state.module.AddFunction(spv::OpFunctionEnd);
    return function;
}

}

static std::uint32_t DefineBdaNoteWrite(SpirvEmitterState& state) {
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t value) { return ConstantU32(state, value); };
    const auto binary = [&](std::uint32_t op, std::uint32_t type, std::uint32_t left, std::uint32_t right) { return Binary(state, op, type, left, right); };
    const auto voidType = state.module.Type(spv::OpTypeVoid);
    const auto function = state.module.AllocateId();
    state.module.AddName(function, "note_bda_write");
    state.module.AddFunction(spv::OpFunction, voidType, function, spv::FunctionControlMaskNone, state.module.Type(spv::OpTypeFunction, voidType, TypeScalarU64(state)));
    const auto address = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunctionParameter, TypeScalarU64(state), address);
    EmitLabel(state, state.module.AllocateId());
    const auto done = state.module.AllocateId();
    state.module.AddFunction(spv::OpVariable, TypePointer(state, spv::StorageClassFunction, boolean), done, spv::StorageClassFunction);
    const auto counter = state.module.AllocateId();
    state.module.AddFunction(spv::OpVariable, TypePointer(state, spv::StorageClassFunction, u32), counter, spv::StorageClassFunction);
    state.module.AddFunction(spv::OpStore, done, ConstantBool(state, false));
    state.module.AddFunction(spv::OpStore, counter, constant(0u));
    const auto u64 = TypeScalarU64(state);
    const auto key = binary(spv::OpIAdd, u32, Unary(state, spv::OpUConvert, u32, binary(spv::OpShiftRightLogical, u64, address, BdaConstant(state, BdaAbi::WrittenChunkShift))), constant(1));
    const auto page = binary(spv::OpBitwiseAnd, u32, Unary(state, spv::OpUConvert, u32, binary(spv::OpShiftRightLogical, u64, address, BdaConstant(state, BdaAbi::WrittenPageShift))), constant(BdaAbi::WrittenChunkPages - 1u));
    const auto bit = binary(spv::OpShiftLeftLogical, u32, constant(1u), page);
    const auto hash = binary(spv::OpShiftRightLogical, u32, binary(spv::OpIMul, u32, key, constant(0x9e3779b1u)), constant(32u - static_cast<std::uint32_t>(std::countr_zero(BdaAbi::WrittenPageSlots))));
    const auto scope = constant(spv::ScopeDevice);
    const auto relaxed = constant(spv::MemorySemanticsMaskNone);
    const auto header = state.module.AllocateId();
    const auto body = state.module.AllocateId();
    const auto continuation = state.module.AllocateId();
    const auto merge = state.module.AllocateId();
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, header);
    const auto probe = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, u32, probe, counter);
    const auto found = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, boolean, found, done);
    const auto searching = binary(spv::OpLogicalAnd, boolean, Unary(state, spv::OpLogicalNot, boolean, found), binary(spv::OpULessThan, boolean, probe, constant(BdaAbi::WrittenPageProbes)));
    state.module.AddFunction(spv::OpLoopMerge, merge, continuation, spv::LoopControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, searching, body, merge);
    EmitLabel(state, body);
    const auto slot = binary(spv::OpBitwiseAnd, u32, binary(spv::OpIAdd, u32, hash, probe), constant(BdaAbi::WrittenPageSlots - 1u));
    const auto keyWord = binary(spv::OpIAdd, u32, constant(BdaAbi::WrittenSlotsWord), binary(spv::OpShiftLeftLogical, u32, slot, constant(1u)));
    const auto current = state.module.AllocateId();
    state.module.AddFunction(spv::OpAtomicLoad, u32, current, BdaWord(state, state.faultBufferVariable, keyWord), scope, relaxed);
    const auto same = binary(spv::OpIEqual, boolean, current, key);
    state.module.AddFunction(spv::OpStore, done, same);
    EmitIfCondition(state, Unary(state, spv::OpLogicalNot, boolean, same), [&] {
        const auto previous = state.module.AllocateId();
        state.module.AddFunction(spv::OpAtomicCompareExchange, u32, previous, BdaWord(state, state.faultBufferVariable, keyWord), scope, relaxed, relaxed, key, constant(0u));
        const auto claimed = binary(spv::OpIEqual, boolean, previous, constant(0u));
        state.module.AddFunction(spv::OpStore, done, binary(spv::OpLogicalOr, boolean, claimed, binary(spv::OpIEqual, boolean, previous, key)));
        EmitIfCondition(state, claimed, [&] {
            const auto index = state.module.AllocateId();
            state.module.AddFunction(spv::OpAtomicIAdd, u32, index, BdaWord(state, state.faultBufferVariable, constant(BdaAbi::WrittenCountWord)), scope, relaxed, constant(1u));
            state.module.AddFunction(spv::OpAtomicStore, BdaWord(state, state.faultBufferVariable, binary(spv::OpIAdd, u32, constant(BdaAbi::WrittenListWord), index)), scope, relaxed, slot);
        });
    });
    const auto holds = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, boolean, holds, done);
    EmitIfCondition(state, holds, [&] {
        const auto maskWord = binary(spv::OpIAdd, u32, keyWord, constant(1u));
        const auto mask = state.module.AllocateId();
        state.module.AddFunction(spv::OpAtomicLoad, u32, mask, BdaWord(state, state.faultBufferVariable, maskWord), scope, relaxed);
        EmitIfCondition(state, binary(spv::OpIEqual, boolean, binary(spv::OpBitwiseAnd, u32, mask, bit), constant(0u)), [&] {
            state.module.AddFunction(spv::OpAtomicOr, u32, state.module.AllocateId(), BdaWord(state, state.faultBufferVariable, maskWord), scope, relaxed, bit);
        });
    });
    state.module.AddFunction(spv::OpBranch, continuation);
    EmitLabel(state, continuation);
    state.module.AddFunction(spv::OpStore, counter, binary(spv::OpIAdd, u32, probe, constant(1u)));
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, merge);
    const auto noted = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, boolean, noted, done);
    EmitIfCondition(state, Unary(state, spv::OpLogicalNot, boolean, noted), [&] {
        state.module.AddFunction(spv::OpAtomicStore, BdaWord(state, state.faultBufferVariable, constant(BdaAbi::WrittenOverflowWord)), scope, relaxed, constant(1u));
    });
    state.module.AddFunction(spv::OpReturn);
    state.module.AddFunction(spv::OpFunctionEnd);
    return function;
}

std::uint32_t DefineBdaResolveFunction(SpirvEmitterState& state, bool radix, std::uint32_t permission) {
    return DefineBdaResolve(state, radix, permission);
}

void DefineGetBdaPointer(SpirvEmitterState& state) {
    if (state.faultBufferVariable != 0) DefineBdaFaultFunction(state);
    if (!state.program.Info().usesDma) return;
    state.bdaPointerFunction = DefineBdaLookup(state, "get_bda_pointer", true);
    if (!BdaByteReadsForced()) state.bdaProbeFunction = DefineBdaLookup(state, "probe_bda_pointer", false);
    DefineBdaDwordReadFunctions(state);
    DefineBdaSpanReadFunctions(state);
    if (state.program.Info().bdaWrites) {
        state.bdaWritePointerFunction = DefineBdaLookup(state, "get_bda_write_pointer", true, BdaAbi::Write);
        if (!BdaByteReadsForced()) state.bdaWriteProbeFunction = DefineBdaLookup(state, "probe_bda_write_pointer", false, BdaAbi::Write);
        state.bdaAtomicPointerFunction = DefineBdaLookup(state, "get_bda_atomic_pointer", true, BdaAbi::Read | BdaAbi::Write);
        state.bdaNoteWriteFunction = DefineBdaNoteWrite(state);
        DefineBdaByteWriteFunctions(state);
    }
}

}

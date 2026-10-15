#include "BdaPlacement.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvMemory/SpirvConstants.hpp"
#include "SpirvBackend/SpirvMemory/SpirvTypes.hpp"
#include <utility>

namespace ShaderRecompiler::BdaPlacement {

namespace {

struct Loop {
    std::uint32_t header = 0;
    std::uint32_t body = 0;
    std::uint32_t continuation = 0;
    std::uint32_t merge = 0;
    std::uint32_t entry = 0;
};

Loop BeginLoop(SpirvEmitterState& state) {
    Loop loop{state.module.AllocateId(), state.module.AllocateId(), state.module.AllocateId(), state.module.AllocateId(), state.currentLabel};
    state.module.AddFunction(spv::OpBranch, loop.header);
    EmitLabel(state, loop.header);
    return loop;
}

void LoopTest(SpirvEmitterState& state, const Loop& loop, std::uint32_t condition) {
    state.module.AddFunction(spv::OpLoopMerge, loop.merge, loop.continuation, spv::LoopControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, condition, loop.body, loop.merge);
    EmitLabel(state, loop.body);
}

void EndLoop(SpirvEmitterState& state, const Loop& loop) {
    state.module.AddFunction(spv::OpBranch, loop.continuation);
    EmitLabel(state, loop.continuation);
}

void CloseLoop(SpirvEmitterState& state, const Loop& loop) {
    state.module.AddFunction(spv::OpBranch, loop.header);
    EmitLabel(state, loop.merge);
}

}

std::vector<std::uint32_t> BuildCopyBackShader() {
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.Info().usesDma = true;
    SpirvEmitterState state(program, {});
    EmitBaseHeader(state.module, program);
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto boolean = TypeBool(state);
    const auto byte = state.module.Type(spv::OpTypeInt, 8u, 0u);
    const auto bytePointer = TypePointer(state, spv::StorageClassPhysicalStorageBuffer, byte);
    const auto wordPointer = TypePhysicalU32Pointer(state);
    const auto addressPointer = TypePointer(state, spv::StorageClassPhysicalStorageBuffer, u64);
    const auto constant = [&](std::uint32_t value) { return ConstantU32(state, value); };
    const auto binary = [&](std::uint32_t op, std::uint32_t type, std::uint32_t left, std::uint32_t right) { return Binary(state, op, type, left, right); };
    const auto push = state.module.Type(spv::OpTypeStruct, u64, u64, u32, u32);
    state.module.AddAnnotation(spv::OpDecorate, push, spv::DecorationBlock);
    for (const auto [member, offset] : {std::pair{0u, 0u}, std::pair{1u, 8u}, std::pair{2u, 16u}, std::pair{3u, 20u}}) state.module.AddAnnotation(spv::OpMemberDecorate, push, member, spv::DecorationOffset, offset);
    const auto pushVariable = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassPushConstant, push), spv::StorageClassPushConstant);
    const auto workgroup = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassInput, TypeU32Vector(state, 3u)), spv::StorageClassInput);
    state.module.AddAnnotation(spv::OpDecorate, workgroup, spv::DecorationBuiltIn, spv::BuiltInWorkgroupId);
    const auto local = state.module.DefineGlobalVariable(TypePointer(state, spv::StorageClassInput, u32), spv::StorageClassInput);
    state.module.AddAnnotation(spv::OpDecorate, local, spv::DecorationBuiltIn, spv::BuiltInLocalInvocationIndex);
    state.mainFunc = state.module.AllocateId();
    state.module.AddFunction(spv::OpFunction, TypeVoid(state), state.mainFunc, spv::FunctionControlMaskNone, TypeFunction(state));
    EmitLabel(state, state.module.AllocateId());
    const auto pushMember = [&](std::uint32_t member, std::uint32_t type) {
        const auto pointer = state.module.AllocateId();
        state.module.AddFunction(spv::OpAccessChain, TypePointer(state, spv::StorageClassPushConstant, type), pointer, pushVariable, constant(member));
        const auto value = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, type, value, pointer);
        return value;
    };
    const auto record = pushMember(0u, u64);
    const auto table = pushMember(1u, u64);
    const auto pass = pushMember(2u, u32);
    const auto at = [&](std::uint32_t pointerType, std::uint32_t address) {
        const auto pointer = state.module.AllocateId();
        state.module.AddFunction(spv::OpConvertUToPtr, pointerType, pointer, address);
        return pointer;
    };
    const auto offsetBy = [&](std::uint32_t base, std::uint32_t bytes) { return binary(spv::OpIAdd, u64, base, Unary(state, spv::OpUConvert, u64, bytes)); };
    const auto loadWord = [&](std::uint32_t base, std::uint32_t word) {
        const auto value = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, u32, value, at(wordPointer, offsetBy(base, binary(spv::OpIMul, u32, word, constant(4u)))), spv::MemoryAccessAlignedMask, 4u);
        return value;
    };
    const auto loadAddress = [&](std::uint32_t address) {
        const auto value = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, u64, value, at(addressPointer, address), spv::MemoryAccessAlignedMask, 8u);
        return value;
    };
    const auto loadByte = [&](std::uint32_t address) {
        const auto value = state.module.AllocateId();
        state.module.AddFunction(spv::OpLoad, byte, value, at(bytePointer, address), spv::MemoryAccessAlignedMask, 1u);
        return Unary(state, spv::OpUConvert, u32, value);
    };
    const auto storeByte = [&](std::uint32_t address, std::uint32_t value) {
        state.module.AddFunction(spv::OpStore, at(bytePointer, address), Unary(state, spv::OpUConvert, byte, value), spv::MemoryAccessAlignedMask, 1u);
    };
    const auto ids = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, TypeU32Vector(state, 3u), ids, workgroup);
    const auto claim = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, u32, claim, ids, 0u);
    const auto page = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, u32, page, ids, 1u);
    const auto thread = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, u32, thread, local);
    const auto count = loadWord(record, constant(BdaAbi::WrittenCountWord));
    EmitIfCondition(state, binary(spv::OpULessThan, boolean, claim, count), [&] {
        const auto slot = loadWord(record, binary(spv::OpIAdd, u32, constant(BdaAbi::WrittenListWord), claim));
        const auto slotWord = binary(spv::OpIAdd, u32, constant(BdaAbi::WrittenSlotsWord), binary(spv::OpShiftLeftLogical, u32, slot, constant(1u)));
        const auto key = loadWord(record, slotWord);
        const auto mask = loadWord(record, binary(spv::OpIAdd, u32, slotWord, constant(1u)));
        const auto written = binary(spv::OpINotEqual, boolean, binary(spv::OpBitwiseAnd, u32, binary(spv::OpShiftRightLogical, u32, mask, page), constant(1u)), constant(0u));
        const auto valid = binary(spv::OpLogicalAnd, boolean, written, binary(spv::OpINotEqual, boolean, key, constant(0u)));
        EmitIfCondition(state, valid, [&] {
            const auto chunk = binary(spv::OpShiftLeftLogical, u64, Unary(state, spv::OpUConvert, u64, binary(spv::OpISub, u32, key, constant(1u))), BdaConstant(state, BdaAbi::WrittenChunkShift));
            const auto pageBegin = binary(spv::OpBitwiseOr, u64, chunk, binary(spv::OpShiftLeftLogical, u64, Unary(state, spv::OpUConvert, u64, page), BdaConstant(state, BdaAbi::WrittenPageShift)));
            const auto pageEnd = binary(spv::OpIAdd, u64, pageBegin, BdaConstant(state, PageBytes));
            const auto placements = loadWord(table, constant(0u));
            const auto outer = BeginLoop(state);
            const auto index = state.module.AllocateId();
            const auto nextIndex = state.module.AllocateId();
            state.module.AddFunction(spv::OpPhi, u32, index, constant(0u), outer.entry, nextIndex, outer.continuation);
            LoopTest(state, outer, binary(spv::OpULessThan, boolean, index, placements));
            const auto entry = offsetBy(table, binary(spv::OpIAdd, u32, constant(sizeof(Header)), binary(spv::OpIMul, u32, index, constant(sizeof(Entry)))));
            const auto field = [&](std::uint32_t offset) { return loadAddress(binary(spv::OpIAdd, u64, entry, BdaConstant(state, offset))); };
            const auto begin = field(0u);
            const auto end = field(8u);
            const auto work = field(16u);
            const auto pristine = field(24u);
            const auto host = field(32u);
            const auto low = Select(state, u64, binary(spv::OpUGreaterThan, boolean, begin, pageBegin), begin, pageBegin);
            const auto high = Select(state, u64, binary(spv::OpULessThan, boolean, end, pageEnd), end, pageEnd);
            const auto inner = BeginLoop(state);
            const auto address = state.module.AllocateId();
            const auto nextAddress = state.module.AllocateId();
            state.module.AddFunction(spv::OpPhi, u64, address, binary(spv::OpIAdd, u64, low, Unary(state, spv::OpUConvert, u64, thread)), inner.entry, nextAddress, inner.continuation);
            LoopTest(state, inner, binary(spv::OpULessThan, boolean, address, high));
            const auto offset = binary(spv::OpISub, u64, address, begin);
            const auto current = loadByte(binary(spv::OpIAdd, u64, work, offset));
            const auto copying = binary(spv::OpIEqual, boolean, pass, constant(static_cast<std::uint32_t>(Pass::CopyBack)));
            EmitIfCondition(state, copying, [&] {
                const auto original = loadByte(binary(spv::OpIAdd, u64, pristine, offset));
                EmitIfCondition(state, binary(spv::OpINotEqual, boolean, current, original), [&] {
                    storeByte(binary(spv::OpIAdd, u64, host, offset), current);
                    storeByte(binary(spv::OpIAdd, u64, pristine, offset), current);
                });
            });
            EmitIfCondition(state, Unary(state, spv::OpLogicalNot, boolean, copying), [&] {
                const auto stored = loadByte(binary(spv::OpIAdd, u64, host, offset));
                EmitIfCondition(state, binary(spv::OpINotEqual, boolean, current, stored), [&] {
                    const auto scope = constant(spv::ScopeDevice);
                    const auto relaxed = constant(spv::MemorySemanticsMaskNone);
                    state.module.AddFunction(spv::OpAtomicIAdd, u32, state.module.AllocateId(), at(wordPointer, offsetBy(table, constant(4u))), scope, relaxed, constant(1u));
                    const auto previous = state.module.AllocateId();
                    state.module.AddFunction(spv::OpAtomicCompareExchange, u32, previous, at(wordPointer, offsetBy(table, constant(8u))), scope, relaxed, relaxed, constant(1u), constant(0u));
                    EmitIfCondition(state, binary(spv::OpIEqual, boolean, previous, constant(0u)), [&] {
                        state.module.AddFunction(spv::OpStore, at(addressPointer, offsetBy(table, constant(16u))), address, spv::MemoryAccessAlignedMask, 8u);
                    });
                });
            });
            EndLoop(state, inner);
            state.module.AddFunction(spv::OpIAdd, u64, nextAddress, address, BdaConstant(state, WorkgroupSize));
            CloseLoop(state, inner);
            EndLoop(state, outer);
            state.module.AddFunction(spv::OpIAdd, u32, nextIndex, index, constant(1u));
            CloseLoop(state, outer);
        });
    });
    state.module.AddFunction(spv::OpReturn);
    state.module.AddFunction(spv::OpFunctionEnd);
    state.module.AddExecutionMode(state.mainFunc, spv::ExecutionModeLocalSize, WorkgroupSize, 1u, 1u);
    state.module.EmitEntryPoint(spv::ExecutionModelGLCompute, state.mainFunc, "main", {workgroup, local});
    return state.module.Finalize();
}

}

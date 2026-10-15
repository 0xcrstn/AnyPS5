#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/BufferReadAnalysis.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace ShaderRecompiler;

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    IrProgram program;
    IrBuilder builder{program};
    IrValue* handle = nullptr;
    IrValue* exec = nullptr;

    Fixture() {
        auto& block = program.CreateBlock();
        program.BlockOrder().push_back(&block);
        BlockInfo info;
        info.terminator.kind = TerminatorKind::Return;
        program.Metadata().blockInfo.push_back(info);
        program.SetEntryBlock(block);
        builder.SetInsertionPoint(block);
        auto& word = builder.Constant(0u);
        handle = &builder.Emit(IrOpcode::GetBufferResource, IrOpcodeType(IrOpcode::GetBufferResource), {&word, &word, &word, &word});
        exec = &builder.ConstantBool(true);
    }

    IrValue& Unknown() {
        return builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    }

    IrValue& Load(IrOpcode opcode, IrValue& index, IrValue& offset, IrValue& soffset) {
        return builder.Emit(opcode, IrOpcodeType(opcode), {handle, &index, &offset, &soffset, exec});
    }

    IrValue& Scalar(IrValue& offset) {
        return builder.Emit(IrOpcode::ReadConstBuffer, IrOpcodeType(IrOpcode::ReadConstBuffer), {handle, &offset});
    }
};

MemoryInfo Memory(std::uint32_t offset, std::uint32_t dwords, std::uint32_t bits = 32u) {
    MemoryInfo memory;
    memory.kind = ResourceKind::Buffer;
    memory.offset = offset;
    memory.dataDwords = dwords;
    memory.dataBits = bits;
    return memory;
}

bool Holds(const BufferReadRanges& ranges, std::uint32_t firstIndex, std::uint32_t lastIndex, std::uint64_t begin, std::uint64_t end) {
    return ranges.bounded && ranges.ranges.size() == 1u && ranges.ranges[0] == BufferReadRange{firstIndex, lastIndex, begin, end};
}

void ConstantOffsets() {
    Fixture fixture;
    BufferReadAnalysis analysis;
    BufferReadRanges ranges;
    auto& load = fixture.Load(IrOpcode::LoadBufferU32x4, fixture.builder.Constant(0u), fixture.builder.Constant(0u), fixture.builder.Constant(16u));
    analysis.Note(ranges, load, Memory(32u, 4u));
    Require(Holds(ranges, 0u, 0u, 48u, 64u), "a four-dword load at soffset 16 and instruction offset 32 does not read bytes 48 to 64");
    auto& byte = fixture.Load(IrOpcode::LoadBufferU8, fixture.builder.Constant(0u), fixture.builder.Constant(3u), fixture.builder.Constant(0u));
    analysis.Note(ranges, byte, Memory(61u, 1u, 8u));
    Require(Holds(ranges, 0u, 0u, 48u, 65u), "a byte load adjoining a range did not extend it to byte 65");
}

void ScalarLoads() {
    Fixture fixture;
    BufferReadAnalysis analysis;
    BufferReadRanges ranges;
    analysis.Note(ranges, fixture.Scalar(fixture.builder.Constant(8u)), Memory(4u, 1u));
    Require(Holds(ranges, 0u, 0u, 12u, 16u), "an s_buffer_load dword at register offset 8 and instruction offset 4 does not read bytes 12 to 16");
    BufferReadRanges unaligned;
    analysis.Note(unaligned, fixture.Scalar(fixture.builder.Constant(6u)), Memory(0u, 1u));
    Require(Holds(unaligned, 0u, 0u, 4u, 8u), "an s_buffer_load dword at byte 6 is not read from its dword-aligned address 4");
    auto& masked = fixture.builder.BitwiseAnd(fixture.Unknown(), fixture.builder.Constant(15u));
    auto& scaled = fixture.builder.ShiftLeftLogical(masked, fixture.builder.Constant(4u));
    auto& offset = fixture.builder.IAdd(scaled, fixture.builder.Constant(64u));
    BufferReadRanges indexed;
    analysis.Note(indexed, fixture.Scalar(offset), Memory(0u, 1u));
    Require(Holds(indexed, 0u, 0u, 64u, 308u), "a scalar load at 64 + (x & 15) * 16 does not read bytes 64 to 308");
}

void BoundedIndex() {
    Fixture fixture;
    BufferReadAnalysis analysis;
    BufferReadRanges ranges;
    auto& index = fixture.builder.Emit(IrOpcode::UMin32, IrType::U32, {&fixture.Unknown(), &fixture.builder.Constant(7u)});
    analysis.Note(ranges, fixture.Load(IrOpcode::LoadBufferU32, index, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(Holds(ranges, 0u, 7u, 0u, 4u), "a dword load at index min(x, 7) does not read records 0 to 7");
    auto& field = fixture.builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&fixture.Unknown(), &fixture.builder.Constant(0u), &fixture.builder.Constant(2u)});
    BufferReadRanges extracted;
    analysis.Note(extracted, fixture.Load(IrOpcode::LoadBufferU32x2, field, fixture.builder.Constant(8u), fixture.builder.Constant(0u)), Memory(0u, 2u));
    Require(Holds(extracted, 0u, 3u, 8u, 16u), "a two-dword load at index bits(x, 0, 2) and offset 8 does not read records 0 to 3, bytes 8 to 16");
}

void Unbounded() {
    Fixture fixture;
    BufferReadAnalysis analysis;
    BufferReadRanges index;
    analysis.Note(index, fixture.Load(IrOpcode::LoadBufferU32, fixture.Unknown(), fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(index.bounded && index.ranges.size() == 1u && index.ranges[0].firstIndex == 0u && index.ranges[0].lastIndex == BufferReadRanges::AnyIndex && index.ranges[0].end == 4u && index.ranges[0].indexOrigin == BufferIndexOrigin::Other, "a load at an unbounded index does not read bytes 0 to 4 of any record of another origin");
    BufferReadRanges loaded;
    analysis.Note(loaded, fixture.Load(IrOpcode::LoadBufferU32, fixture.Scalar(fixture.builder.Constant(0u)), fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(loaded.bounded && loaded.ranges.size() == 1u && loaded.ranges[0].lastIndex == BufferReadRanges::AnyIndex && loaded.ranges[0].indexOrigin == BufferIndexOrigin::Memory, "a load at an index read from memory was not marked with that origin");
    BufferReadRanges offset;
    analysis.Note(offset, fixture.Load(IrOpcode::LoadBufferU32, fixture.builder.Constant(0u), fixture.builder.IAdd(fixture.Unknown(), fixture.builder.Constant(4u)), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(!offset.bounded && offset.unbound == BufferReadUnbound::Offset, "a load at an unbounded offset was given a range or another reason");
    BufferReadRanges carried;
    analysis.Note(carried, fixture.Scalar(fixture.builder.Constant(0xfffffff0u)), Memory(0x20u, 1u));
    Require(!carried.bounded && carried.unbound == BufferReadUnbound::Wrap, "a scalar load whose offset wraps past 32 bits was given a range or another reason");
    BufferReadRanges formatted;
    auto memory = Memory(0u, 1u);
    memory.formatted = true;
    analysis.Note(formatted, fixture.Load(IrOpcode::LoadBufferU32, fixture.builder.Constant(0u), fixture.builder.Constant(0u), fixture.builder.Constant(0u)), memory);
    Require(Holds(formatted, 0u, 0u, 0u, FormattedElementBytes), "a formatted load at offset 0 does not read the 16 bytes of its largest element");
    BufferReadRanges typedOffset;
    auto typed = Memory(8u, 1u);
    typed.formatted = true;
    typed.typed = true;
    analysis.Note(typedOffset, fixture.Load(IrOpcode::LoadBufferU32, fixture.builder.Emit(IrOpcode::UMin32, IrType::U32, {&fixture.Unknown(), &fixture.builder.Constant(3u)}), fixture.builder.Constant(32u), fixture.builder.Constant(0u)), typed);
    Require(Holds(typedOffset, 0u, 3u, 40u, 56u), "a typed load at index min(x, 3) and offset 32 + 8 does not read records 0 to 3, bytes 40 to 56");
    BufferReadRanges sticky;
    analysis.Note(sticky, fixture.Scalar(fixture.Unknown()), Memory(0u, 1u));
    analysis.Note(sticky, fixture.Scalar(fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(!sticky.bounded && sticky.ranges.empty() && sticky.unbound == BufferReadUnbound::ScalarOffset, "a later bounded load narrowed a buffer already read without bound, or changed its reason");
    BufferReadRanges written;
    analysis.Note(written, fixture.builder.Emit(IrOpcode::StoreBufferU32, IrType::Void, {fixture.handle, &fixture.builder.Constant(0u), &fixture.builder.Constant(0u), &fixture.builder.Constant(0u), &fixture.builder.Constant(0u), fixture.exec}), Memory(0u, 1u));
    Require(written.bounded && written.ranges.empty(), "a store was counted as a read");
}

void DrawIndices() {
    Fixture fixture;
    BufferReadAnalysis analysis;
    auto& vertex = fixture.builder.Emit(IrOpcode::GetBuiltin, IrOpcodeType(IrOpcode::GetBuiltin), {&fixture.builder.Constant(static_cast<std::uint32_t>(StageInputKind::VertexIndex)), &fixture.builder.Constant(0u)});
    auto& scaled = fixture.builder.ShiftLeftLogical(vertex, fixture.builder.Constant(2u));
    auto& index = fixture.builder.IAdd(scaled, fixture.builder.Constant(1u));
    BufferReadRanges ranges;
    analysis.Note(ranges, fixture.Load(IrOpcode::LoadBufferU32, index, fixture.builder.Constant(8u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(ranges.bounded && ranges.ranges.size() == 1u && ranges.ranges[0] == BufferReadRange{1u, 1u, 8u, 12u, BufferIndexSource::VertexIndex, BufferIndexOrigin::Bounded, 4u}, "a load at index (vertex << 2) + 1 was not recorded as index 4 * vertex + 1");
    auto& instance = fixture.builder.Emit(IrOpcode::GetBuiltin, IrOpcodeType(IrOpcode::GetBuiltin), {&fixture.builder.Constant(static_cast<std::uint32_t>(StageInputKind::InstanceIndex)), &fixture.builder.Constant(0u)});
    auto& times = fixture.builder.IMul(instance, fixture.builder.Constant(6u));
    BufferReadRanges instanced;
    analysis.Note(instanced, fixture.Load(IrOpcode::LoadBufferU32x2, times, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 2u));
    Require(instanced.bounded && instanced.ranges.size() == 1u && instanced.ranges[0] == BufferReadRange{0u, 0u, 0u, 8u, BufferIndexSource::InstanceIndex, BufferIndexOrigin::Bounded, 6u}, "a load at index instance * 6 was not recorded as index 6 * instance");
    auto& divided = fixture.builder.Emit(IrOpcode::UDiv32, IrType::U32, {&vertex, &fixture.builder.Constant(3u)});
    BufferReadRanges halved;
    analysis.Note(halved, fixture.Load(IrOpcode::LoadBufferU32, divided, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(halved.bounded && halved.ranges.size() == 1u && halved.ranges[0].indexSource == BufferIndexSource::Expression && halved.ranges[0].indexExpression == std::vector<BufferIndexNode>{{BufferIndexOp::VertexIndex, 0u, 0u, 0u, 0u}, {BufferIndexOp::Divide, 0u, 0u, 3u, 0u}}, "a load at index vertex / 3 was not recorded as that expression");
    BufferIndexInputs inputs{300u, 599u, 0u, 0u};
    const auto third = EvaluateBufferIndex(halved.ranges[0].indexExpression, inputs);
    Require(third && third->first == 100u && third->second == 199u, "vertex / 3 over vertices 300 to 599 does not give records 100 to 199");
    auto& reciprocal = fixture.builder.Emit(IrOpcode::UMulHi, IrType::U32, {&vertex, &fixture.builder.Constant(0xaaaaaaabu)});
    auto& magic = fixture.builder.ShiftRightLogical(reciprocal, fixture.builder.Constant(1u));
    BufferReadRanges multiplied;
    analysis.Note(multiplied, fixture.Load(IrOpcode::LoadBufferU32, magic, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(multiplied.bounded && multiplied.ranges.size() == 1u && multiplied.ranges[0].indexSource == BufferIndexSource::Expression, "a load at index mulhi(vertex, 0xaaaaaaab) >> 1 was not recorded as an expression");
    const auto magicThird = EvaluateBufferIndex(multiplied.ranges[0].indexExpression, inputs);
    Require(magicThird && magicThird->first == 100u && magicThird->second == 199u, "mulhi(vertex, 0xaaaaaaab) >> 1 over vertices 300 to 599 does not give records 100 to 199");
    auto& fold = fixture.builder.ISub(vertex, fixture.builder.GetUserData(static_cast<ScalarReg>(12)));
    auto& clamped = fixture.builder.Emit(IrOpcode::UMin32, IrType::U32, {&fold, &fixture.builder.Constant(1000u)});
    BufferReadRanges folded;
    analysis.Note(folded, fixture.Load(IrOpcode::LoadBufferU32, clamped, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(folded.bounded && folded.ranges.size() == 1u && folded.ranges[0].indexSource == BufferIndexSource::Expression, "a load at index min(vertex - user word, 1000) was not recorded as an expression");
    BufferIndexInputs foldedInputs{5000u, 5100u, 0u, 0u, 12, 4990u, -1, 0u};
    const auto foldedRange = EvaluateBufferIndex(folded.ranges[0].indexExpression, foldedInputs);
    Require(foldedRange && foldedRange->first == 10u && foldedRange->second == 110u, "min(vertex - folded base vertex, 1000) over vertices 5000 to 5100 with base 4990 does not give records 10 to 110");
    foldedInputs.vertexFoldSgpr = 13;
    Require(!EvaluateBufferIndex(folded.ranges[0].indexExpression, foldedInputs), "a user word other than the folded vertex offset was taken as known at the draw");
    auto& based = fixture.builder.IAdd(fixture.builder.Emit(IrOpcode::UMax32, IrType::U32, {&fixture.builder.GetUserData(static_cast<ScalarReg>(18)), &fixture.builder.Constant(0u)}), vertex);
    BufferReadRanges offsetted;
    analysis.Note(offsetted, fixture.Load(IrOpcode::LoadBufferU32, based, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(offsetted.bounded && offsetted.ranges.size() == 1u && offsetted.ranges[0].indexSource == BufferIndexSource::Expression, "a load at index max(user word, 0) + vertex was not recorded as an expression");
    const std::vector<std::uint32_t> userWords{0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 4096u};
    BufferIndexInputs userInputs{0u, 99u, 0u, 0u};
    userInputs.firstUserSgpr = 8u;
    userInputs.userData = userWords;
    const auto userRange = EvaluateBufferIndex(offsetted.ranges[0].indexExpression, userInputs);
    Require(userRange && userRange->first == 4096u && userRange->second == 4195u, "max(user word 18, 0) + vertex over vertices 0 to 99 with the word at 4096 does not give records 4096 to 4195");
    userInputs.userData = std::span(userWords).first(10u);
    Require(!EvaluateBufferIndex(offsetted.ranges[0].indexExpression, userInputs), "a user word past the draw's user data was taken as known");
    userInputs.userData = userWords;
    userInputs.firstUserSgpr = 19u;
    Require(!EvaluateBufferIndex(offsetted.ranges[0].indexExpression, userInputs), "a user word below the draw's first user SGPR was taken as known");
    auto& extracted = fixture.builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&vertex, &fixture.builder.Constant(2u), &fixture.builder.Constant(4u)});
    BufferReadRanges field;
    analysis.Note(field, fixture.Load(IrOpcode::LoadBufferU32, extracted, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    const auto fieldRange = EvaluateBufferIndex(field.ranges.front().indexExpression, BufferIndexInputs{16u, 23u, 0u, 0u});
    Require(fieldRange && fieldRange->first == 4u && fieldRange->second == 5u, "bits 2 to 5 of vertices 16 to 23 do not give records 4 to 5");
    const auto wrappedField = EvaluateBufferIndex(field.ranges.front().indexExpression, BufferIndexInputs{60u, 70u, 0u, 0u});
    Require(wrappedField && wrappedField->first == 0u && wrappedField->second == 15u, "bits 2 to 5 of vertices crossing a 64 boundary do not give the whole field");
    auto& remainder = fixture.builder.ISub(vertex, fixture.builder.IMul(fixture.builder.Emit(IrOpcode::UDiv32, IrType::U32, {&vertex, &fixture.builder.Constant(3u)}), fixture.builder.Constant(3u)));
    BufferReadRanges modulo;
    analysis.Note(modulo, fixture.Load(IrOpcode::LoadBufferU32, remainder, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(modulo.ranges.front().indexSource == BufferIndexSource::Expression && !EvaluateBufferIndex(modulo.ranges.front().indexExpression, inputs), "vertex - (vertex / 3) * 3, which interval arithmetic cannot bound below, was given a range");
    auto& mixed = fixture.builder.BitwiseXor(vertex, fixture.builder.Constant(5u));
    BufferReadRanges blocked;
    analysis.Note(blocked, fixture.Load(IrOpcode::LoadBufferU32, mixed, fixture.builder.Constant(0u), fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(blocked.ranges.front().lastIndex == BufferReadRanges::AnyIndex && blocked.ranges.front().indexOrigin == BufferIndexOrigin::VertexIndex && blocked.ranges.front().blockingOpcode == static_cast<std::uint16_t>(IrOpcode::BitwiseXor32), "an index stopped by an exclusive or did not name that step");
}

void Collapse() {
    Fixture fixture;
    BufferReadAnalysis analysis;
    BufferReadRanges ranges;
    for (std::uint32_t load = 0; load <= BufferReadRanges::MaxRanges; ++load) analysis.Note(ranges, fixture.Scalar(fixture.builder.Constant(load * 64u)), Memory(0u, 1u));
    Require(Holds(ranges, 0u, 0u, 0u, BufferReadRanges::MaxRanges * 64u + 4u), "more disjoint reads than the range limit did not collapse to their hull");
}

}

int main() {
    try {
        ConstantOffsets();
        ScalarLoads();
        BoundedIndex();
        Unbounded();
        DrawIndices();
        Collapse();
        std::puts("buffer read analysis tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

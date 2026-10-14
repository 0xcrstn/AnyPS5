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
    Require(Holds(index, 0u, BufferReadRanges::AnyIndex, 0u, 4u), "a load at an unbounded index does not read bytes 0 to 4 of any record");
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
    Require(!formatted.bounded && formatted.unbound == BufferReadUnbound::Formatted, "a formatted load was given a range or another reason");
    BufferReadRanges sticky;
    analysis.Note(sticky, fixture.Scalar(fixture.Unknown()), Memory(0u, 1u));
    analysis.Note(sticky, fixture.Scalar(fixture.builder.Constant(0u)), Memory(0u, 1u));
    Require(!sticky.bounded && sticky.ranges.empty() && sticky.unbound == BufferReadUnbound::ScalarOffset, "a later bounded load narrowed a buffer already read without bound, or changed its reason");
    BufferReadRanges written;
    analysis.Note(written, fixture.builder.Emit(IrOpcode::StoreBufferU32, IrType::Void, {fixture.handle, &fixture.builder.Constant(0u), &fixture.builder.Constant(0u), &fixture.builder.Constant(0u), &fixture.builder.Constant(0u), fixture.exec}), Memory(0u, 1u));
    Require(written.bounded && written.ranges.empty(), "a store was counted as a read");
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
        Collapse();
        std::puts("buffer read analysis tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

#include "IntermediateRepresentation/IrBuilder.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ShaderRecompiler;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<const IrValue*> instructions(const IrBlock& block) {
    return {block.Instructions().begin(), block.Instructions().end()};
}

void removesChainsAcrossBlocks() {
    IrProgram program;
    auto& first = program.CreateBlock();
    auto& second = program.CreateBlock();
    program.SetEntryBlock(first);
    program.BlockOrder() = {&first, &second};
    IrBuilder builder(program);
    builder.SetInsertionPoint(first);
    auto& base = builder.IAdd(builder.Constant(1u), builder.Constant(2u));
    auto& deadInFirst = builder.IAdd(base, builder.Constant(3u));
    auto& kept = builder.IAdd(base, builder.Constant(4u));
    builder.SetInsertionPoint(second);
    auto& deadInSecond = builder.IAdd(deadInFirst, deadInFirst);
    auto& deadTail = builder.IAdd(deadInSecond, kept);
    static_cast<void>(deadTail);
    auto& sink = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&kept});
    DeadCodeEliminator{}.Eliminate(program);
    require(instructions(first) == std::vector<const IrValue*>{&base, &kept}, "a dead chain spanning two blocks was not removed in full, or a live value went with it");
    require(instructions(second) == std::vector<const IrValue*>{&sink}, "the dead values of the later block were not removed");
    require(deadInFirst.Parent() == nullptr && deadInSecond.Parent() == nullptr && deadInFirst.Opcode() == IrOpcode::Unreachable, "a removed value kept its block or opcode");
    require(base.Uses() == std::vector<IrValue*>{&kept}, "a surviving value kept a use by a removed one");
    require(kept.Uses() == std::vector<IrValue*>{&sink}, "the side-effect user lost its operand");
}

void keepsSurvivingUseOrder() {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder() = {&block};
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& value = builder.IAdd(builder.Constant(5u), builder.Constant(6u));
    std::vector<IrValue*> live;
    for (std::uint32_t index = 0; index < 6u; ++index) {
        auto& user = builder.IAdd(value, builder.Constant(index));
        if (index % 2u == 0u) static_cast<void>(builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&user}));
        if (index % 2u == 0u) live.push_back(&user);
    }
    DeadCodeEliminator{}.Eliminate(program);
    require(value.Uses() == live, "dead users were not removed from the use list in place, so the survivors changed order");
    for (const IrValue* user : live) require(user->Parent() == &block, "a live user was removed");
}

void staysInsideTheGivenBlocks() {
    IrProgram program;
    auto& outside = program.CreateBlock();
    auto& inside = program.CreateBlock();
    program.SetEntryBlock(outside);
    program.BlockOrder() = {&outside, &inside};
    IrBuilder builder(program);
    builder.SetInsertionPoint(outside);
    auto& source = builder.IAdd(builder.Constant(7u), builder.Constant(8u));
    builder.SetInsertionPoint(inside);
    auto& user = builder.IAdd(source, source);
    static_cast<void>(user);
    const std::vector<IrBlock*> scope{&inside};
    DeadCodeEliminator{}.Eliminate(std::span<IrBlock* const>(scope));
    require(instructions(inside).empty(), "the dead value inside the given blocks was not removed");
    require(instructions(outside) == std::vector<const IrValue*>{&source} && source.Parent() == &outside, "a value outside the given blocks was removed");
    require(!source.HasUses(), "the removed user still used its operand");
}

void keepsCycles() {
    IrProgram program;
    auto& header = program.CreateBlock();
    program.SetEntryBlock(header);
    program.BlockOrder() = {&header};
    auto& phi = program.CreateValue(IrOpcode::Phi, IrType::U32);
    header.AppendInstruction(&phi);
    IrBuilder builder(program);
    builder.SetInsertionPoint(header);
    auto& step = builder.IAdd(phi, builder.Constant(1u));
    phi.AddPhiOperand(&header, &step);
    DeadCodeEliminator{}.Eliminate(program);
    require(instructions(header) == std::vector<const IrValue*>{&phi, &step}, "values that only use each other were removed, which repeated passes never did");
}

}

int main() {
    try {
        removesChainsAcrossBlocks();
        keepsSurvivingUseOrder();
        staysInsideTheGivenBlocks();
        keepsCycles();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_dead_code_tests: %s\n", error.what());
        return 1;
    }
    std::puts("agc_dead_code_tests passed");
    return 0;
}

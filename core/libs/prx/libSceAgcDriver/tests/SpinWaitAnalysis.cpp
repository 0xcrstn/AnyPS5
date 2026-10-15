#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SpinWaitAnalysis.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>

using namespace ShaderRecompiler;

namespace {

struct Loop {
    IrProgram program;
    IrBlock* entry = nullptr;
    IrBlock* header = nullptr;
    IrBlock* body = nullptr;
    IrBlock* exit = nullptr;

    Loop() {
        entry = &block();
        header = &block();
        body = &block();
        exit = &block();
        program.SetEntryBlock(*entry);
        entry->AddBranch(header);
        header->AddBranch(body);
        body->AddBranch(header);
        body->AddBranch(exit);
    }

    IrBlock& block() {
        auto& created = program.CreateBlock();
        program.BlockOrder().push_back(&created);
        program.Metadata().blockInfo.push_back({});
        return created;
    }

    IrValue& constant(std::uint32_t value) {
        auto& created = program.CreateValue(IrOpcode::Void, IrType::U32);
        created.SetImmediateU32(value);
        return created;
    }

    IrValue& emit(IrBlock& block, IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
        auto& created = program.CreateValue(opcode, type);
        for (auto* argument : arguments) created.AddArgument(argument);
        block.AppendInstruction(&created);
        return created;
    }

    void exitWhen(IrValue& condition) {
        program.Metadata().blockInfo[2].condition = &condition;
    }
};

bool run(const char* name, const std::function<bool()>& test) {
    try {
        if (test()) {
            std::printf("[PASS] %s\n", name);
            return true;
        }
        std::printf("[FAIL] %s\n", name);
    } catch (const std::exception& error) {
        std::printf("[FAIL] %s: %s\n", name, error.what());
    }
    return false;
}

}

int main() {
    bool passed = true;

    passed &= run("a loop that exits on a value it loads from memory each iteration spins on memory", [] {
        Loop loop;
        auto& loaded = loop.emit(*loop.body, IrOpcode::LoadAddressU32, IrType::U32, {});
        loop.exitWhen(loop.emit(*loop.body, IrOpcode::IEqual32, IrType::Bool, {&loaded, &loop.constant(0u)}));
        return HasMemorySpinWait(loop.program);
    });

    passed &= run("a loop that exits on its counter does not spin, even when it loads memory", [] {
        Loop loop;
        auto& counter = loop.program.CreateValue(IrOpcode::Phi, IrType::U32);
        loop.header->AppendInstruction(&counter);
        auto& next = loop.emit(*loop.body, IrOpcode::IAdd32, IrType::U32, {&counter, &loop.constant(1u)});
        counter.AddPhiOperand(loop.entry, &loop.constant(0u));
        counter.AddPhiOperand(loop.body, &next);
        static_cast<void>(loop.emit(*loop.body, IrOpcode::LoadAddressU32, IrType::U32, {}));
        loop.exitWhen(loop.emit(*loop.body, IrOpcode::IEqual32, IrType::Bool, {&next, &loop.constant(8u)}));
        return !HasMemorySpinWait(loop.program);
    });

    passed &= run("a loop whose exit depends on a value loaded once before it does not spin", [] {
        Loop loop;
        auto& loaded = loop.emit(*loop.entry, IrOpcode::LoadAddressU32, IrType::U32, {});
        loop.exitWhen(loop.emit(*loop.body, IrOpcode::IEqual32, IrType::Bool, {&loaded, &loop.constant(0u)}));
        return !HasMemorySpinWait(loop.program);
    });

    passed &= run("a loop that carries a loaded value through a phi into its exit spins on memory", [] {
        Loop loop;
        auto& carried = loop.program.CreateValue(IrOpcode::Phi, IrType::U32);
        loop.header->AppendInstruction(&carried);
        auto& loaded = loop.emit(*loop.body, IrOpcode::LoadAddressU32, IrType::U32, {});
        carried.AddPhiOperand(loop.entry, &loop.constant(0u));
        carried.AddPhiOperand(loop.body, &loaded);
        loop.exitWhen(loop.emit(*loop.body, IrOpcode::IEqual32, IrType::Bool, {&carried, &loop.constant(1u)}));
        return HasMemorySpinWait(loop.program);
    });

    passed &= run("a program without loops does not spin", [] {
        IrProgram program;
        auto& only = program.CreateBlock();
        program.BlockOrder().push_back(&only);
        program.Metadata().blockInfo.push_back({});
        program.SetEntryBlock(only);
        return !HasMemorySpinWait(program);
    });

    return passed ? 0 : 1;
}

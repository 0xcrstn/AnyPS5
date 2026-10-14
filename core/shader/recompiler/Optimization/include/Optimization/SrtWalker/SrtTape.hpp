#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTTAPE_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTTAPE_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

class SrtTape {
public:
    struct Operand {
        std::uint32_t slot = 0;
        bool immediate = false;
        std::uint64_t value = 0;
    };
    enum class Step : std::uint8_t { UserData, ShaderBase, Operation, Extract64, AddCarry, RawAddress, RawBuffer };
    struct Instruction {
        Step step = Step::Operation;
        IrOpcode opcode = IrOpcode::Void;
        std::uint8_t count = 0;
        bool loaded = false;
        std::uint32_t index = 0;
        std::int64_t immediate = 0;
        std::array<Operand, 5> arguments{};
    };
    struct Source {
        std::uint32_t dwordCount = 0;
        std::array<Operand, 8> dwords{};
    };
    struct FlatRoot {
        std::uint32_t flatOffset = 0;
        Operand value;
    };

    static std::shared_ptr<const SrtTape> Compile(const IrResourcePlan& plan, std::span<const std::uint32_t> sources);
    static std::shared_ptr<const SrtTape> For(const IrResourcePlan& plan);
    bool Run(const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::vector<std::uint8_t>& activeSources, std::uint32_t& nullRootReads) const;
    std::size_t Instructions() const { return instructions.size(); }
    std::size_t Reads() const { return reads; }

private:
    std::vector<Instruction> instructions;
    std::vector<Source> sources;
    std::vector<FlatRoot> flatRoots;
    std::size_t flatSize = 0;
    std::size_t activeSize = 0;
    std::uint32_t userDataBase = 0;
    std::size_t reads = 0;
};

bool CompiledWalkEnabled();

}

#endif

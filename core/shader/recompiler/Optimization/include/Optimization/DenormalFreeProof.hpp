#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DENORMALFREEPROOF_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DENORMALFREEPROOF_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>
#include <vector>

namespace ShaderRecompiler {

[[nodiscard]] const IrValue* MaskedOperand(const IrValue* value, IrOpcode opcode, std::uint32_t bits);
[[nodiscard]] IrValue* DenormalFlushSource(const IrValue& value);
[[nodiscard]] bool DenormalFreeConstant(std::uint32_t bits);

class DenormalFreeProof {
public:
    explicit DenormalFreeProof(IrProgram& program);
    [[nodiscard]] bool Contains(const IrValue* value) const;

private:
    enum class State : std::uint8_t { Untracked, Free, NotFree };
    struct Entry {
        const IrValue* value = nullptr;
        State state = State::Untracked;
    };

    [[nodiscard]] bool evaluate(const IrValue& value) const;
    [[nodiscard]] State stateOf(const IrValue* value) const;

    std::vector<Entry> state;
};

}

#endif

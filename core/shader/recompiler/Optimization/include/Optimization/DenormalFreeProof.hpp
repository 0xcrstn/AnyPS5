#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DENORMALFREEPROOF_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DENORMALFREEPROOF_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>
#include <unordered_map>

namespace ShaderRecompiler {

[[nodiscard]] const IrValue* MaskedOperand(const IrValue* value, IrOpcode opcode, std::uint32_t bits);
[[nodiscard]] IrValue* DenormalFlushSource(const IrValue& value);
[[nodiscard]] bool DenormalFreeConstant(std::uint32_t bits);

class DenormalFreeProof {
public:
    explicit DenormalFreeProof(IrProgram& program);
    [[nodiscard]] bool Contains(const IrValue* value) const;

private:
    [[nodiscard]] bool evaluate(const IrValue& value) const;

    std::unordered_map<const IrValue*, bool> state;
};

}

#endif

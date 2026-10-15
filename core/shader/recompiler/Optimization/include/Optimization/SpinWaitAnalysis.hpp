#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_SPINWAITANALYSIS_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_SPINWAITANALYSIS_HPP

#include "IntermediateRepresentation/IrProgram.hpp"

namespace ShaderRecompiler {

[[nodiscard]] bool HasMemorySpinWait(const IrProgram& program);

}

#endif

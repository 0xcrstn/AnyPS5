#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_INTERPOLATIONFACTSANNOTATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_INTERPOLATIONFACTSANNOTATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "RewriteSwitch.hpp"
#include <cstdint>

namespace ShaderRecompiler {

struct InterpolationFactsStats {
    std::uint32_t specializedOperations = 0;
    std::uint32_t provenInputs = 0;
    std::uint32_t checkedInputs = 0;
};

class InterpolationFactsAnnotator {
public:
    [[nodiscard]] InterpolationFactsStats Annotate(IrProgram& program) const;
    [[nodiscard]] InterpolationFactsStats Annotate(IrProgram& program, RewriteMode mode) const;
};

}

#endif

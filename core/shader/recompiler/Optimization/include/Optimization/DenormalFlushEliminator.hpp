#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DENORMALFLUSHELIMINATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DENORMALFLUSHELIMINATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "RewriteSwitch.hpp"
#include <cstdint>

namespace ShaderRecompiler {

struct DenormalFlushEliminationStats {
    std::uint32_t removedFlushes = 0;
    std::uint32_t checkedFlushes = 0;
};

class DenormalFlushEliminator {
public:
    [[nodiscard]] DenormalFlushEliminationStats Eliminate(IrProgram& program) const;
    [[nodiscard]] DenormalFlushEliminationStats Eliminate(IrProgram& program, RewriteMode proofs) const;
};

}

#endif

#ifndef CORE_SHADER_RECOMPILER_SPIRVINTERN_HPP
#define CORE_SHADER_RECOMPILER_SPIRVINTERN_HPP

#include "Recompiler.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ShaderRecompiler {

struct SpirvInternCounters {
    std::uint64_t lookups = 0;
    std::uint64_t shared = 0;
    std::uint64_t sharedBytes = 0;
    std::uint64_t collisions = 0;
    std::size_t entries = 0;
};

[[nodiscard]] bool SpirvInternEnabled();

[[nodiscard]] SharedSpirv InternSpirv(std::vector<std::uint32_t> words, bool enabled = SpirvInternEnabled());

[[nodiscard]] SpirvInternCounters SpirvInternTotals();

}

#endif

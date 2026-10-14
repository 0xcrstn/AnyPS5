#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_INCLUDE_OPTIMIZATION_BUFFERREADANALYSIS_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_INCLUDE_OPTIMIZATION_BUFFERREADANALYSIS_HPP

#include "BufferReadRanges.hpp"
#include "IntermediateRepresentation/IrMetadata/ResourceInfo.hpp"
#include "IntermediateRepresentation/IrValue.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ShaderRecompiler {

class BufferReadAnalysis {
public:
    struct Interval {
        std::uint64_t low = 0;
        std::uint64_t high = 0;
    };

    std::optional<Interval> Of(const IrValue* value);
    void Note(BufferReadRanges& ranges, const IrValue& inst, const MemoryInfo& memory);

private:
    static constexpr std::size_t MaxDepth = 64;
    std::optional<Interval> compute(const IrValue& value);
    std::unordered_map<const IrValue*, std::optional<Interval>> intervals;
    std::unordered_set<const IrValue*> visiting;
};

}

#endif

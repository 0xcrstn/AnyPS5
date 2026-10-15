#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_INCLUDE_OPTIMIZATION_BUFFERREADANALYSIS_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_INCLUDE_OPTIMIZATION_BUFFERREADANALYSIS_HPP

#include "BufferReadRanges.hpp"
#include "IntermediateRepresentation/IrMetadata/ResourceInfo.hpp"
#include "IntermediateRepresentation/IrMetadata/StageIO.hpp"
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

    struct Affine {
        BufferIndexSource source = BufferIndexSource::None;
        std::uint64_t scale = 0;
        std::uint64_t low = 0;
        std::uint64_t high = 0;
    };

    std::optional<Interval> Of(const IrValue* value);
    std::optional<Affine> AffineOf(const IrValue* value, std::uint32_t depth = 0);
    BufferIndexOrigin OriginOf(const IrValue* value);
    std::optional<std::uint8_t> BuildIndex(const IrValue* value, std::vector<BufferIndexNode>& nodes, std::uint16_t& blocking, std::uint32_t depth = 0);
    std::optional<std::uint8_t> BuildFloatIndex(const IrValue* value, std::vector<BufferIndexNode>& nodes, std::uint16_t& blocking, std::uint32_t depth);
    void Note(BufferReadRanges& ranges, const IrValue& inst, const MemoryInfo& memory);

private:
    static constexpr std::uint32_t MaxAffineDepth = 16;
    static constexpr std::size_t MaxOriginValues = 64;
    static constexpr std::size_t MaxIndexNodes = 32;
    static constexpr std::size_t MaxDepth = 64;
    std::optional<Interval> compute(const IrValue& value);
    std::unordered_map<const IrValue*, std::optional<Interval>> intervals;
    std::unordered_set<const IrValue*> visiting;
};

}

#endif

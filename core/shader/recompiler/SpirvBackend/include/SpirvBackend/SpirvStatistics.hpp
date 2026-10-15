#ifndef CORE_SHADER_RECOMPILER_SPIRVBACKEND_SPIRVSTATISTICS_HPP
#define CORE_SHADER_RECOMPILER_SPIRVBACKEND_SPIRVSTATISTICS_HPP

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace ShaderRecompiler {

struct SpirvStatistics {
    std::uint64_t words = 0;
    std::uint64_t instructions = 0;
    std::uint64_t functions = 0;
    std::uint64_t calls = 0;
    std::uint64_t float64Arithmetic = 0;
    std::uint64_t conversions = 0;
    std::uint64_t shuffles = 0;
    std::uint64_t ballots = 0;
    std::uint64_t otherGroupOperations = 0;
    std::uint64_t controlBarriers = 0;
    std::uint64_t memoryBarriers = 0;
    std::uint64_t functionArrays = 0;
    std::uint64_t physicalLoads = 0;
    std::uint64_t physicalStores = 0;
    std::uint64_t atomics = 0;
    std::uint64_t loops = 0;
    std::uint64_t selections = 0;
    std::uint64_t lines = 0;
    std::map<std::string, std::uint64_t> callsByCallee;
    std::map<std::string, std::uint64_t> instructionsByFunction;
};

SpirvStatistics AnalyzeSpirv(std::span<const std::uint32_t> words);
std::string SpirvStatisticsJson(const SpirvStatistics& statistics);

}

#endif

#ifndef CORE_SHADER_RECOMPILER_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVMODULESHAPE_HPP
#define CORE_SHADER_RECOMPILER_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVMODULESHAPE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ShaderRecompiler {

struct SpirvModuleShape {
    std::size_t words = 0;
    std::size_t functions = 0;
    std::size_t entryFunctionWords = 0;
    std::size_t helperFunctionWords = 0;
    std::size_t largestHelperWords = 0;
    std::size_t switches = 0;
    std::size_t switchTargets = 0;
    std::size_t largestSwitchTargets = 0;
    std::size_t functionCalls = 0;
};

SpirvModuleShape DescribeSpirvShape(std::span<const std::uint32_t> words);

std::string DescribeSpirvInterface(std::span<const std::uint32_t> words);

std::optional<std::string> CompareSpirvInterfaces(std::span<const std::uint32_t> reference, std::span<const std::uint32_t> candidate);

}

#endif

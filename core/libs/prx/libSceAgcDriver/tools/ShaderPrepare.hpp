#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TOOLS_SHADERPREPARE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TOOLS_SHADERPREPARE_HPP

#include "Recompiler.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::ShaderPreparationTool {

enum class TargetSelection { Recorded, Current };

struct Options {
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path pipelineRecipes;
    std::filesystem::path recipeOutput;
    TargetSelection target = TargetSelection::Recorded;
    std::uint32_t jobs = 1;
    std::uint64_t inputMemoryBytes = 64u * 1024u * 1024u;
    std::uint64_t maxEntryBytes = 16u * 1024u * 1024u;
    std::optional<ShaderRecompiler::ShaderStage> stage;
    std::string identityPrefix;
    bool force = false;
    bool native = false;
    bool help = false;
};

struct Summary {
    std::uint64_t discovered = 0;
    std::uint64_t selected = 0;
    std::uint64_t compiled = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t materialized = 0;
    std::uint64_t filtered = 0;
    std::uint64_t failed = 0;
    std::uint64_t recipesDiscovered = 0;
    std::uint64_t recipesRefreshed = 0;
    std::uint64_t recipesFailed = 0;
    std::uint64_t nativeCompiled = 0;
    std::uint64_t nativeReady = 0;
    bool cancelled = false;
    [[nodiscard]] bool Complete() const;
};

[[nodiscard]] Options ParseOptions(std::span<const std::string_view> arguments);
[[nodiscard]] bool PathsOverlap(const std::filesystem::path& left, const std::filesystem::path& right);
void ConfigureOutput(const Options& options);
[[nodiscard]] Summary Run(const Options& options, VulkanDevice* device = nullptr);
void Cancel();
void PrintUsage();

}

#endif

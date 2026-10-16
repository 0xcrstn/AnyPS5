#include "ShaderPrepare.hpp"
#include <charconv>
#include <cstdio>
#include <cwctype>
#include <stdexcept>
#include <utility>

namespace AgcDriver::ShaderPreparationTool {

namespace {

std::uint32_t number(std::string_view text, std::uint32_t maximum) {
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0u || value > maximum) throw std::invalid_argument("option value is outside its supported range");
    return value;
}

ShaderRecompiler::ShaderStage parseStage(std::string_view value) {
    using ShaderRecompiler::ShaderStage;
    if (value == "compute") return ShaderStage::Compute;
    if (value == "vertex") return ShaderStage::Vertex;
    if (value == "fragment") return ShaderStage::Fragment;
    if (value == "local") return ShaderStage::Local;
    if (value == "mesh") return ShaderStage::Mesh;
    if (value == "geometry") return ShaderStage::Geometry;
    if (value == "tessellation-control") return ShaderStage::TessellationControl;
    if (value == "tessellation-evaluation") return ShaderStage::TessellationEvaluation;
    throw std::invalid_argument("unknown shader stage");
}

}

Options ParseOptions(std::span<const std::string_view> arguments) {
    Options options;
    bool selectedTarget = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        const auto value = [&]() {
            if (++i == arguments.size() || arguments[i].empty() || arguments[i].starts_with("--")) throw std::invalid_argument("missing option value");
            return arguments[i];
        };
        if (argument == "--help") options.help = true;
        else if (argument == "--input") options.input = value();
        else if (argument == "--output") options.output = value();
        else if (argument == "--pipeline-recipes") options.pipelineRecipes = value();
        else if (argument == "--recipe-output") options.recipeOutput = value();
        else if (argument == "--target") {
            const auto target = value();
            if (selectedTarget) throw std::invalid_argument("target must be specified exactly once");
            selectedTarget = true;
            if (target == "recorded") options.target = TargetSelection::Recorded;
            else if (target == "current") options.target = TargetSelection::Current;
            else throw std::invalid_argument("target must be recorded or current");
        } else if (argument == "--jobs") options.jobs = number(value(), 16u);
        else if (argument == "--input-memory-mib") options.inputMemoryBytes = static_cast<std::uint64_t>(number(value(), 512u)) * 1024u * 1024u;
        else if (argument == "--max-entry-mib") options.maxEntryBytes = static_cast<std::uint64_t>(number(value(), 64u)) * 1024u * 1024u;
        else if (argument == "--stage") options.stage = parseStage(value());
        else if (argument == "--id-prefix") options.identityPrefix = value();
        else if (argument == "--force") options.force = true;
        else if (argument == "--native") options.native = true;
        else throw std::invalid_argument("unknown option: " + std::string(argument));
    }
    if (options.help) return options;
    if (options.input.empty() || options.output.empty() || !selectedTarget) throw std::invalid_argument("--input, --output and explicit --target are required");
    if (options.maxEntryBytes > options.inputMemoryBytes) throw std::invalid_argument("maximum entry size exceeds the in-flight input budget");
    if (options.pipelineRecipes.empty() != options.recipeOutput.empty()) throw std::invalid_argument("--pipeline-recipes and --recipe-output must be used together");
    if (!options.pipelineRecipes.empty() && options.target != TargetSelection::Current) throw std::invalid_argument("recipe refresh requires --target current");
    if (options.native && options.pipelineRecipes.empty()) throw std::invalid_argument("--native requires --pipeline-recipes and --recipe-output");
    if (!options.pipelineRecipes.empty() && (options.stage || !options.identityPrefix.empty())) throw std::invalid_argument("recipe refresh cannot be combined with input filters");
    return options;
}

bool PathsOverlap(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto first = std::filesystem::weakly_canonical(left);
    const auto second = std::filesystem::weakly_canonical(right);
    const auto contains = [](const auto& parent, const auto& child) {
        auto a = parent.begin();
        auto b = child.begin();
        while (a != parent.end() && b != child.end()) {
#ifdef _WIN32
            auto leftComponent = a->native();
            auto rightComponent = b->native();
            for (auto& character : leftComponent) character = std::towlower(character);
            for (auto& character : rightComponent) character = std::towlower(character);
            if (leftComponent != rightComponent) return false;
#else
            if (*a != *b) return false;
#endif
            ++a;
            ++b;
        }
        return a == parent.end();
    };
    return contains(first, second) || contains(second, first);
}

bool Summary::Complete() const {
    return selected != 0u && compiled + cacheHits == selected && materialized == selected && failed == 0u && !cancelled && recipesFailed == 0u && recipesRefreshed == recipesDiscovered && nativeReady == nativeCompiled;
}

void PrintUsage() {
    std::fputs("usage: agc_shader_prepare --input <inventory> --output <shader-cache-root> --target recorded|current\n"
               "  [--jobs 1..16] [--input-memory-mib 1..512] [--max-entry-mib 1..64]\n"
               "  [--stage <stage>] [--id-prefix <prefix>] [--force]\n"
               "  [--pipeline-recipes <directory> --recipe-output <directory> [--native]]\n"
               "Captured inputs are preserved. Output uses the current compiler's production cache keys.\n"
               "Recipe refresh requires the current device. --native builds exact renderer pipelines.\n"
               "The input budget bounds serialized inputs in flight, not total compiler memory.\n"
               "Ctrl+C stops admission; active compiler calls finish before shutdown.\n", stdout);
}

}

#include "../tools/ShaderPrepare.hpp"
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace AgcDriver::ShaderPreparationTool;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<std::size_t N>
void rejects(const std::array<std::string_view, N>& arguments) {
    bool failed = false;
    try {
        static_cast<void>(ParseOptions(arguments));
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    require(failed, "invalid preparation options were accepted");
}

}

int main() {
    try {
        constexpr std::array<std::string_view, 6> basic{"--input", "inputs", "--output", "outputs", "--target", "recorded"};
        const auto defaults = ParseOptions(basic);
        require(defaults.target == TargetSelection::Recorded && defaults.jobs == 1u, "recorded target defaults changed");
        require(defaults.maxEntryBytes <= defaults.inputMemoryBytes, "default input limit cannot be admitted");
        constexpr std::array<std::string_view, 12> filtered{"--input", "inputs", "--output", "outputs", "--target", "current", "--stage", "fragment", "--jobs", "4", "--id-prefix", "abcd"};
        const auto options = ParseOptions(filtered);
        require(options.target == TargetSelection::Current && options.jobs == 4u && options.stage == ShaderRecompiler::ShaderStage::Fragment && options.identityPrefix == "abcd", "explicit options were lost");
        rejects(std::array<std::string_view, 4>{"--input", "inputs", "--output", "outputs"});
        rejects(std::array<std::string_view, 8>{"--input", "inputs", "--output", "outputs", "--target", "recorded", "--jobs", "0"});
        rejects(std::array<std::string_view, 8>{"--input", "inputs", "--output", "outputs", "--target", "recorded", "--jobs", "17"});
        rejects(std::array<std::string_view, 8>{"--input", "inputs", "--output", "outputs", "--target", "recorded", "--jobs", "1junk"});
        rejects(std::array<std::string_view, 8>{"--input", "inputs", "--output", "outputs", "--target", "recorded", "--target", "current"});
        rejects(std::array<std::string_view, 8>{"--input", "inputs", "--output", "outputs", "--target", "recorded", "--input-memory-mib", "1"});
        rejects(std::array<std::string_view, 7>{"--input", "inputs", "--output", "outputs", "--target", "current", "--native"});
        rejects(std::array<std::string_view, 10>{"--input", "inputs", "--output", "outputs", "--target", "recorded", "--pipeline-recipes", "recipes", "--recipe-output", "refreshed"});
        rejects(std::array<std::string_view, 12>{"--input", "inputs", "--output", "outputs", "--target", "current", "--pipeline-recipes", "recipes", "--recipe-output", "refreshed", "--stage", "vertex"});
        const auto directory = std::filesystem::temp_directory_path() / "aps5-prepare-options";
        require(PathsOverlap(directory, directory / "child"), "nested output escaped input protection");
        require(PathsOverlap(directory / "child", directory), "parent output escaped input protection");
        require(PathsOverlap(directory / "child" / "..", directory), "relative path components escaped input protection");
        require(!PathsOverlap(directory / "inputs", directory / "outputs"), "sibling paths were treated as overlapping");
#ifdef _WIN32
        require(PathsOverlap(directory / "INPUTS", directory / "inputs" / "child"), "path case escaped input protection");
#endif
        Summary summary;
        require(!summary.Complete(), "empty inventory reported success");
        summary.selected = 2u;
        summary.compiled = 1u;
        summary.cacheHits = 1u;
        require(!summary.Complete(), "unmaterialized cache hits reported success");
        summary.materialized = 2u;
        require(summary.Complete(), "complete translated inputs were rejected");
        summary.recipesDiscovered = 1u;
        require(!summary.Complete(), "unrefreshed recipes reported success");
        summary.recipesRefreshed = 1u;
        require(summary.Complete(), "complete recipe refresh was rejected");
        summary.nativeCompiled = 1u;
        require(!summary.Complete(), "unflushed native compilation reported success");
        summary.nativeReady = 1u;
        require(summary.Complete(), "persisted native compilation was rejected");
        summary.cancelled = true;
        require(!summary.Complete(), "cancelled work reported success");
        summary.cancelled = false;
        summary.failed = 1u;
        require(!summary.Complete(), "partial failure reported success");
        std::cout << "shader preparation option tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

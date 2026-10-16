#include "../tools/ShaderPrepare.hpp"
#include "BdaAbi.hpp"
#include "ShaderDiskCache.hpp"
#include "ShaderInputInventory.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::ShaderPreparationTool;
using namespace ShaderRecompiler;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    require(_putenv_s(name, value) == 0, "cannot configure test environment");
#else
    require(setenv(name, value, 1) == 0, "cannot configure test environment");
#endif
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "cannot read captured test input");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}

int main() {
    try {
        setEnvironment("ANYPS5_NO_SHADER_CACHE", "0");
        setEnvironment("APS5_SINGLE_LANE", "");
        const auto directory = std::filesystem::temp_directory_path() / ("aps5-shader-prepare-" + std::to_string(std::random_device{}()));
        const auto inputs = directory / "inputs";
        std::filesystem::create_directories(inputs);
        const std::array<std::uint32_t, 1> code{0xbf810000u};
        const std::array<std::uint32_t, 1> capabilities{1u};
        RecompileRequest request{};
        request.shader = {ShaderStage::Compute, 0x10000u, code, 0, {}};
        request.context.waveSize = 32u;
        request.context.compute = ShaderComputeStageInfo{{32u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32u;
        request.target.bdaAbiVersion = BdaAbi::Version;
        request.target.supportedCapabilities = capabilities;
        request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
        request.target.maxWorkgroupInvocations = 1024u;
        request.target.maxWorkgroupSharedMemoryBytes = 32768u;
        request.layout = {0u, 0u, 0u, 128u};
        ShaderInputInventory inventory(inputs);
        require(inventory.Store(request) == ShaderInputCaptureStatus::Stored, "cannot store first test input");
        request.context.waveSize = 64u;
        request.context.compute->numThreads[0] = 64u;
        require(inventory.Store(request) == ShaderInputCaptureStatus::Stored, "cannot store second test input");
        const auto records = inventory.List();
        require(records.size() == 2u, "test inputs were not distinct");
        std::vector<std::string> originals;
        for (const auto& record : records) originals.push_back(read(record.path));
        Options options;
        options.input = inputs;
        options.output = directory / "cache";
        options.jobs = 2u;
        const auto first = Run(options);
        require(first.Complete() && first.compiled == 2u && first.cacheHits == 0u && first.materialized == 2u, "cold preparation did not produce complete translated outputs");
        for (const auto& record : records) {
            auto owned = inventory.Load(record.path);
            std::vector<std::byte> key;
            ShaderDiskCache::BuildKey(owned.request, owned.request.target.subgroupSize, key);
            CompiledVariant variant;
            require(ShaderDiskCache::Load(key, variant) && !variant.artifact.spirv.empty(), "prepared output is not readable by the production cache");
        }
        const auto second = Run(options);
        require(second.Complete() && second.compiled == 0u && second.cacheHits == 2u && second.materialized == 2u, "incremental preparation did not reuse current exact outputs");
        {
            auto owned = inventory.Load(records.front().path);
            std::vector<std::byte> key;
            ShaderDiskCache::BuildKey(owned.request, owned.request.target.subgroupSize, key);
            const auto path = ShaderDiskCache::EntryDirectory() / ShaderDiskCache::EntryName(key);
            auto outdated = read(path);
            require(outdated.size() > 16u, "cache entry header is truncated");
            outdated[8] ^= 0x40;
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(outdated.data(), static_cast<std::streamsize>(outdated.size()));
            require(static_cast<bool>(file), "cannot invalidate the output source version");
        }
        const auto rebuilt = Run(options);
        require(rebuilt.Complete() && rebuilt.compiled == 1u && rebuilt.cacheHits == 1u, "stale source output was reused instead of regenerated");
        options.force = true;
        const auto forced = Run(options);
        require(forced.Complete() && forced.compiled == 2u && forced.cacheHits == 0u, "forced preparation reused output artifacts");
        options.force = false;
        options.identityPrefix = "no-match";
        const auto filtered = Run(options);
        require(!filtered.Complete() && filtered.selected == 0u && filtered.filtered == 2u, "fully filtered corpus reported success");
        options.identityPrefix.clear();
        options.stage = ShaderStage::Fragment;
        const auto wrongStage = Run(options);
        require(!wrongStage.Complete() && wrongStage.filtered == 2u, "fully stage-filtered corpus reported success");
        options.stage.reset();
        const auto empty = directory / "empty";
        std::filesystem::create_directories(empty);
        options.input = empty;
        require(!Run(options).Complete(), "empty corpus reported success");
        options.input = inputs;
        for (std::size_t i = 0; i < records.size(); ++i) require(read(records[i].path) == originals[i], "preparation changed the input corpus");
        Cancel();
        const auto cancelled = Run(options);
        require(cancelled.cancelled && !cancelled.Complete(), "cancellation reported complete coverage");
        ShaderDiskCache::Flush();
        std::filesystem::remove_all(directory);
        std::cout << "shader preparation cache tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "CompiledVariant.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "ShaderDiskCache.hpp"
#include "ShaderInputInventory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeStore.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace {

using namespace ShaderRecompiler;
using namespace AgcDriver::Graphics;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class Directory {
public:
    Directory() {
        path = std::filesystem::temp_directory_path() / ("anyps5-pipeline-input-bridge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Require(std::filesystem::create_directory(path), "could not create pipeline input bridge test directory");
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

void SetInputDirectory(const std::filesystem::path& directory) {
#ifdef _WIN32
    Require(_wputenv_s(L"ANYPS5_SHADER_INPUT_DIR", directory.c_str()) == 0, "could not configure automatic input directory");
    Require(_putenv_s("ANYPS5_NO_SHADER_INPUTS", "0") == 0 && _putenv_s("APS5_NO_SHADER_INPUTS", "0") == 0, "could not enable automatic input capture");
#else
    Require(setenv("ANYPS5_SHADER_INPUT_DIR", directory.c_str(), 1) == 0, "could not configure automatic input directory");
    Require(setenv("ANYPS5_NO_SHADER_INPUTS", "0", 1) == 0 && setenv("APS5_NO_SHADER_INPUTS", "0", 1) == 0, "could not enable automatic input capture");
#endif
}

struct Input {
    std::array<std::uint32_t, 4> code{0x7e000280u, 0xf800180fu, 0u, 0xbf810000u};
    std::array<std::uint32_t, 1> capabilities{1u};
    RecompileRequest Request() const {
        RecompileRequest request{};
        request.shader = {ShaderStage::Fragment, 0x10000u, code, 0u, {}};
        request.context.waveSize = 32u;
        request.context.pixel.emplace();
        request.context.pixel->wave32 = true;
        request.context.pixel->targetOutputMode[0] = 9u;
        request.context.pixel->targetExportMapping[0] = 0xe4u;
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32u;
        request.target.supportedCapabilities = capabilities;
        request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
        request.target.maxWorkgroupInvocations = 1024u;
        request.target.maxWorkgroupSharedMemoryBytes = 32768u;
        request.target.fixedPushSlots = true;
        request.layout = {0u, 0u, 128u, 128u};
        request.useCache = false;
        return request;
    }
};

std::shared_ptr<const RecompileResult> Materialize(const RecompileRequest& request) {
    std::vector<std::byte> key;
    ShaderDiskCache::BuildKey(request, request.target.subgroupSize, key);
    const auto artifact = CompileShaderUncached(request);
    Require(artifact != nullptr, "offline compilation produced no artifact");
    auto result = MaterializeShaderUncached(request, *artifact, key);
    Require(!result.spirv.empty() && result.poisonedSrtReads == 0u && result.imageTableFaults == 0u, "captured input did not materialize a complete shader");
    return std::make_shared<const RecompileResult>(std::move(result));
}

PipelineRecipe Recipe(std::shared_ptr<const RecompileResult> program, const std::string& inputIdentity) {
    PipelineRecipe recipe;
    recipe.state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    recipe.state.hasColorTarget = true;
    ColorTarget color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.exportIndex = 0u;
    recipe.state.colors.push_back(color);
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 15u;
    recipe.state.blends.push_back(blend);
    const auto offset = program->pushConstants.empty() ? 0u : 128u;
    recipe.stages.push_back({ShaderStage::Fragment, offset, std::move(program), inputIdentity});
    return recipe;
}

void RegenerationBridge() {
    Directory directory;
    Input input;
    const auto request = input.Request();
    const auto serialized = RequestSerializer{}.SerializeBinary(request);
    ShaderInputInventory inventory(directory.path / "inputs");
    Require(inventory.Store(request) == ShaderInputCaptureStatus::Stored, "full input was not persisted");
    const auto records = inventory.List();
    Require(records.size() == 1u && records.front().identity.size() == 64u, "inventory did not preserve an exact request identity");
    const auto identity = records.front().identity;
    auto restored = inventory.Load(records.front().path);
    Require(RequestSerializer{}.SerializeBinary(restored.request) == serialized, "inventory replay changed the captured request");
    const auto first = Materialize(restored.request);
    SetInputDirectory(directory.path / "inputs");
    const auto captured = CaptureShaderInput(restored.request, first);
    Require(captured == ShaderInputCaptureStatus::Queued || captured == ShaderInputCaptureStatus::Duplicate, "automatic capture refused its actual materialized result");
    FlushShaderInputs();
    Require(FindShaderInputIdentity(first) == identity, "automatic capture did not associate exact input with its actual result");
    const auto copied = *first;
    Require(FindShaderInputIdentity(copied) == identity, "copying a materialized result lost its input provenance");
    const auto recipe = Recipe(first, identity);
    const auto nativeIdentity = PipelineShaderIdentity({ShaderStage::Fragment, first.get(), recipe.stages.front().pushConstantOffset});
    PipelineRecipeStore recipes(directory.path / "recipes");
    Require(recipes.Store(recipe) == PipelineRecipeStoreStatus::Stored, "materialized recipe was not persisted");
    PipelineRecipe manifest;
    Require(recipes.LoadInputs(PipelineRecipeIdentity(recipe), manifest) == PipelineRecipeStoreStatus::Loaded, "durable input manifest was not loaded");
    Require(manifest.stages.size() == 1u && manifest.stages.front().program == nullptr && manifest.stages.front().inputIdentity == identity, "input manifest retained compiled output or lost its full request reference");
    const auto found = std::ranges::find_if(records, [&](const auto& record) { return record.identity == manifest.stages.front().inputIdentity; });
    Require(found != records.end(), "manifest input association could not be resolved");
    auto replay = inventory.Load(found->path);
    Require(replay.request.shader.stage == manifest.stages.front().stage, "manifest stage disagrees with input");
    manifest.stages.front().program = Materialize(replay.request);
    const auto expectedOffset = manifest.stages.front().program->pushConstants.empty() ? 0u : replay.request.layout.pushConstantOffsetBytes;
    Require(expectedOffset == manifest.stages.front().pushConstantOffset, "manifest normalized push offset disagrees with regenerated input");
    Require(PipelineShaderIdentity({manifest.stages.front().stage, manifest.stages.front().program.get(), manifest.stages.front().pushConstantOffset}) == nativeIdentity, "regeneration produced a different specialized shader identity");
    Require(PipelineRecipeIdentity(manifest) == PipelineRecipeIdentity(recipe), "regenerated native recipe did not match its source");
    Require(PipelineRecipeInputIdentity(manifest) == PipelineRecipeInputIdentity(recipe), "regeneration changed the durable recipe input identity");
    auto changed = replay.request;
    changed.target.fixedPushSlots = false;
    Require(inventory.Store(changed) == ShaderInputCaptureStatus::Stored && inventory.List().size() == 2u, "push ABI disappeared from durable input identity");
    changed = replay.request;
    changed.target.subgroupSize = 64u;
    Require(inventory.Store(changed) == ShaderInputCaptureStatus::Stored && inventory.List().size() == 3u, "subgroup target disappeared from durable input identity");
    std::filesystem::remove(found->path);
    bool missingRejected = false;
    try {
        static_cast<void>(inventory.Load(found->path));
    } catch (const std::exception&) {
        missingRejected = true;
    }
    Require(missingRejected, "missing referenced input silently regenerated a pipeline");
}

}

int main() {
    try {
        RegenerationBridge();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pipeline input bridge test: %s\n", error.what());
        return 1;
    }
}

#include "execution/VulkanTestDevice.hpp"
#include "../tools/ShaderPrepare.hpp"
#include "CompiledVariant.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"
#include "ShaderInputInventory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeStore.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

using namespace ShaderRecompiler;
using namespace AgcDriver::Graphics;
using namespace AgcDriver::ShaderPreparationTool;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class Directory {
public:
    Directory() {
        path = std::filesystem::temp_directory_path() / ("anyps5-native-graphics-preparation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Require(std::filesystem::create_directory(path), "could not create native preparation test directory");
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

void EnableShaderCache() {
#ifdef _WIN32
    Require(_putenv_s("ANYPS5_NO_SHADER_CACHE", "0") == 0, "could not enable production shader cache");
#else
    Require(setenv("ANYPS5_NO_SHADER_CACHE", "0", 1) == 0, "could not enable production shader cache");
#endif
}

std::unique_ptr<AgcDriver::VulkanDevice> OpenCompileDevice() {
    try {
        return std::make_unique<AgcDriver::VulkanDevice>(AgcDriver::VulkanDevice::CompileOnly{});
    } catch (const std::exception& error) {
        const std::string_view message(error.what());
        const bool absent = (message.starts_with("Vulkan loader: ") && message != "Vulkan loader: vkGetInstanceProcAddr missing") || message == "Vulkan: no Vulkan 1.1 graphics and compute queue" || message == "vkCreateInstance: Vulkan result -9";
        if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr || !absent) throw;
        std::printf("skipped, no usable Vulkan device: %s\n", error.what());
        return nullptr;
    }
}

std::shared_ptr<const RecompileResult> Compile(const RecompileRequest& request) {
    const auto artifact = CompileShaderUncached(request);
    Require(artifact != nullptr, "native fixture compilation produced no artifact");
    std::vector<std::byte> key;
    const auto subgroup = request.shader.stage == ShaderStage::Vertex && !request.target.vertexSubgroups ? 1u : request.target.subgroupSize;
    ShaderDiskCache::BuildKey(request, subgroup, key);
    return std::make_shared<const RecompileResult>(MaterializeShaderUncached(request, *artifact, key));
}

std::string StoredIdentity(ShaderInputInventory& inventory, ShaderStage stage) {
    for (const auto& record : inventory.List()) {
        if (inventory.Load(record.path).request.shader.stage == stage) return record.identity;
    }
    throw std::runtime_error("native fixture input was not persisted");
}

void CheckNativeFile(const std::filesystem::path& output) {
    std::size_t files = 0u;
    for (const auto& entry : std::filesystem::directory_iterator(output)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("vk-pipelines-") || entry.path().extension() != ".bin") continue;
        Require(entry.is_regular_file() && entry.file_size() >= 56u, "prepared native cache has no production/Vulkan header");
        ++files;
    }
    Require(files == 1u, "native preparation did not persist its production cache in the requested output directory");
}

void Check(AgcDriver::VulkanDevice& device, Options options) {
    constexpr std::array<std::uint32_t, 7> VertexCode{0x7e000280u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
    constexpr std::array<std::uint32_t, 4> FragmentCode{0x7e0e02f2u, 0xf800180fu, 0x07070707u, 0xbf810000u};
    const auto target = device.Target();
    RecompileRequest vertex{};
    vertex.shader = {ShaderStage::Vertex, 0x10000u, VertexCode, 0u, {}};
    vertex.context.waveSize = 32u;
    vertex.context.vertex.emplace();
    vertex.target = target;
    vertex.layout = {0u, 0u, 0u, PipelinePushSlotBytes};
    vertex.useCache = false;
    const auto vertexResult = Compile(vertex);
    RecompileRequest fragment{};
    fragment.shader = {ShaderStage::Fragment, 0x20000u, FragmentCode, 0u, {}};
    fragment.context.waveSize = 32u;
    fragment.context.pixel.emplace();
    fragment.context.pixel->wave32 = true;
    fragment.context.pixel->targetOutputMode[0] = 9u;
    fragment.context.pixel->targetExportMapping.fill(0xe4u);
    fragment.target = target;
    fragment.layout = PixelPushLayout(static_cast<std::uint32_t>(vertexResult->pushConstants.size()), target);
    fragment.useCache = false;
    const auto fragmentResult = Compile(fragment);
    ShaderInputInventory inventory(options.input);
    Require(inventory.Store(vertex) == ShaderInputCaptureStatus::Stored && inventory.Store(fragment) == ShaderInputCaptureStatus::Stored, "native fixture inputs were not persisted");
    PipelineRecipe recipe;
    recipe.state.stages.path = ShaderPath::Vertex;
    recipe.state.stages.vertexWaveSize = 32u;
    recipe.state.stages.fragmentWaveSize = 32u;
    recipe.state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    recipe.state.hasColorTarget = true;
    ColorTarget color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.componentMapping = 0xe4u;
    recipe.state.colors.push_back(color);
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 15u;
    recipe.state.blends.push_back(blend);
    recipe.stages.push_back({ShaderStage::Vertex, 0u, vertexResult, StoredIdentity(inventory, ShaderStage::Vertex)});
    const auto fragmentOffset = fragmentResult->pushConstants.empty() ? 0u : fragment.layout.pushConstantOffsetBytes;
    recipe.stages.push_back({ShaderStage::Fragment, fragmentOffset, fragmentResult, StoredIdentity(inventory, ShaderStage::Fragment)});
    Require(device.RefreshGraphicsPipelineRecipe(recipe), "renderer refused current exact fixture inputs");
    const auto before = PipelineLibraryCountersOf(device.Device());
    Require(device.PrepareGraphicsPipeline(recipe), "first native preparation failed");
    const auto first = PipelineLibraryCountersOf(device.Device());
    Require(device.PrepareGraphicsPipeline(recipe), "second native preparation failed");
    const auto second = PipelineLibraryCountersOf(device.Device());
    Require(first.built == second.built && first.linked == second.linked, "identical replay rebuilt or relinked native pipeline libraries");
    Require(device.FlushPreparedPipelines(), "native cache checkpoint failed to persist");
    CheckNativeFile(options.output);
    auto incompatible = recipe;
    auto shader = std::make_shared<RecompileResult>(*incompatible.stages.front().program);
    ++shader->runtimeAbiVersion;
    incompatible.stages.front().program = std::move(shader);
    bool rejected = false;
    try {
        device.PrepareGraphicsPipeline(incompatible);
    } catch (const std::exception& error) {
        Require(std::string_view(error.what()).find("incompatible version") != std::string_view::npos, "incompatible recipe failed outside its runtime ABI guard");
        rejected = true;
    }
    Require(rejected, "renderer accepted an incompatible runtime ABI");
    options.pipelineRecipes = options.input.parent_path() / "missing-provenance";
    options.recipeOutput = options.input.parent_path() / "refreshed";
    options.native = true;
    auto incomplete = recipe;
    incomplete.stages.back().inputIdentity.clear();
    PipelineRecipeStore missing(options.pipelineRecipes);
    Require(missing.Store(incomplete) == PipelineRecipeStoreStatus::Stored, "could not persist the missing-provenance negative fixture");
    const auto failed = Run(options, &device);
    Require(!failed.Complete() && failed.recipesFailed == 1u && failed.nativeReady == 0u, "missing provenance silently produced native readiness");
    const bool libraryReuseObserved = first.built != before.built || first.linked != before.linked;
    std::printf("native graphics preparation: current target replay, repeated pipeline, durable cache, ABI refusal and missing provenance; library reuse observed=%u\n", libraryReuseObserved ? 1u : 0u);
}

}

int main() {
    try {
        Directory directory;
        Options options;
        options.input = directory.path / "inputs";
        options.output = directory.path / "cache";
        options.target = TargetSelection::Current;
        std::filesystem::create_directories(options.input);
        EnableShaderCache();
        ConfigureOutput(options);
        auto device = OpenCompileDevice();
        if (device == nullptr) return VulkanTestSkipped;
        Check(*device, options);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native graphics preparation test: %s\n", error.what());
        return 1;
    }
}

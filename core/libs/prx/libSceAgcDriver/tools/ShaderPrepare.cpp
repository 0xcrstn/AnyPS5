#include "ShaderPrepare.hpp"
#include "CompiledVariant.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "ShaderDiskCache.hpp"
#include "ShaderInputInventory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeStore.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeGenerators.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace AgcDriver::ShaderPreparationTool {

namespace {

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> interrupted{false};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    const auto result = _putenv_s(name, value.c_str());
#else
    const auto result = setenv(name, value.c_str(), 1);
#endif
    require(result == 0, "cannot configure shader cache output directory");
}

std::string firstLine(const std::exception& error) {
    const std::string_view message(error.what());
    return std::string(message.substr(0, std::min<std::size_t>(message.find('\n'), 400u)));
}

std::uint32_t hostSubgroupSize(const ShaderRecompiler::RecompileRequest& request) {
    const auto stage = request.shader.stage;
    if ((stage == ShaderRecompiler::ShaderStage::Vertex || stage == ShaderRecompiler::ShaderStage::Local) && !request.target.vertexSubgroups) return 1u;
    return request.target.subgroupSize;
}

struct PreparedInput {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> program;
    bool cacheHit = false;
};

bool loadOutput(std::span<const std::byte> key, ShaderRecompiler::CompiledVariant& variant) {
    using namespace ShaderRecompiler;
    const auto path = ShaderDiskCache::EntryDirectory() / ShaderDiskCache::EntryName(key);
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    require(!error, "cannot inspect translated output entry");
    if (!exists) return false;
    const auto size = std::filesystem::file_size(path);
    require(size <= 64u * 1024u * 1024u, "translated output entry exceeds the offline read limit");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return false;
    if (file.peek() != std::ifstream::traits_type::eof()) return false;
    return ShaderDiskCache::DecodeEntry(bytes, key, variant) == ShaderDiskCache::LoadStatus::Loaded;
}

PreparedInput prepare(ShaderRecompiler::RecompileRequest& request, const Options& options, VulkanDevice* device) {
    using namespace ShaderRecompiler;
    if (options.target == TargetSelection::Current) {
        require(device != nullptr, "current target requires a compile-only Vulkan device");
        const auto target = request.shader.stage == ShaderStage::Compute ? device->ComputeTarget(request.context.waveSize) : device->Target();
        require(request.target.bdaAbiVersion == target.bdaAbiVersion, "captured input uses an incompatible buffer address ABI");
        require(request.target.fixedPushSlots == target.fixedPushSlots, "captured input uses an incompatible push constant ABI");
        request.target = target;
    }
    request.useCache = false;
    std::vector<std::byte> key;
    ShaderDiskCache::BuildKey(request, hostSubgroupSize(request), key);
    std::shared_ptr<const CompiledVariant> variant;
    CompiledVariant loaded;
    bool hit = false;
    if (!options.force && loadOutput(key, loaded)) {
        loaded.layout = request.layout;
        variant = std::make_shared<const CompiledVariant>(std::move(loaded));
        hit = true;
    } else {
        variant = CompileShaderUncached(request);
        require(variant != nullptr && !variant->artifact.spirv.empty(), "compiler produced no translated artifact");
    }
    auto materialized = MaterializeShaderUncached(request, *variant, key);
    require(!materialized.spirv.empty(), "materialization produced no shader");
    require(materialized.poisonedSrtReads == 0u && materialized.imageTableFaults == 0u, "captured input is incomplete or contains poisoned resource reads");
    if (!hit) require(ShaderDiskCache::Persist(key, *variant) != nullptr, "cannot persist and verify translated artifact");
    return {std::make_shared<const RecompileResult>(std::move(materialized)), hit};
}

void refreshRecipes(const Options& options, VulkanDevice* device, ShaderRecompiler::ShaderInputInventory& inventory, const std::vector<ShaderRecompiler::ShaderInputRecord>& records, Summary& summary) {
    using namespace Graphics;
    if (options.pipelineRecipes.empty()) return;
    require(device != nullptr, "recipe refresh requires a current Vulkan device");
    std::map<std::string, const ShaderRecompiler::ShaderInputRecord*> inputs;
    for (const auto& record : records) inputs.emplace(record.identity, &record);
    PipelineRecipeStore source(options.pipelineRecipes);
    PipelineRecipeStoreLimits outputLimits;
    outputLimits.evict = false;
    PipelineRecipeStore destination(options.recipeOutput, outputLimits);
    const auto identities = source.ListForPreparation(4096u);
    require(!identities.empty(), "no pipeline recipes were discovered");
    require(identities.size() <= 4096u, "pipeline recipe count exceeds the preparation limit");
    summary.recipesDiscovered = identities.size();
    for (const auto& identity : identities) {
        if (interrupted.load(std::memory_order_relaxed)) break;
        try {
            PipelineRecipe recipe;
            require(source.LoadInputs(identity, recipe) == PipelineRecipeStoreStatus::Loaded, "cannot load pipeline recipe input manifest");
            require(!recipe.stages.empty(), "pipeline recipe contains no shader stages");
            for (std::size_t index = 0; index < recipe.stages.size(); ++index) {
                auto& stage = recipe.stages[index];
                if (IsDerivedPipelineStage(recipe, index)) continue;
                const auto found = inputs.find(stage.inputIdentity);
                require(found != inputs.end(), "pipeline recipe references a missing captured input");
                require(found->second->bytes <= options.inputMemoryBytes, "pipeline recipe input exceeds the input budget");
                auto owned = inventory.Load(found->second->path);
                require(owned.request.shader.stage == stage.stage, "pipeline stage disagrees with its captured input");
                stage.program = prepare(owned.request, options, device).program;
                const auto pushOffset = stage.program->pushConstants.empty() ? 0u : owned.request.layout.pushConstantOffsetBytes;
                require(pushOffset == stage.pushConstantOffset, "pipeline stage push constant offset disagrees with its captured input");
            }
            const auto mainLayout = recipe.mainLayoutKey;
            const auto fragmentLayout = recipe.fragmentLayoutKey;
            const auto splitSets = recipe.splitSets;
            require(device->RefreshGraphicsPipelineRecipe(recipe), "renderer refused the refreshed pipeline recipe");
            require(recipe.mainLayoutKey == mainLayout && recipe.fragmentLayoutKey == fragmentLayout && recipe.splitSets == splitSets, "current compiler changes the captured descriptor layout; capture a new pipeline recipe");
            const auto status = destination.Store(recipe);
            require(status == PipelineRecipeStoreStatus::Stored || status == PipelineRecipeStoreStatus::Present, "cannot persist refreshed pipeline recipe");
            if (options.native) {
                require(device->PrepareGraphicsPipeline(recipe), "renderer refused exact native pipeline preparation");
                ++summary.nativeCompiled;
            }
            ++summary.recipesRefreshed;
            std::printf("[shader-prepare] recipe %llu/%llu refreshed native=%u\n", static_cast<unsigned long long>(summary.recipesRefreshed + summary.recipesFailed), static_cast<unsigned long long>(summary.recipesDiscovered), options.native ? 1u : 0u);
        } catch (const std::exception& error) {
            ++summary.recipesFailed;
            std::fprintf(stderr, "[shader-prepare] recipe %s failed: %s\n", identity.c_str(), firstLine(error).c_str());
        }
    }
    if (summary.nativeCompiled != 0u) {
        try {
            require(device->FlushPreparedPipelines(), "cannot persist the prepared native pipeline cache");
            summary.nativeReady = summary.nativeCompiled;
        } catch (const std::exception& error) {
            ++summary.recipesFailed;
            std::fprintf(stderr, "[shader-prepare] native cache checkpoint failed: %s\n", firstLine(error).c_str());
        }
    }
}

}

void Cancel() {
    interrupted.store(true, std::memory_order_relaxed);
}

void ConfigureOutput(const Options& options) {
    using namespace ShaderRecompiler;
    require(!options.input.empty() && !options.output.empty(), "input and output directories are required");
    require(options.jobs > 0u && options.jobs <= 16u, "worker count is outside the supported range");
    require(options.maxEntryBytes > 0u && options.maxEntryBytes <= options.inputMemoryBytes, "entry limit exceeds the in-flight input budget");
    require(options.inputMemoryBytes <= 512u * 1024u * 1024u && options.maxEntryBytes <= 64u * 1024u * 1024u, "input limits exceed the supported range");
    require(std::filesystem::is_directory(options.input), "input inventory directory does not exist");
    require(!PathsOverlap(options.input, options.output), "input inventory and shader cache output must be separate directories");
    if (!options.pipelineRecipes.empty()) {
        require(std::filesystem::is_directory(options.pipelineRecipes), "pipeline recipe input directory does not exist");
        require(!options.recipeOutput.empty(), "pipeline recipe output directory is required");
        require(!PathsOverlap(options.input, options.recipeOutput), "input inventory and recipe output must be separate directories");
        require(!PathsOverlap(options.pipelineRecipes, options.output), "pipeline recipe inputs and shader cache output must be separate directories");
        require(!PathsOverlap(options.pipelineRecipes, options.recipeOutput), "pipeline recipe input and output must be separate directories");
        require(options.target == TargetSelection::Current && !options.stage && options.identityPrefix.empty(), "recipe refresh requires an unfiltered current target");
    }
    require(!options.native || !options.pipelineRecipes.empty(), "native preparation requires pipeline recipes");
    if (const auto* value = std::getenv("APS5_SINGLE_LANE")) require(*value == '\0', "offline preparation does not support APS5_SINGLE_LANE");
    setEnvironment("ANYPS5_SHADER_CACHE_DIR", std::filesystem::weakly_canonical(options.output).string());
}

Summary Run(const Options& options, VulkanDevice* device) {
    using namespace ShaderRecompiler;
    ConfigureOutput(options);
    require(ShaderDiskCache::Enabled(), "shader disk cache is disabled");
    require(std::filesystem::weakly_canonical(ShaderDiskCache::EntryDirectory().parent_path()) == std::filesystem::weakly_canonical(options.output), "shader cache was initialized for a different output directory");
    ShaderInputInventoryOptions limits;
    limits.maxEntryBytes = options.maxEntryBytes;
    ShaderInputInventory inventory(options.input, limits);
    auto records = inventory.List();
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.identity < b.identity; });
    Summary summary;
    summary.discovered = records.size();
    std::printf("[shader-prepare] discovered=%llu jobs=%u input_budget_bytes=%llu source_version=%llx\n", static_cast<unsigned long long>(summary.discovered), options.jobs, static_cast<unsigned long long>(options.inputMemoryBytes), static_cast<unsigned long long>(ShaderDiskCache::SourceVersion()));
    std::mutex mutex;
    std::condition_variable available;
    std::size_t next = 0;
    std::uint64_t admittedBytes = 0;
    const auto worker = [&] {
        while (true) {
            const ShaderInputRecord* record = nullptr;
            std::uint64_t bytes = 0;
            {
                std::unique_lock lock(mutex);
                while (!interrupted.load(std::memory_order_relaxed) && next < records.size()) {
                    const auto& candidate = records[next];
                    if (!candidate.identity.starts_with(options.identityPrefix)) {
                        ++summary.filtered;
                        ++next;
                        continue;
                    }
                    if (candidate.bytes > options.maxEntryBytes || candidate.bytes > options.inputMemoryBytes) {
                        ++summary.failed;
                        ++summary.selected;
                        ++next;
                        std::fprintf(stderr, "[shader-prepare] input %s exceeds admission limits\n", candidate.identity.c_str());
                        continue;
                    }
                    if (candidate.bytes > options.inputMemoryBytes - admittedBytes) {
                        available.wait_for(lock, std::chrono::milliseconds(100));
                        continue;
                    }
                    record = &candidate;
                    bytes = candidate.bytes;
                    admittedBytes += bytes;
                    ++next;
                    break;
                }
                if (record == nullptr) return;
            }
            bool filtered = false;
            bool success = false;
            bool hit = false;
            std::string failure;
            try {
                require(std::filesystem::file_size(record->path) == bytes, "captured input changed after discovery");
                auto owned = inventory.Load(record->path);
                if (options.stage && owned.request.shader.stage != *options.stage) filtered = true;
                else {
                    const auto result = prepare(owned.request, options, device);
                    hit = result.cacheHit;
                    success = true;
                }
            } catch (const std::exception& error) {
                failure = firstLine(error);
            } catch (...) {
                failure = "unknown preparation failure";
            }
            {
                std::lock_guard lock(mutex);
                admittedBytes -= bytes;
                if (filtered) ++summary.filtered;
                else {
                    ++summary.selected;
                    if (success) {
                        if (hit) ++summary.cacheHits;
                        else ++summary.compiled;
                        ++summary.materialized;
                    } else ++summary.failed;
                }
                std::printf("[shader-prepare] input %llu/%llu %s %s\n", static_cast<unsigned long long>(summary.filtered + summary.selected), static_cast<unsigned long long>(summary.discovered), record->identity.c_str(), filtered ? "filtered" : success ? hit ? "current-cache-hit materialized" : "compiled persisted materialized" : "failed");
                if (!failure.empty()) std::fprintf(stderr, "[shader-prepare] input %s failed: %s\n", record->identity.c_str(), failure.c_str());
            }
            available.notify_all();
        }
    };
    {
        std::vector<std::jthread> workers;
        workers.reserve(options.jobs);
        for (std::uint32_t i = 0; i < options.jobs; ++i) workers.emplace_back(worker);
    }
    if (summary.failed == 0u && !interrupted.load(std::memory_order_relaxed)) refreshRecipes(options, device, inventory, records, summary);
    ShaderDiskCache::Flush();
    summary.cancelled = interrupted.load(std::memory_order_relaxed);
    std::printf("[shader-prepare] discovered=%llu selected=%llu compiled=%llu cache_hits=%llu materialized=%llu filtered=%llu failed=%llu recipes=%llu refreshed=%llu recipe_failed=%llu native_compiled=%llu native_ready=%llu cancelled=%u complete=%u\n",
        static_cast<unsigned long long>(summary.discovered), static_cast<unsigned long long>(summary.selected), static_cast<unsigned long long>(summary.compiled), static_cast<unsigned long long>(summary.cacheHits), static_cast<unsigned long long>(summary.materialized), static_cast<unsigned long long>(summary.filtered), static_cast<unsigned long long>(summary.failed), static_cast<unsigned long long>(summary.recipesDiscovered), static_cast<unsigned long long>(summary.recipesRefreshed), static_cast<unsigned long long>(summary.recipesFailed), static_cast<unsigned long long>(summary.nativeCompiled), static_cast<unsigned long long>(summary.nativeReady), summary.cancelled ? 1u : 0u, summary.Complete() ? 1u : 0u);
    return summary;
}

}

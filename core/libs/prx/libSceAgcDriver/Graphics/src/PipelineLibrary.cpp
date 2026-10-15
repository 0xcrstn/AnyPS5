#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineCreationTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/PipelineLinkOptimization.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <string>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <thread>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>

namespace AgcDriver::Graphics {

namespace {

constexpr std::array<VkDynamicState, 15> dynamicStates{
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_DEPTH_BIAS,
    VK_DYNAMIC_STATE_DEPTH_BOUNDS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE_EXT,
    VK_DYNAMIC_STATE_FRONT_FACE_EXT,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
    VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
    VK_DYNAMIC_STATE_STENCIL_OP_EXT,
};

using Key = std::vector<std::byte>;

struct BuiltLibrary {
    Key part;
    Key layout;
    Key rendering;
    std::vector<std::uint64_t> fields;
};

struct DeviceLibraries {
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    PFN_vkDestroyShaderModule destroyModule = nullptr;
    std::vector<VkShaderModule> modules;
    std::map<Key, VkPipelineLayout> layouts;
    std::array<std::map<Key, VkPipeline>, 4> parts;
    std::array<std::map<Key, BuiltLibrary>, 4> shaders;
    std::array<std::map<Key, std::shared_future<VkPipeline>>, 4> building;
    PipelineLibraryCounters counters;
};

void countLibrary(DeviceLibraries& device, std::size_t part, const Key& shaders, const Key& state, const Key& layout, const Key& rendering, std::span<const std::uint64_t> fields, std::span<std::uint64_t> rebuiltFor, LibraryBuildNote& note) {
    if (shaders.empty()) return;
    auto& seen = device.shaders[part];
    const auto found = seen.find(shaders);
    if (found == seen.end()) {
        seen.emplace(shaders, BuiltLibrary{state, layout, rendering, {fields.begin(), fields.end()}});
        ++device.counters.shaders[part];
        return;
    }
    note.rebuilt = true;
    if (found->second.part != state) ++device.counters.rebuiltForState[part];
    if (found->second.layout != layout) ++device.counters.rebuiltForLayout[part];
    if (found->second.rendering != rendering) ++device.counters.rebuiltForRendering[part];
    for (std::size_t field = 0; field < fields.size() && field < found->second.fields.size(); ++field) {
        if (found->second.fields[field] == fields[field]) continue;
        ++rebuiltFor[field];
        note.differingFields |= std::uint64_t{1} << field;
    }
    found->second = {state, layout, rendering, {fields.begin(), fields.end()}};
}

double millisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

template<std::size_t Count>
std::string describeFields(const std::array<std::uint64_t, Count>& counts, const std::array<const char*, Count>& names) {
    std::string text;
    for (std::size_t field = 0; field < Count; ++field) {
        if (counts[field] == 0) continue;
        text += (text.empty() ? "" : ", ") + std::string(names[field]) + " " + std::to_string(counts[field]);
    }
    return text.empty() ? "none" : text;
}

void reportLibraries(const DeviceLibraries& device) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static std::chrono::steady_clock::time_point last{};
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) return;
    last = now;
    const auto& counters = device.counters;
    std::fprintf(stderr, "[pipeline-libraries] pre-rasterization %llu libraries for %llu shaders, fragment %llu for %llu; built again for a shader by what differed: fixed state %llu/%llu, layout %llu/%llu, rendering formats %llu/%llu; fixed state by field: pre-rasterization %s; fragment %s; linked %llu\n",
        static_cast<unsigned long long>(counters.built[1]), static_cast<unsigned long long>(counters.shaders[1]), static_cast<unsigned long long>(counters.built[2]), static_cast<unsigned long long>(counters.shaders[2]),
        static_cast<unsigned long long>(counters.rebuiltForState[1]), static_cast<unsigned long long>(counters.rebuiltForState[2]), static_cast<unsigned long long>(counters.rebuiltForLayout[1]), static_cast<unsigned long long>(counters.rebuiltForLayout[2]),
        static_cast<unsigned long long>(counters.rebuiltForRendering[1]), static_cast<unsigned long long>(counters.rebuiltForRendering[2]), describeFields(counters.preRasterizationRebuiltFor, PreRasterizationLibraryFields).c_str(), describeFields(counters.fragmentRebuiltFor, FragmentLibraryFields).c_str(), static_cast<unsigned long long>(counters.linked));
}

struct LibraryStore {
    std::mutex mutex;
    std::map<VkDevice, DeviceLibraries> devices;
    std::condition_variable idle;
    std::condition_variable work;
    std::deque<std::pair<VkDevice, std::function<void()>>> jobs;
    std::map<VkDevice, std::size_t> pending;
    bool started = false;
};

LibraryStore& Libraries() {
    static auto* store = new LibraryStore();
    return *store;
}

void runJobs(LibraryStore& store) {
    std::unique_lock lock(store.mutex);
    for (;;) {
        store.work.wait(lock, [&] { return !store.jobs.empty(); });
        auto [device, job] = std::move(store.jobs.front());
        store.jobs.pop_front();
        lock.unlock();
        job();
        lock.lock();
        if (--store.pending[device] == 0) store.idle.notify_all();
    }
}

void enqueue(LibraryStore& store, VkDevice device, std::function<void()> job) {
    if (!store.started) {
        std::thread(runJobs, std::ref(store)).detach();
        store.started = true;
    }
    ++store.pending[device];
    store.jobs.emplace_back(device, std::move(job));
    store.work.notify_one();
}

Key Combined(const Key& part, const Key& context) {
    Key key;
    const auto append = [&](const Key& value) {
        const auto size = value.size();
        const auto bytes = std::as_bytes(std::span(&size, 1));
        key.insert(key.end(), bytes.begin(), bytes.end());
        key.insert(key.end(), value.begin(), value.end());
    };
    append(part);
    append(context);
    return key;
}

}

std::span<const VkDynamicState> PipelineLibraryDynamicStates() {
    return dynamicStates;
}

VkPipeline LinkPipelineFromLibraries(const Context& context, const VkGraphicsPipelineCreateInfo& info, const VkPipelineRenderingCreateInfoKHR& rendering, const VkPipelineLayoutCreateInfo& layout, const PipelineLibraryKeys& keys, std::shared_ptr<OptimizedPipeline>* optimized, bool partsOnly, std::span<VkShaderModule> modules) {
    Require(info.renderPass == VK_NULL_HANDLE && info.pDynamicState != nullptr, "pipeline library parts need dynamic rendering and the library dynamic states");
    const bool split = (layout.flags & VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) != 0;
    Require(!split || (layout.setLayoutCount == 2u && layout.pSetLayouts != nullptr && ((keys.preRasterizationSets | keys.fragmentSets) & ~3u) == 0u), "split pipeline library layout requires two descriptor set slots and a valid mask");
    auto& store = Libraries();
    std::unique_lock lock(store.mutex);
    ++store.pending[context.device];
    struct Busy {
        LibraryStore& store;
        VkDevice device;
        std::unique_lock<std::mutex>& lock;
        ~Busy() {
            if (!lock.owns_lock()) lock.lock();
            if (--store.pending[device] == 0) store.idle.notify_all();
        }
    } busy{store, context.device, lock};
    auto& device = store.devices[context.device];
    if (device.destroyPipeline == nullptr) {
        device.destroyPipeline = context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        device.destroyLayout = context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
        device.destroyModule = context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    }
    const auto layoutFor = [&](const Key& key, std::optional<std::uint32_t> mask) {
        auto found = device.layouts.find(key);
        if (found != device.layouts.end()) return found->second;
        auto info = layout;
        std::array<VkDescriptorSetLayout, 2> sets{VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (mask.has_value()) {
            for (std::uint32_t set = 0; set < sets.size(); ++set) {
                if ((*mask & (1u << set)) != 0u) sets[set] = layout.pSetLayouts[set];
            }
            info.pSetLayouts = sets.data();
        }
        VkPipelineLayout handle = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &info, nullptr, &handle), "vkCreatePipelineLayout library");
        return device.layouts.emplace(key, handle).first->second;
    };
    const auto fullLayout = layoutFor(keys.layout, std::nullopt);
    const std::array<VkPipelineLayout, 4> partLayouts{VK_NULL_HANDLE, split ? layoutFor(keys.preRasterizationLayout, keys.preRasterizationSets) : fullLayout, split ? layoutFor(keys.fragmentLayout, keys.fragmentSets) : fullLayout, VK_NULL_HANDLE};
    std::vector<VkPipelineShaderStageCreateInfo> preRasterization;
    const VkPipelineShaderStageCreateInfo* fragment = nullptr;
    bool mesh = false;
    for (std::uint32_t index = 0; index < info.stageCount; ++index) {
        const auto& stage = info.pStages[index];
        if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) fragment = &stage;
        else preRasterization.push_back(stage);
        mesh |= stage.stage == VK_SHADER_STAGE_MESH_BIT_EXT;
    }
    Require(fragment != nullptr && !preRasterization.empty(), "a pipeline built from libraries needs a fragment and a pre-rasterization stage");
    constexpr std::array<VkGraphicsPipelineLibraryFlagsEXT, 4> flags{
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
    };
    const std::array<const Key*, 4> partKeys{&keys.vertexInput, &keys.preRasterization, &keys.fragmentShader, &keys.fragmentOutput};
    const Key none;
    VkPipelineRenderingCreateInfoKHR shaderRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    shaderRendering.viewMask = rendering.viewMask;
    shaderRendering.depthAttachmentFormat = rendering.depthAttachmentFormat;
    shaderRendering.stencilAttachmentFormat = rendering.stencilAttachmentFormat;
    const std::array<std::uint32_t, 3> attachments{shaderRendering.viewMask, static_cast<std::uint32_t>(shaderRendering.depthAttachmentFormat), static_cast<std::uint32_t>(shaderRendering.stencilAttachmentFormat)};
    const auto attachmentBytes = std::as_bytes(std::span(attachments));
    const Key attachmentKey(attachmentBytes.begin(), attachmentBytes.end());
    const auto preRasterizationContext = Combined(keys.preRasterizationLayout, attachmentKey);
    const auto fragmentContext = Combined(keys.fragmentLayout, attachmentKey);
    const std::array<const Key*, 4> contextKeys{&none, &preRasterizationContext, &fragmentContext, &keys.renderPass};
    VkPipelineDepthStencilStateCreateInfo dynamicDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    static const bool backgroundLinking = std::getenv("APS5_NO_PIPELINE_LTO") == nullptr;
    const bool optimize = backgroundLinking && context.pipelineLinkTimeOptimization;
    std::array<bool, 4> used{};
    std::array<Key, 4> combinedKeys;
    std::array<VkPipeline, 4> handles{};
    std::array<std::shared_future<VkPipeline>, 4> joined;
    std::array<std::optional<std::promise<VkPipeline>>, 4> owned;
    std::array<VkGraphicsPipelineLibraryCreateInfoEXT, 4> libraryInfos{};
    std::array<VkGraphicsPipelineCreateInfo, 4> creates{};
    for (std::size_t part = 0; part < flags.size(); ++part) {
        if (part == 0 && mesh) continue;
        used[part] = true;
        combinedKeys[part] = Combined(*partKeys[part], *contextKeys[part]);
        const auto& key = combinedKeys[part];
        if (const auto found = device.parts[part].find(key); found != device.parts[part].end()) {
            handles[part] = found->second;
            continue;
        }
        if (const auto building = device.building[part].find(key); building != device.building[part].end()) {
            joined[part] = building->second;
            ++device.counters.joinedParts[part];
            continue;
        }
        auto& library = libraryInfos[part];
        library = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, flags[part] == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT ? &rendering : &shaderRendering};
        library.flags = flags[part];
        auto& create = creates[part];
        create = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &library};
        create.flags = PipelineLibraryCreateFlags(optimize);
        create.pDynamicState = info.pDynamicState;
        create.layout = partLayouts[part];
        switch (flags[part]) {
        case VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT:
            create.pVertexInputState = info.pVertexInputState;
            create.pInputAssemblyState = info.pInputAssemblyState;
            create.layout = VK_NULL_HANDLE;
            break;
        case VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT:
            create.stageCount = static_cast<std::uint32_t>(preRasterization.size());
            create.pStages = preRasterization.data();
            create.pViewportState = info.pViewportState;
            create.pRasterizationState = info.pRasterizationState;
            create.pTessellationState = info.pTessellationState;
            break;
        case VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT:
            create.stageCount = 1;
            create.pStages = fragment;
            create.pMultisampleState = info.pMultisampleState;
            create.pDepthStencilState = info.pDepthStencilState != nullptr ? info.pDepthStencilState : &dynamicDepth;
            break;
        default:
            create.pMultisampleState = info.pMultisampleState;
            create.pColorBlendState = info.pColorBlendState;
            create.layout = VK_NULL_HANDLE;
            break;
        }
        owned[part].emplace();
        device.building[part].emplace(key, owned[part]->get_future().share());
    }
    lock.unlock();
    std::exception_ptr failure;
    for (std::size_t part = 0; part < flags.size(); ++part) {
        if (!owned[part].has_value()) continue;
        const auto& key = combinedKeys[part];
        if (failure != nullptr) {
            lock.lock();
            device.building[part].erase(key);
            lock.unlock();
            owned[part]->set_exception(failure);
            continue;
        }
        try {
            PerformanceTimer timing("Vulkan.GraphicsPipelineLibrary");
            auto& create = creates[part];
            VkPipeline handle = VK_NULL_HANDLE;
            VkResult created;
            LibraryBuildNote note{part, 0.0, 0.0, optimize, false, 0u};
            {
                PipelineCreationTrace trace("pipeline.library_part", context.pipelineCreationFeedback, create.stageCount);
                trace.Event().Number("part", part).Flag("prebuild", partsOnly).Flag("store_lock_held", false).Flag("retain_lto", optimize).Hex("part_key", ShaderCompileTrace::HashOf(std::span<const std::byte>(key)));
                trace.Chain(create);
                const auto waiting = std::chrono::steady_clock::now();
                auto pipelineCacheLock = context.LockPipelineCacheForUse();
                note.lockWaitMs = millisecondsSince(waiting);
                const auto creating = std::chrono::steady_clock::now();
                created = context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(context.device, context.pipelineCache, 1, &create, nullptr, &handle);
                note.createMs = millisecondsSince(creating);
                trace.Created(created);
                trace.Event().Real("cache_lock_wait_us", note.lockWaitMs * 1000.0);
                trace.Unchain(create);
            }
            Check(created, "vkCreateGraphicsPipelines library");
            lock.lock();
            auto& cache = device.parts[part];
            if (const auto raced = cache.find(key); raced != cache.end()) {
                device.destroyPipeline(context.device, handle, nullptr);
                handle = raced->second;
            } else {
                cache.emplace(key, handle);
                if (part == PreRasterizationLibraryPart) countLibrary(device, part, keys.preRasterizationShaders, *partKeys[part], keys.preRasterizationLayout, attachmentKey, keys.preRasterizationFields, device.counters.preRasterizationRebuiltFor, note);
                if (part == FragmentShaderLibraryPart) countLibrary(device, part, keys.fragmentShaders, *partKeys[part], keys.fragmentLayout, attachmentKey, keys.fragmentFields, device.counters.fragmentRebuiltFor, note);
                ThreadLibraryBuildNotes().push_back(note);
                reportLibraries(device);
                if (optimize) {
                    for (std::uint32_t stage = 0; stage < create.stageCount; ++stage) {
                        for (auto& module : modules) {
                            if (module == VK_NULL_HANDLE || module != create.pStages[stage].module) continue;
                            device.modules.push_back(module);
                            module = VK_NULL_HANDLE;
                        }
                    }
                }
                ++device.counters.built[part];
                ++ThreadBuiltLibraryParts()[part];
            }
            device.building[part].erase(key);
            lock.unlock();
            handles[part] = handle;
            owned[part]->set_value(handle);
        } catch (...) {
            failure = std::current_exception();
            if (!lock.owns_lock()) lock.lock();
            device.building[part].erase(key);
            lock.unlock();
            owned[part]->set_exception(failure);
        }
    }
    if (failure != nullptr) std::rethrow_exception(failure);
    for (std::size_t part = 0; part < flags.size(); ++part) {
        if (joined[part].valid()) handles[part] = joined[part].get();
    }
    if (partsOnly) return VK_NULL_HANDLE;
    std::vector<VkPipeline> libraries;
    for (std::size_t part = 0; part < flags.size(); ++part) {
        if (used[part]) libraries.push_back(handles[part]);
    }
    PerformanceTimer timing("Vulkan.GraphicsPipelineLink");
    VkPipelineLibraryCreateInfoKHR linked{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    linked.libraryCount = static_cast<std::uint32_t>(libraries.size());
    linked.pLibraries = libraries.data();
    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &linked};
    create.layout = info.layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        LibraryBuildNote note{PipelineLinkPart, 0.0, 0.0, false, false, 0u};
        PipelineCreationTrace trace("pipeline.link", context.pipelineCreationFeedback, 0u);
        trace.Event().Number("libraries", libraries.size()).Flag("store_lock_held", false);
        trace.Chain(create);
        const auto waiting = std::chrono::steady_clock::now();
        auto pipelineCacheLock = context.LockPipelineCacheForUse();
        note.lockWaitMs = millisecondsSince(waiting);
        const auto creating = std::chrono::steady_clock::now();
        const auto linkedResult = context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(context.device, context.pipelineCache, 1, &create, nullptr, &pipeline);
        trace.Created(linkedResult);
        Check(linkedResult, "vkCreateGraphicsPipelines link");
        note.createMs = millisecondsSince(creating);
        ThreadLibraryBuildNotes().push_back(note);
    }
    lock.lock();
    ++device.counters.linked;
    if (optimize && optimized != nullptr) {
        auto slot = std::make_shared<OptimizedPipeline>();
        slot->device = context.device;
        slot->destroy = device.destroyPipeline;
        *optimized = slot;
        const auto createPipelines = context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
        const auto cache = context.pipelineCache;
        const auto cacheSynchronization = context.pipelineCacheSynchronization;
        const auto libraryLayout = fullLayout;
        const bool feedback = context.pipelineCreationFeedback;
        const auto enqueued = ShaderCompileTrace::Clock::now();
        enqueue(store, context.device, [slot, createPipelines, cache, cacheSynchronization, libraryLayout, libraries, &store, devicePointer = &device, feedback, enqueued] {
            PipelineCreationTrace trace("pipeline.lto", feedback, 0u);
            trace.Event().Queued(enqueued).Number("libraries", libraries.size());
            if (slot->released.load()) {
                slot->state.store(OptimizedPipeline::Released);
                trace.Event().Flag("released_before_start", true);
                return;
            }
            VkPipelineLibraryCreateInfoKHR linked{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
            linked.libraryCount = static_cast<std::uint32_t>(libraries.size());
            linked.pLibraries = libraries.data();
            VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &linked};
            create.flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
            create.layout = libraryLayout;
            trace.Chain(create);
            VkPipeline result = VK_NULL_HANDLE;
            const auto pipelineCreationResult = [&] {
                auto pipelineCacheLock = cacheSynchronization != nullptr ? cacheSynchronization->LockForPipelineUse() : PipelineCacheSynchronization::PipelineUseLock{};
                return createPipelines(slot->device, cache, 1, &create, nullptr, &result);
            }();
            trace.Created(pipelineCreationResult);
            if (pipelineCreationResult != VK_SUCCESS) {
                slot->failure.store(pipelineCreationResult);
                slot->state.store(OptimizedPipeline::Failed);
                std::lock_guard lock(store.mutex);
                const auto failures = ++devicePointer->counters.optimizationFailed;
                if (failures <= 4) std::fprintf(stderr, "[pipeline-libraries] link-time optimization failed with VkResult %d (%llu so far); the fast-linked pipeline stays bound\n", static_cast<int>(pipelineCreationResult), static_cast<unsigned long long>(failures));
                return;
            }
            slot->handle.store(result);
            slot->state.store(OptimizedPipeline::Ready);
            if (slot->released.load()) {
                if (const auto handle = slot->handle.exchange(VK_NULL_HANDLE)) slot->destroy(slot->device, handle, nullptr);
                slot->state.store(OptimizedPipeline::Released);
            }
            std::lock_guard lock(store.mutex);
            ++devicePointer->counters.optimized;
        });
    }
    lock.unlock();
    return pipeline;
}

void OptimizedPipeline::Release() noexcept {
    released.store(true);
    if (state.load() == Ready) state.store(Released);
    if (const auto pipeline = handle.exchange(VK_NULL_HANDLE)) destroy(device, pipeline, nullptr);
}

void WaitForOptimizedPipelines(VkDevice device) {
    auto& store = Libraries();
    std::unique_lock lock(store.mutex);
    store.idle.wait(lock, [&] { return store.pending[device] == 0; });
}

void ClearPipelineLibraries(VkDevice device) {
    auto& store = Libraries();
    std::unique_lock lock(store.mutex);
    store.idle.wait(lock, [&] { return store.pending[device] == 0; });
    const auto found = store.devices.find(device);
    if (found == store.devices.end()) return;
    auto& libraries = found->second;
    for (auto& part : libraries.parts) {
        for (const auto& [key, handle] : part) libraries.destroyPipeline(device, handle, nullptr);
    }
    for (const auto& [key, handle] : libraries.layouts) libraries.destroyLayout(device, handle, nullptr);
    for (const auto module : libraries.modules) libraries.destroyModule(device, module, nullptr);
    store.devices.erase(found);
}

namespace {

std::atomic<std::uint64_t> createdModules{0};
std::atomic<std::uint64_t> createdModuleBytes{0};

}

void NoteShaderModule(std::size_t spirvBytes) {
    createdModules.fetch_add(1, std::memory_order_relaxed);
    createdModuleBytes.fetch_add(spirvBytes, std::memory_order_relaxed);
}

ShaderModuleCounters CreatedShaderModules() {
    return {createdModules.load(std::memory_order_relaxed), createdModuleBytes.load(std::memory_order_relaxed)};
}

std::array<std::uint64_t, 4>& ThreadBuiltLibraryParts() {
    thread_local std::array<std::uint64_t, 4> built{};
    return built;
}

namespace {

struct LibraryBuildNotesTag {};

}

std::vector<LibraryBuildNote>& ThreadLibraryBuildNotes() {
    return HostThreadLocal<std::vector<LibraryBuildNote>, LibraryBuildNotesTag>();
}

PipelineLibraryCounters PipelineLibraryCountersOf(VkDevice device) {
    auto& store = Libraries();
    std::lock_guard lock(store.mutex);
    const auto found = store.devices.find(device);
    return found == store.devices.end() ? PipelineLibraryCounters{} : found->second.counters;
}

}

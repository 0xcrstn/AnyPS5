#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/CompileService.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferCopy.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DwordPatternFill.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

struct Driver::DispatchInputs {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
    std::shared_ptr<const ShaderSnapshot> shader;
    std::shared_ptr<DispatchVariant> keepVariant;
    std::shared_ptr<DispatchVariant> attachVariant;
    std::shared_ptr<ShaderMemory> shaderMemory;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    std::vector<ShaderRecompiler::MemoryRegion> captured;
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    std::shared_ptr<RecipeHit> recipeHit;
    std::shared_ptr<PreparedDispatch> prepared;
    std::array<std::uint32_t, 3> groups;
    std::uint64_t indirectArguments;
    std::uint64_t dispatchThreads;
    std::uint64_t address;
    bool dataHit;
    DispatchVariant* attachTo;
    std::vector<PreparedCpuCopy> stageCopies{};
};

FrozenFallback Driver::dispatchHleFallback(const Registers& shader, std::span<const std::uint32_t> packet, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, std::uint64_t indirectArguments) {
    if (indirectArguments != 0 && matchesFillKernel(code, userData, compute)) return FrozenFallback::ArgumentRead;
    if (Graphics::HtileDepthClearAddress(code, userData, compute.numThreads) != 0) return FrozenFallback::DepthNote;
    if (indirectArguments == 0 && (packet[4] & 0x20u) == 0 && Graphics::MatchMemoryValueFill(code, userData, compute.numThreads, {packet[1], packet[2], packet[3]})) return FrozenFallback::DepthNote;
    std::array<std::uint32_t, 3> partialGroups{};
    if (indirectArguments == 0 && (packet[4] & 0x2u) != 0) partialGroups = Pm4::PartialGroupThreads({packet[1], packet[2], packet[3]}, {readRegister(shader, 0x207), readRegister(shader, 0x208), readRegister(shader, 0x209)});
    if (partialGroups != std::array<std::uint32_t, 3>{}) return FrozenFallback::None;
    if (MatchDwordPatternFill(packet, code, userData, compute) || MatchConstantDwordFill(packet, code, userData, compute) || MatchMemoryValueBufferFill(packet, code, userData, compute) || matchesFillKernel(code, userData, compute)) return FrozenFallback::FillHle;
    if (indirectArguments == 0 && (matchesCopyKernel(code, userData, compute) || MatchesStreamCopyKernel(code, userData, compute))) return FrozenFallback::CopyHle;
    return FrozenFallback::None;
}

FrozenFallback Driver::dispatchFrozenFallback(const QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments) {
    static const bool debug = std::getenv("APS5_PROBE_DISPATCH") != nullptr || std::getenv("APS5_DUMP_SHADERS") != nullptr || std::getenv("APS5_TRACE_DISPATCH_CACHE") != nullptr || std::getenv("APS5_TRACE_DISPATCH_IO") != nullptr || std::getenv("APS5_NO_UNLOCKED_DEVICE") != nullptr;
    if (debug || verifyDataHits() || traceCapSync() || ShaderRecompiler::DebugProbeActive()) return FrozenFallback::Debug;
    if (device.Load() == nullptr) return FrozenFallback::NoDevice;
    if (GuestMemory::GpuMutex().HeldByThisThread()) return FrozenFallback::GpuMutexHeld;
    if (!deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value()) return FrozenFallback::Labels;
    if (resolvedAhead().contains(currentPacketOffset())) return FrozenFallback::ResolvedAhead;
    const auto address = (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20c)) << 8u) | (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20d) & 0xffu) << 40u);
    if (address == SkippedShader() && address != 0) return FrozenFallback::None;
    const auto registeredShader = RegisteredProgram(*submission.shaders, address, {0});
    if (!registeredShader || registeredShader->type != 0 || address < registeredShader->codeAddress) return FrozenFallback::UnregisteredShader;
    const auto& snapshot = *registeredShader;
    const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
    if (codeOffset > snapshot.code.size()) return FrozenFallback::UnregisteredShader;
    const auto userCount = (readRegister(queue.shader, 0x213) >> 1u) & 0x1fu;
    std::vector<std::uint32_t> userData;
    userData.reserve(userCount);
    for (std::uint32_t i = 0; i < userCount; ++i) userData.push_back(readUserData(queue.shader, 0x240 + i));
    const auto compute = Graphics::DecodeComputeStageInfo(queue.shader, snapshot.header);
    return dispatchHleFallback(queue.shader, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, indirectArguments);
}

void Driver::noteDispatchInputWriters(const DispatchInputs& inputs, std::uint32_t queue) {
    if (writeEvidenceEnabled() || traceCapSync()) noteWrittenBuffers(inputs.address, queue, *inputs.compiled);
}

void Driver::recordDispatchInputs(DispatchInputs& inputs, const std::shared_ptr<VulkanDevice>& localDevice) {
    const auto& compiled = *inputs.compiled;
    const auto& groups = inputs.groups;
    std::shared_ptr<const Recipe> builtRecipe;
    for (;;) {
        if (inputs.recipeHit != nullptr) {
            VulkanDevice::IndirectOutcome outcome{0, 0};
            const auto result = localDevice->DispatchRecipe(compiled, groups[0], groups[1], groups[2], inputs.indirectArguments, inputs.address, inputs.recipeHit, outcome, VulkanDevice::VerifyRecipes() ? inputs.prepared : nullptr, inputs.dataHit);
            if (result == RecipeOutcome::Rebuild) {
                inputs.recipeHit = nullptr;
                VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, inputs.indirectArguments != 0);
                inputs.prepared = localDevice->PrepareDispatch(compiled, inputs.snapshots, inputs.dispatchThreads, inputs.address);
                continue;
            }
            if (inputs.indirectArguments != 0) countIndirect(outcome.cpuReason, outcome.argumentReadMs);
        } else if (inputs.indirectArguments != 0) {
            const auto outcome = localDevice->DispatchIndirect(compiled, inputs.indirectArguments, inputs.snapshots, inputs.address, std::move(inputs.prepared), inputs.attachTo != nullptr ? &builtRecipe : nullptr);
            countIndirect(outcome.cpuReason, outcome.argumentReadMs);
        } else {
            localDevice->Dispatch(compiled, groups[0], groups[1], groups[2], inputs.snapshots, inputs.address, std::move(inputs.prepared), inputs.attachTo != nullptr ? &builtRecipe : nullptr);
        }
        break;
    }
    if (builtRecipe == nullptr) return;
    if (inputs.dataHit) {
        auto own = std::make_shared<Recipe>(*builtRecipe);
        own->dataWordsHash = Graphics::ShaderResources::DataWordsHash({ShaderRecompiler::ShaderStage::Compute, inputs.keepVariant->compiled.get(), 0});
        builtRecipe = std::move(own);
    }
    inputs.attachTo->recipe.store(std::move(builtRecipe), std::memory_order_release);
    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, inputs.indirectArguments != 0);
}

bool Driver::staleDispatchInputs(const DispatchInputs& adopted, const DispatchInputs& fresh, const std::function<bool(std::uint64_t, std::size_t)>& touchedEarlier) {
    char text[320];
    const auto fail = [&](const char* what) {
        std::snprintf(text, sizeof(text), "APS5_VERIFY_PREPARE_DISPATCHES: the preparation on a worker differs from the in-order preparation in %s", what);
        throw std::runtime_error(text);
    };
    if (adopted.address != fresh.address) fail("the program address");
    if (adopted.indirectArguments != fresh.indirectArguments || adopted.groups != fresh.groups || adopted.dispatchThreads != fresh.dispatchThreads) fail("the indirect decision or the group counts");
    if (adopted.snapshots.size() != fresh.snapshots.size()) fail("the number of bound ranges");
    for (std::size_t index = 0; index < adopted.snapshots.size(); ++index) {
        const auto& left = adopted.snapshots[index];
        const auto& right = fresh.snapshots[index];
        if (left.address != right.address || left.bytes.size() != right.bytes.size()) {
            std::snprintf(text, sizeof(text), "APS5_VERIFY_PREPARE_DISPATCHES: bound range %zu is 0x%llx+0x%zx on a worker, 0x%llx+0x%zx in order", index, static_cast<unsigned long long>(left.address), left.bytes.size(), static_cast<unsigned long long>(right.address), right.bytes.size());
            throw std::runtime_error(text);
        }
    }
    for (std::size_t index = 0; index < adopted.snapshots.size(); ++index) {
        const auto& left = adopted.snapshots[index];
        const auto& right = fresh.snapshots[index];
        if (left.bytes.empty() || std::memcmp(left.bytes.data(), right.bytes.data(), left.bytes.size()) == 0) continue;
        std::vector<std::byte> live(left.bytes.size());
        GuestMemory::Read(left.address, live);
        if (std::memcmp(live.data(), right.bytes.data(), live.size()) == 0 && !touchedEarlier(left.address, left.bytes.size())) return true;
        std::snprintf(text, sizeof(text), "APS5_VERIFY_PREPARE_DISPATCHES: bound range %zu at 0x%llx holds different bytes on a worker and in order, and no CPU store after the preparation explains it", index, static_cast<unsigned long long>(left.address));
        throw std::runtime_error(text);
    }
    for (const auto& copy : adopted.stageCopies) {
        const auto found = std::find_if(fresh.stageCopies.begin(), fresh.stageCopies.end(), [&](const PreparedCpuCopy& other) { return other.begin == copy.begin && other.end == copy.end; });
        if (found == fresh.stageCopies.end() || copy.bytes.empty() || found->bytes.size() != copy.bytes.size()) continue;
        if (std::memcmp(copy.bytes.data(), found->bytes.data(), copy.bytes.size()) == 0) continue;
        std::vector<std::byte> live(copy.bytes.size());
        GuestMemory::Read(copy.begin, live);
        if (std::memcmp(live.data(), found->bytes.data(), live.size()) == 0 && !touchedEarlier(copy.begin, copy.bytes.size())) return true;
        std::snprintf(text, sizeof(text), "APS5_VERIFY_PREPARE_DISPATCHES: stage A copied 0x%llx+0x%zx on the CPU with different bytes on a worker and in order, and no CPU store after the preparation explains it", static_cast<unsigned long long>(copy.begin), copy.bytes.size());
        throw std::runtime_error(text);
    }
    const auto& left = *adopted.compiled;
    const auto& right = *fresh.compiled;
    if (left.PipelineVariantId() != right.PipelineVariantId() || left.spirv != right.spirv) fail("the shader variant");
    if (!sameResourceBindings(left, right)) fail("the resource bindings or push constants");
    if (Graphics::ShaderResources::DataWordsHash({ShaderRecompiler::ShaderStage::Compute, &left, 0}) != Graphics::ShaderResources::DataWordsHash({ShaderRecompiler::ShaderStage::Compute, &right, 0})) fail("the shader data words");
    if (adopted.prepared != nullptr && fresh.prepared != nullptr && !std::ranges::equal(VulkanDevice::PreparedKey(*adopted.prepared), VulkanDevice::PreparedKey(*fresh.prepared))) fail("the resource template key");
    return false;
}

void Driver::dispatchNow(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments) {
    PerformanceTimer timing("Driver.Dispatch");
    const auto address = (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20c)) << 8u) | (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20d) & 0xffu) << 40u);
    if (address == SkippedShader() && address != 0) return;
    auto registeredShader = RegisteredProgram(*submission.shaders, address, {0});
    auto* const sink = frozenSink();
    const auto refuse = [&](FrozenFallback reason) {
        if (!sink->Refuse(reason)) throw std::logic_error(std::string("a frozen dispatch needs queue 0 in its in-order preparation: ") + FrozenFallbackNames[static_cast<std::size_t>(reason)]);
    };
    if (!registeredShader && sink != nullptr) {
        refuse(FrozenFallback::UnregisteredShader);
        return;
    }
    if (!registeredShader) registeredShader = ReadRawShader(address);
    const auto& snapshot = *registeredShader;
    require(snapshot.type == 0, "compute program refers to a non-compute shader");
    const auto userCount = (readRegister(queue.shader, 0x213) >> 1u) & 0x1fu;
    std::vector<std::uint32_t> userData;
    for (std::uint32_t i = 0; i < userCount; ++i) {
        userData.push_back(readUserData(queue.shader, 0x240 + i));
    }
    auto compute = Graphics::DecodeComputeStageInfo(queue.shader, snapshot.header);
    const bool resolving = resolvingAhead();
    std::vector<ShaderRecompiler::MemoryRegion> memory{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}};
    if (!snapshot.header.empty()) memory.push_back({snapshot.headerAddress, snapshot.header});

    static const bool unlockedDevice = std::getenv("APS5_NO_UNLOCKED_DEVICE") == nullptr;
    std::shared_ptr<VulkanDevice> localDevice = unlockedDevice ? device.Load() : nullptr;
    if (sink != nullptr) {
        const auto reason = localDevice == nullptr ? FrozenFallback::NoDevice : dispatchHleFallback(queue.shader, packet, std::span(snapshot.code).subspan(static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t))), userData, compute, indirectArguments);
        if (reason != FrozenFallback::None) {
            refuse(reason);
            return;
        }
    }
    if (localDevice == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
        QueueLock gpuLock(device, submission.queue);
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
    std::array<std::uint32_t, 5> resolved{};
    ResolvedDispatch ahead;
    bool fromAhead = false;
    auto& groupStats = groupCaptureStats();
    if (!resolving) {
        auto& parked = resolvedAhead();
        if (const auto found = parked.find(currentPacketOffset()); found != parked.end()) {
            ahead = std::move(found->second);
            parked.erase(found);
            fromAhead = ahead.address == address;
            if (!fromAhead) ++groupStats.addressMismatches;
        }
    }
    if (fromAhead) {
        resolved = ahead.packet;
        packet = resolved;
        indirectArguments = ahead.indirectArguments;
    } else if (indirectArguments != 0 && matchesFillKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute)) {
        if (drawRecordState().active) waitDrawRecords();
        recordQueuedLabelsBeforeRead(submission.queue);
        const auto readStart = std::chrono::steady_clock::now();
        resolved = Pm4::ReadDispatchArguments(indirectArguments, packet[4]);
        countIndirect(IndirectFillKernel, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        packet = resolved;
        indirectArguments = 0;
    }
    std::array<std::uint32_t, 3> partialGroups{};
    if (indirectArguments == 0 && (packet[4] & 0x2u) != 0) {
        partialGroups = Pm4::PartialGroupThreads({packet[1], packet[2], packet[3]}, {readRegister(queue.shader, 0x207), readRegister(queue.shader, 0x208), readRegister(queue.shader, 0x209)});
    }
    const bool partialLastGroup = partialGroups != std::array<std::uint32_t, 3>{};
    if (resolving) {
        if (!partialLastGroup && matchesFillKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute)) return;
        if (!partialLastGroup && indirectArguments == 0 && (matchesCopyKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute) || MatchesStreamCopyKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute))) return;
    } else {
        if (const auto htile = Graphics::HtileDepthClearAddress(std::span(snapshot.code).subspan(codeOffset), userData, compute.numThreads); htile != 0) {
            const auto threads = (packet[4] & 0x20u) != 0 ? static_cast<std::uint64_t>(packet[1]) : static_cast<std::uint64_t>(packet[1]) * compute.numThreads[0];
            Graphics::NoteHtileDepthClear(htile, static_cast<std::size_t>(threads * 16u));
        }
        if (indirectArguments == 0 && (packet[4] & 0x20u) == 0) {
            if (const auto fill = Graphics::MatchMemoryValueFill(std::span(snapshot.code).subspan(codeOffset), userData, compute.numThreads, {packet[1], packet[2], packet[3]})) Graphics::NoteMemoryValueFill(*fill);
        }
        if (!partialLastGroup && fillBuffer(queue, submission.queue, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, localDevice)) {
            if (drawRecordState().active) {
                drawRecordState().settled = true;
                finishDrawRecords();
            }
            pendingDispatchPhases().outcome = DispatchOutcome::FillHle;
            return;
        }
        if (!partialLastGroup && indirectArguments == 0 && copyBuffer(queue, submission.queue, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, localDevice, address)) {
            if (drawRecordState().active) {
                drawRecordState().settled = true;
                finishDrawRecords();
            }
            pendingDispatchPhases().outcome = DispatchOutcome::CopyHle;
            return;
        }
    }
    if (indirectArguments == 0 && (packet[4] & 0x20u) != 0) {
        const std::array<std::uint32_t, 3> threads{packet[1], packet[2], packet[3]};
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            if (threads[axis] % compute.numThreads[axis] != 0) compute.partialThreads = threads;
        }
    }
    if (partialLastGroup) compute.partialThreads = partialGroups;
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
        {(packet[4] & 0x8000u) != 0 ? 32u : 64u, 0, userData, compute, std::nullopt, std::nullopt, memory, DispatchFloatMode(snapshot, queue.shader)},
        localDevice->ComputeTarget((packet[4] & 0x8000u) != 0 ? 32u : 64u),
        {0, 0, 0, 128}
    };
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static double captureMs = 0, keyMs = 0, recompileMs = 0, deviceMs = 0;
    static std::uint64_t cacheHits = 0;
    static std::uint64_t dispatches = 0;
    auto lap = std::chrono::steady_clock::now();

    std::array<double, DriverPhaseCount> phaseMs{};
    auto phaseLap = lap;
    DispatchPhaseTiming phaseTiming{profile, lap, phaseMs, phaseLap};
    if (profile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[PhasePrologue] = std::chrono::duration<double, std::milli>(lap - packetStartedAt()).count();

    static const bool noDispatchCacheEnv = std::getenv("APS5_NO_DISPATCH_CACHE") != nullptr;

    static const std::pair<std::uint64_t, std::uint64_t> probeDispatch = [] {
        const char* text = std::getenv("APS5_PROBE_DISPATCH");
        if (text == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0, 0};
        char* end = nullptr;
        const auto probeAddress = std::strtoull(text, &end, 16);
        const auto index = end != nullptr && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 0ull;
        return std::pair<std::uint64_t, std::uint64_t>{probeAddress, index};
    }();
    bool probeThis = false;

    if (probeDispatch.first != 0 && (address & 0xfffffffffull) == (probeDispatch.first & 0xfffffffffull)) {
        static std::atomic<std::uint64_t> dispatchesSeen{0};
        probeThis = dispatchesSeen.fetch_add(1) == probeDispatch.second;
        if (probeThis) std::fprintf(stderr, "[gpu] probing dispatch %llu of 0x%llx\n", static_cast<unsigned long long>(probeDispatch.second), static_cast<unsigned long long>(address));
    }

    struct ProbeScope {
        bool active;
        explicit ProbeScope(bool active) : active(active) { if (active) ShaderRecompiler::SetDebugProbeActive(true); }
        ~ProbeScope() { if (active) ShaderRecompiler::SetDebugProbeActive(false); }
    } probeScope{probeThis};
    const bool noDispatchCache = noDispatchCacheEnv || probeThis;
    std::uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    DispatchKeyParts keyParts{address, 0, {}, userData};
    const auto component = [&](DispatchKeyComponent which, std::uint64_t value) {
        auto& part = keyParts.components[static_cast<std::size_t>(which)];
        part ^= value;
        part *= 0x100000001b3ull;
    };
    mix(address);
    mix(packet[4] & 0x8000u);
    component(DispatchKeyComponent::WaveSize, packet[4] & 0x8000u);
    for (const auto threads : compute.partialThreads) {
        mix(threads);
        component(DispatchKeyComponent::PartialThreads, threads);
    }
    for (const auto word : userData) mix(word);

    static const bool keyHygiene = std::getenv("APS5_NO_DISPATCH_KEY_HYGIENE") == nullptr;
    if (keyHygiene) {
        for (const auto offset : {0x207u, 0x208u, 0x209u, 0x212u, 0x213u}) {
            const auto found = queue.shader.find(offset);

            mix(found == queue.shader.end() ? (1ull << 32u) : found->second);
            component(DispatchKeyComponent::Registers, found == queue.shader.end() ? (1ull << 32u) : found->second);
        }
    } else {
        for (const auto& [offset, value] : queue.shader) {
            mix(offset);
            mix(value);
            component(DispatchKeyComponent::Registers, (static_cast<std::uint64_t>(offset) << 32u) | value);
        }
    }
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiledResult;

    std::shared_ptr<DispatchVariant> keepVariant;

    std::shared_ptr<DispatchVariant> attachVariant;

    std::shared_ptr<ShaderMemory> shaderMemory;
    std::vector<ShaderRecompiler::MemoryRegion> captured;

    std::vector<std::uint32_t> liveWords;
    bool dataHit = false;
    bool cached = false;
    bool validated = false;

    std::shared_ptr<DispatchEntry> missedEntry;
    bool missedDiffering = false;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;

    static const bool traceCache = std::getenv("APS5_TRACE_DISPATCH_CACHE") != nullptr;
    if (traceCache) {
        std::lock_guard traceLock(dispatchCacheMutex);
        struct Last { std::vector<std::uint32_t> userData; std::map<std::uint32_t, std::uint32_t> shader; std::uint64_t key; };
        static std::map<std::uint64_t, Last> last;
        static int reports = 0;
        auto& previous = last[address];
        if (previous.key != 0 && previous.key != key && reports < 200) {
            std::string what;
            for (std::size_t i = 0; i < userData.size(); ++i) {
                if (i >= previous.userData.size() || previous.userData[i] != userData[i]) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " user[%zu] %08x->%08x", i, i < previous.userData.size() ? previous.userData[i] : 0u, userData[i]);
                    what += text;
                }
            }
            for (const auto& [offset, value] : queue.shader) {
                const auto old = previous.shader.find(offset);
                if (old == previous.shader.end() || old->second != value) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " sh[%x] %08x->%08x", offset, old == previous.shader.end() ? 0u : old->second, value);
                    what += text;
                }
            }
            ++reports;
            std::fprintf(stderr, "[dispatch-cache] 0x%llx key changed:%s\n", static_cast<unsigned long long>(address), what.c_str());
        }
        previous.userData = userData;
        previous.shader = std::map<std::uint32_t, std::uint32_t>(queue.shader.begin(), queue.shader.end());
        previous.key = key;
    }

    mix(reinterpret_cast<std::uintptr_t>(registeredShader.get()));
    component(DispatchKeyComponent::Shader, reinterpret_cast<std::uintptr_t>(registeredShader.get()));
    keyParts.identity = 0xcbf29ce484222325ull;
    for (const auto value : {address, static_cast<std::uint64_t>(userData.size())}) {
        keyParts.identity ^= value;
        keyParts.identity *= 0x100000001b3ull;
    }
    for (const auto value : keyParts.components) {
        keyParts.identity ^= value;
        keyParts.identity *= 0x100000001b3ull;
    }
    if (fromAhead && key != ahead.key) {
        fromAhead = false;
        ++groupStats.keyMismatches;
    }
    phaseTiming.Phase(PhaseKey);
    timing.Mark("decode_key_device_setup");
    if (fromAhead) {
        compiledResult = std::move(ahead.compiledResult);
        keepVariant = std::move(ahead.keepVariant);
        attachVariant = std::move(ahead.attachVariant);
        shaderMemory = std::move(ahead.shaderMemory);
        captured = std::move(ahead.captured);
        liveWords = std::move(ahead.liveWords);
        capture = std::move(ahead.capture);
        dataHit = ahead.dataHit;
        cached = ahead.cached;
        validated = ahead.validated;
        ++groupStats.adopted;
        const auto age = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ahead.resolvedAt).count();
        groupStats.ageMs += age;
        groupStats.ageMaxMs = std::max(groupStats.ageMaxMs, age);
        phaseTiming.Phase(PhaseLookup);
    } else {
        lookupDispatch(address, submission, key, keyParts, registeredShader, noDispatchCache, traceCache, profile, memory, phaseTiming, phaseMs, compiledResult, keepVariant, captured, liveWords, dataHit, cached, validated, missedEntry, missedDiffering);
    }
    timing.Mark("lookup_cache");
    if (cached) {
        require(keepVariant != nullptr && keepVariant->shader == registeredShader, "dispatch cache belongs to another registered shader");
        captureMs += phaseTiming.Elapsed();
    } else if (!fromAhead) {
        shaderMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
        std::uint64_t forgetAtCapture = 0;

        static const bool dumpShaders = std::getenv("APS5_DUMP_SHADERS") != nullptr;
        try {
            const auto invocation = InvocationFor(snapshot, codeOffset, request);
            timing.Mark("prepared_invocation");

            const auto waitedBefore = traceCapSync() ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            forgetAtCapture = GuestMemory::ForgetSerial();
            capture = [&] {
                const SampledReadScope sampling(evidenceReads);
                return shaderMemory->Capture(invocation);
            }();
            captured = shaderMemory->Regions();
            request.context.memory = captured;
            if (traceCapSync()) traceCapture("dispatch-capture", address, submission.queue, captured, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
            captureMs += phaseTiming.Elapsed();
            phaseTiming.Phase(PhaseCapture);
            if (dumpShaders) static_cast<void>(dumpRequest(address, request));
            const auto started = std::chrono::steady_clock::now();


            timing.Mark("capture_resources");
            compiledResult = invocation.Materialize(*capture);
            timing.Mark("materialize");
            if (compiledResult->cacheHit) ++cacheHits;
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            static double totalMs = 0;
            totalMs += elapsed;
            if (profile && elapsed > 200) std::fprintf(stderr, "[gpu] compute shader 0x%llx recompile took %.0f ms (%zu SPIR-V words, %zu captured regions, total %.1f s)\n", static_cast<unsigned long long>(address), elapsed, compiledResult->spirv.size(), captured.size(), totalMs / 1000);
        } catch (const std::exception& error) {
            const auto dump = dumpShaders ? dumpRequest(address, request) : std::string{};

            std::string reason = error.what();
            if (const auto newline = reason.find('\n'); newline != std::string::npos) reason.resize(newline);
            char where[96];
            if (dump.empty()) std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
            else std::snprintf(where, sizeof(where), "compute shader 0x%llx (%s): ", static_cast<unsigned long long>(address), dump.c_str());
            throw std::runtime_error(where + reason);
        }
        recompileMs += phaseTiming.Elapsed();
        phaseTiming.Phase(PhaseRecompile);
        insertDispatch(address, key, keyParts, noDispatchCache || !CacheableResult(*compiledResult), profile, registeredShader, forgetAtCapture, memory, shaderMemory, captured, capture, compiledResult, missedEntry, missedDiffering, attachVariant, phaseTiming);
    }
    if (resolving) {
        auto& parked = resolvedAhead()[currentPacketOffset()];
        parked = ResolvedDispatch{};
        parked.address = address;
        parked.key = key;
        parked.indirectArguments = indirectArguments;
        std::copy_n(packet.begin(), std::min(packet.size(), parked.packet.size()), parked.packet.begin());
        parked.registeredShader = registeredShader;
        parked.compiledResult = compiledResult;
        parked.keepVariant = keepVariant;
        parked.attachVariant = attachVariant;
        parked.shaderMemory = shaderMemory;
        parked.captured = captured;
        parked.liveWords = liveWords;
        parked.capture = capture;
        parked.dataHit = dataHit;
        parked.cached = cached;
        parked.validated = validated;
        parked.resolvedAt = std::chrono::steady_clock::now();
        ++groupStats.resolved;
        if (cached) ++groupStats.resolvedHits;
        return;
    }
    if (verifyDataHits() && cached && !liveWords.empty()) verifyDataHit(snapshot, codeOffset, request, memory, address, *keepVariant, liveWords, *compiledResult);

    if (recordQueuedLabelsAfterCapture(submission.queue, captured)) {
        dispatch(queue, packet, submission, indirectArguments);
        return;
    }
    phaseTiming.Phase(PhaseQueuedLabels);
    const auto& compiled = *compiledResult;
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    for (const auto& region : captured) snapshots.push_back({region.guestAddress, region.bytes});
    if (drawRecordState().active && !settleDrawRecords(snapshots, true)) return;
    std::array<std::uint32_t, 3> groups{packet[1], packet[2], packet[3]};
    if (indirectArguments == 0 && (packet[4] & 0x20u) != 0) {

        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const auto threads = std::max(readRegister(queue.shader, 0x207 + axis) & 0xffffu, 1u);
            groups[axis] = (groups[axis] + threads - 1) / threads;
        }
    }
    const std::uint64_t dispatchThreads = indirectArguments != 0 ? 0 : std::uint64_t{groups[0]} * groups[1] * groups[2] * compute.numThreads[0] * compute.numThreads[1] * compute.numThreads[2];
    static const bool traceIo = std::getenv("APS5_TRACE_DISPATCH_IO") != nullptr;
    if (traceIo) {

        std::string words;
        if (std::getenv("APS5_TRACE_DISPATCH_IO")[0] == '2') {
            for (const auto word : userData) {
                char text[12];
                std::snprintf(text, sizeof(text), " %08x", word);
                words += text;
            }
        }
        std::fprintf(stderr, "[dispatch-io] shader 0x%llx%s\n", static_cast<unsigned long long>(address), words.c_str());
    }

    const auto rethrow = [&](const std::exception& error) {
        if (fromAhead) {
            ++groupStats.deviceRejects;
            dispatch(queue, packet, submission, indirectArguments);
            return true;
        }
        char where[64];
        std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
        throw std::runtime_error(where + std::string(error.what()));
    };
    phaseTiming.Phase(PhaseSnapshots);

    std::shared_ptr<RecipeHit> recipeHit;
    const auto* variantCompiled = keepVariant != nullptr ? keepVariant.get() : attachVariant.get();
    const bool recipeBindings = variantCompiled != nullptr && variantCompiled->compiled != nullptr && sameResourceBindings(compiled, *variantCompiled->compiled);
    if (cached && keepVariant != nullptr && !stampValidate() && recipeBindings) {
        recipeHit = localDevice->PrepareRecipe(keepVariant->recipe.load(std::memory_order_acquire), indirectArguments != 0);
        phaseTiming.Phase(PhaseRecipePrecheck);
    }

    std::shared_ptr<const Recipe> builtRecipe;
    auto* const attachTo = stampValidate() || !recipeBindings ? nullptr : keepVariant != nullptr ? keepVariant.get() : attachVariant.get();
    bool writersNoted = false;
    const bool noteWrites = writeEvidenceEnabled() || traceCapSync();

    if (VulkanDevice::CompileParkingEnabled() && sink == nullptr && onWorkerThread() && !GuestMemory::GpuMutex().HeldByThisThread()) {
        if (auto ticket = localDevice->TryComputePipeline(compiled, address)) {
            ParkedOp op;
            op.queue = submission.queue;
            op.submission = submission.serial;
            op.cursor = currentPacketOffset();
            op.what = "dispatch waiting for its compute pipeline";
            op.arm = [device = localDevice, ticket](std::function<void()> wake) { return device->ArmCompileWait(ticket, std::move(wake)); };
            op.resume = [ticket] { ticket->Wait(); };
            ParkPacket(std::move(op));
            static const bool verify = std::getenv("APS5_VERIFY_COMPILE_JOBS") != nullptr;
            if (verify) require(localDevice->TryComputePipeline(compiled, address) == nullptr, "APS5_VERIFY_COMPILE_JOBS: a dispatch resumed after its pipeline build settled, but the pipeline is still not built");
        }
    }
    const bool deferred = (drawRecordState().active || sink != nullptr) && !fromAhead;
    if (deferred) {
        std::shared_ptr<PreparedDispatch> prepared;
        if (recipeHit == nullptr || VulkanDevice::VerifyRecipes()) {
            try {
                prepared = localDevice->PrepareDispatch(compiled, snapshots, dispatchThreads, address);
            } catch (const std::exception& error) {
                rethrow(error);
            }
        }
        if (sink == nullptr && (!deferredLabels().labels.empty() || Graphics::Recorder::PendingLabelSince().has_value())) {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
            QueueLock gpuLock(device, submission.queue);
            recordLabelsForPacket(localDevice.get(), submission.queue);
        }
        phaseTiming.Phase(PhaseLabels);
        if (sink == nullptr && noteWrites && writerKeyedEvidence()) noteWrittenBuffers(address, submission.queue, compiled);
        phaseTiming.Phase(PhaseNoteWriters);
        std::vector<PreparedCpuCopy> stageCopies;
        if (sink != nullptr && prepared != nullptr) {
            bool unbounded = false;
            stageCopies = VulkanDevice::PreparedCpuCopies(*prepared, unbounded);
            if (unbounded && sink->Refuse(FrozenFallback::UnboundedCopies)) return;
        }
        auto inputs = std::make_shared<DispatchInputs>(DispatchInputs{compiledResult, registeredShader, keepVariant, attachVariant, shaderMemory, capture, std::move(captured), std::move(snapshots), std::move(recipeHit), std::move(prepared), groups, indirectArguments, dispatchThreads, address, dataHit, attachTo, std::move(stageCopies)});
        if (sink != nullptr) {
            sink->program = address;
            sink->reads.reserve(inputs->snapshots.size() + inputs->stageCopies.size());
            for (const auto& read : inputs->snapshots) sink->reads.push_back({read.address, read.bytes.size()});
            for (const auto& copy : inputs->stageCopies) sink->reads.push_back({copy.begin, static_cast<std::size_t>(copy.end - copy.begin)});
            sink->dispatchInputs = std::move(inputs);
            return;
        }
        postDrawRecord(localDevice, submission.queue, recordIdentity(submission, indirectArguments != 0 ? "indirect dispatch" : "dispatch", address), [inputs, localDevice](DrawRecordResult&) { recordDispatchInputs(*inputs, localDevice); });
    }
    for (; !deferred;) {

        std::shared_ptr<PreparedDispatch> prepared;
        if (recipeHit == nullptr || VulkanDevice::VerifyRecipes()) {
            try {
                prepared = localDevice->PrepareDispatch(compiled, snapshots, dispatchThreads, address);
            } catch (const std::exception& error) {
                if (rethrow(error)) return;
            }
            if (profile) {
                const auto now = std::chrono::steady_clock::now();
                const auto prepareMs = std::chrono::duration<double, std::milli>(now - phaseLap).count();
                phaseLap = now;
                double parts = 0;
                if (prepared != nullptr) {
                    const auto phases = VulkanDevice::PreparePhaseMs(*prepared);
                    phaseMs[PhasePrepareKey] += phases[0];
                    phaseMs[PhasePrepareFind] += phases[1];
                    phaseMs[PhasePreparePrecollect] += phases[2];
                    phaseMs[PhasePreparePresync] += phases[3];
                    phaseMs[PhasePrepareStageA] += phases[4];
                    for (const auto part : phases) parts += part;
                }
                phaseMs[PhasePrepareOther] += std::max(0.0, prepareMs - parts);
            }
        }
        GuestMemory::TagGpuLockSite(indirectArguments != 0 ? GuestMemory::GpuLockSite::Indirect : GuestMemory::GpuLockSite::Dispatch);
        QueueLock gpuLock(device, submission.queue);
        phaseTiming.Phase(PhaseLockWait);

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(PhaseLabels);
        if (noteWrites && writerKeyedEvidence() && !writersNoted) {
            noteWrittenBuffers(address, submission.queue, compiled);
            writersNoted = true;
        }
        phaseTiming.Phase(PhaseNoteWriters);
        try {
            if (recipeHit != nullptr) {
                VulkanDevice::IndirectOutcome outcome{0, 0};
                const auto result = localDevice->DispatchRecipe(compiled, groups[0], groups[1], groups[2], indirectArguments, address, recipeHit, outcome, VulkanDevice::VerifyRecipes() ? prepared : nullptr, dataHit);
                if (result == RecipeOutcome::Rebuild) {

                    recipeHit = nullptr;
                    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, indirectArguments != 0);
                    continue;
                }
                if (indirectArguments != 0) countIndirect(outcome.cpuReason, outcome.argumentReadMs);
            } else if (indirectArguments != 0) {
                const auto outcome = localDevice->DispatchIndirect(compiled, indirectArguments, snapshots, address, std::move(prepared), attachTo != nullptr ? &builtRecipe : nullptr);
                countIndirect(outcome.cpuReason, outcome.argumentReadMs);
            } else {
                localDevice->Dispatch(compiled, groups[0], groups[1], groups[2], snapshots, address, std::move(prepared), attachTo != nullptr ? &builtRecipe : nullptr);
            }
        } catch (const std::exception& error) {
            if (rethrow(error)) return;
        }
        if (builtRecipe != nullptr) {
            if (dataHit) {

                auto own = std::make_shared<Recipe>(*builtRecipe);
                own->dataWordsHash = Graphics::ShaderResources::DataWordsHash({ShaderRecompiler::ShaderStage::Compute, keepVariant->compiled.get(), 0});
                builtRecipe = std::move(own);
            }
            attachTo->recipe.store(std::move(builtRecipe), std::memory_order_release);
            VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, indirectArguments != 0);
        }
        break;
    }
    phaseTiming.Phase(PhaseDevice);
    if (noteWrites && !writerKeyedEvidence()) noteWrittenBuffers(address, submission.queue, compiled);
    deviceMs += phaseTiming.Elapsed();
    phaseTiming.Phase(PhaseTail);
    if (profile) {
        auto& pending = pendingDispatchPhases();
        pending.outcome = DispatchOutcome::Real;
        pending.phases = true;
        pending.hit = cached;
        pending.validated = validated;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
    }
    if (profile && ++dispatches % 100 == 0) AgcDriver::ProfilePrint_nid_no_patch( "[gpu] %llu dispatches (%llu dispatch cache hits, %llu evictions, %llu recompile cache hits): capture %.1f s, cache key %.1f s, recompile %.1f s, device %.1f s\n", static_cast<unsigned long long>(dispatches), static_cast<unsigned long long>(dispatchCacheHits), static_cast<unsigned long long>(dispatchCacheEvictions), static_cast<unsigned long long>(cacheHits), captureMs / 1000, keyMs / 1000, recompileMs / 1000, deviceMs / 1000);
}

}

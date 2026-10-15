#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

std::shared_ptr<const ShaderRecompiler::RecompileResult> Driver::materializeDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, std::string& rejected) {
    PerformanceTimer timing("Shader.DrawStage");
    using Stage = ShaderRecompiler::ShaderStage;
    phaseTiming.Phase(DrawRowVectors);
    const auto& program = programs[i];
    const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
    ShaderRecompiler::RecompileRequest request{
        program.binary,
        {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(pixel) : std::nullopt, vertexInfos[i], memory, RegisteredFloatMode(*program.snapshot)},
        localDevice->Target(),
        {0, 0, pushOffset, (graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : graphics.stages.tessellation && pushOffset < Graphics::PipelinePushSlotBytes ? ShaderRecompiler::TessellationLevelPushOffsetBytes : Graphics::PipelinePushSlotBytes) - pushOffset % Graphics::PipelinePushSlotBytes},
        ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}}
    };
    const auto waitedBefore = traceCapSync() || profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
    const auto invocation = InvocationFor(*program.snapshot, program.codeOffset, request);
    timing.Mark("prepared_invocation");
    phaseTiming.Phase(DrawRowCaptureInvocation);
    auto& stageCapture = stageCaptures[i];
    stageCapture.forgetSerial = GuestMemory::ForgetSerial();
    stageCapture.pushOffset = pushOffset;
    const auto named = [&](const std::exception& error) {
        static const bool dumpShaders = std::getenv("APS5_DUMP_SHADERS") != nullptr;
        std::string dump;
        if (dumpShaders) {
            try {
                dump = dumpRequest(program.binary.codeAddress, request);
            } catch (...) {}
        }
        std::ostringstream where;
        where << "shader 0x" << std::hex << program.binary.codeAddress << " stage " << std::dec << static_cast<unsigned>(program.binary.stage);
        if (!dump.empty()) where << " (" << dump << ")";
        where << ": ";
        return std::runtime_error(where.str() + error.what());
    };
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> planValues;
    ShaderMemory::IncrementalState* incremental = nullptr;
    ShaderMemory::PlanInstance* planInstance = nullptr;
    ShaderMemory::PlanCapture planCapture;
    if (bindingPlan() && !ShaderRecompiler::DebugProbeActive() && dumpTarget == 0 && dumpSlot1 == 0) {
        if (incrementalPlans() && invocation.Handle() != nullptr) planInstance = &resourcePlanInstance(invocation.Handle(), pushOffset, invocation.Request().context.userData);
        else if (incrementalBindings() && invocation.Handle() != nullptr) incremental = &incrementalBinding(invocation.Handle(), invocation.Request().context.userData);
        planValues = [&] {
            const SampledReadScope sampling(evidenceReads);
            try {
                if (planInstance != nullptr) {
                    planCapture = shaderMemory.CapturePlanned(invocation, *planInstance, verifyIncrementalPlans());
                    return planCapture.values;
                }
                return incremental != nullptr ? shaderMemory.CaptureValues(invocation, *incremental) : shaderMemory.CaptureValues(invocation);
            } catch (const std::exception& error) {
                throw named(error);
            }
        }();
    }
    const bool planned = planValues != nullptr && (planValues->plan == nullptr || !ShaderMemory::UsesImageTables(*planValues->plan));
    if (!planned && incremental != nullptr) *incremental = {};
    if (!planned && planInstance != nullptr) *planInstance = {};
    if (planned) {
        const auto& values = planValues;
        phaseTiming.Phase(DrawRowCapture);
        stageCapture.prepared = invocation.Handle();
        recompiled[i] = true;
        if (profile) {
            ++captures;
            const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBefore, phaseMs[DrawRowCapture]);
            phaseMs[DrawRowCapture] -= waited;
            phaseMs[DrawRowCaptureHookWaits] += waited;
        }
        phaseTiming.Phase(DrawRowCaptureRegions);
        bool templateHit = false;
        try {
            stageCapture.compiled = invocation.MaterializeValues(*values, &templateHit);
        } catch (const std::exception& error) {
            throw named(error);
        }
        phaseTiming.Phase(DrawRowRecompile);
        if (incremental != nullptr) {
            const bool verified = verifyIncrementalBindings();
            bool stale = false;
            if (verified) {
                const auto kept = shaderMemory.TakeKeptWords();
                std::vector<ShaderRecompiler::MemoryRegion> code;
                for (const auto& region : program.memory) {
                    if (!region.bytes.empty()) code.push_back(region);
                }
                ShaderMemory reference(code, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
                const auto fresh = reference.CaptureValues(invocation);
                const auto freshResult = invocation.MaterializeValues(*fresh);
                if (!SameSnapshot(fresh->snapshot, values->snapshot) || !SameStageResult(*freshResult, *stageCapture.compiled)) {
                    stale = std::any_of(kept.begin(), kept.end(), [](const std::pair<std::uint64_t, std::uint32_t>& word) {
                        std::uint32_t now = 0;
                        std::memcpy(&now, reinterpret_cast<const void*>(word.first), sizeof(now));
                        return now != word.second;
                    });
                    if (!stale) {
                        noteIncrementalStage(*incremental->instance, true, false);
                        ThrowIncrementalMismatch(submission.queue, submission.serial, currentPacketOffset(), i, program.binary.codeAddress);
                    }
                }
                phaseTiming.Phase(DrawRowStageVerify);
            }
            noteIncrementalStage(*incremental->instance, verified, stale);
        }
        if (planInstance != nullptr) {
            const bool verified = verifyIncrementalPlans();
            bool stale = false;
            if (verified) {
                std::vector<ShaderRecompiler::MemoryRegion> code;
                for (const auto& region : program.memory) {
                    if (!region.bytes.empty()) code.push_back(region);
                }
                ShaderMemory reference(code, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
                const auto fresh = reference.CaptureValues(invocation);
                if (!SameSnapshot(fresh->snapshot, values->snapshot)) {
                    stale = !ShaderMemory::PlanReadsCurrent(*planInstance);
                    if (!stale) {
                        notePlanInstance(planCapture, true, false);
                        ThrowPlanInstanceMismatch(submission.queue, submission.serial, currentPacketOffset(), i, program.binary.codeAddress, planCapture, fresh->snapshot, values->snapshot);
                    }
                }
                phaseTiming.Phase(DrawRowStageVerify);
            }
            notePlanInstance(planCapture, verified, stale);
        }
        bool mismatch = false;
        auto part = SnapshotPart::None;
        if (verifyBindingPlan()) {
            const auto full = shaderMemory.Capture(invocation);
            const auto reference = invocation.Materialize(*full);
            part = FirstSnapshotDifference(*full, values->snapshot);
            if (part == SnapshotPart::None && !SameStageResult(*reference, *stageCapture.compiled)) part = SnapshotPart::Result;
            mismatch = part != SnapshotPart::None;
            phaseTiming.Phase(DrawRowStageVerify);
        }
        notePlannedStage(verifyBindingPlan(), mismatch, part, templateHit, ShaderRecompiler::CountSpecializationWords(values->snapshot));
        if (mismatch) ThrowStageMismatch(submission.queue, submission.serial, currentPacketOffset(), i, program.binary.codeAddress, part);
        return stageCapture.compiled;
    }
    const auto capture = [&] {
        const SampledReadScope sampling(evidenceReads);
        try {
            return shaderMemory.Capture(invocation);
        } catch (const std::exception& error) {
            throw named(error);
        }
    }();
    phaseTiming.Phase(DrawRowCapture);

    stageCapture.regions = shaderMemory.TakeRecentRegions();
    stageCapture.userDataUse = capture->userDataUse;
    stageCapture.prepared = invocation.Handle();
    stageCapture.bufferBaseWords = capture->bufferBaseWords;
    stageCapture.dataLeaves = capture->readTrace.leaves;
    stageCapture.otherReads = capture->readTrace.otherReads;
    stageCapture.baseSlots = capture->baseSlots;
    recompiled[i] = true;
    memory = shaderMemory.Regions();

    for (std::size_t j = 0; j < programs.size(); ++j) {
        if (matched[j] != nullptr && !recompiled[j] && (drawHit || j < i)) memory.insert(memory.end(), matchedRegions[j].begin(), matchedRegions[j].end());
    }
    request.context.memory = memory;
    if (traceCapSync()) traceCapture("draw-capture", program.binary.codeAddress, submission.queue, memory, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
    if (profile) {
        ++captures;
        phaseTiming.Phase(DrawRowCaptureRegions);

        const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBefore, phaseMs[DrawRowCapture]);
        phaseMs[DrawRowCapture] -= waited;
        phaseMs[DrawRowCaptureHookWaits] += waited;
    }
    if (dumpTarget != 0) {

        const auto slot0 = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
        if ((graphics.hasColorTarget && graphics.color.address == dumpTarget) || slot0 == dumpTarget) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    }
    if (dumpSlot1 != 0) {
        const auto value = [&](std::uint32_t offset) -> std::uint64_t { const auto it = queue.context.find(offset); return it == queue.context.end() ? 0u : it->second; };
        const auto slot1 = (value(0x391) << 40u) | (value(0x327) << 8u);
        if (slot1 == dumpSlot1) {
            static_cast<void>(dumpRequest(program.binary.codeAddress, request));
            if (std::FILE* file = std::fopen("draw_slot1.regs", "w")) {
                for (const auto& [offset, value] : queue.context) std::fprintf(file, "context %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.userConfig) std::fprintf(file, "uconfig %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.shader) std::fprintf(file, "shader %x %08x\n", offset, value);
                std::fclose(file);
            }
        }
    }

    phaseTiming.Phase(DrawRowCaptureRegions);

    timing.Mark("capture_resources");
    try {
        stageCapture.compiled = invocation.Materialize(*capture);
    } catch (const std::exception& error) {
        throw named(error);
    }
    stageCapture.capture = capture;
    timing.Mark("materialize");
    phaseTiming.Phase(DrawRowRecompile);
    return stageCapture.compiled;
}

void Driver::recordStageDataPositions(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture, const std::vector<Graphics::DecodeRead>& decodeReads) {
    const auto& compiled = *variant.compiled;
    if (variant.dataPositions.empty()) {
        for (std::size_t b = 0; b < compiled.bindings.size(); ++b) {
            if (compiled.bindings[b].role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
            variant.flatBinding = static_cast<std::uint32_t>(b);
            static_cast<void>(DataWordPositions(variant.runs, capture.readTrace.leaves, capture.readTrace.otherReads, variant.words, compiled.bindings[b].guestDescriptor, variant.dataPositions, variant.dataSlots));
            break;
        }
        variant.bufferWordBuffers.assign(variant.dataPositions.size(), {});
    }
    if (variant.dataPositions.empty() || decodeReads.empty()) return;
    std::vector<bool> decoded(variant.words.size(), false);
    std::size_t offset = 0;
    for (const auto& [begin, end] : variant.runs) {
        for (const auto& read : decodeReads) {
            const auto from = std::max(begin, read.address);
            const auto to = std::min(end, read.address + read.bytes.size());
            if (from >= to) continue;
            for (auto word = (from - begin) / sizeof(std::uint32_t); word < (to - begin + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t); ++word) decoded[offset + static_cast<std::size_t>(word)] = true;
        }
        offset += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
    }
    const bool buffers = variant.bufferWordBuffers.size() == variant.dataPositions.size();
    std::size_t kept = 0;
    for (std::size_t k = 0; k < variant.dataPositions.size(); ++k) {
        if (decoded[variant.dataPositions[k]]) continue;
        variant.dataPositions[kept] = variant.dataPositions[k];
        variant.dataSlots[kept] = variant.dataSlots[k];
        if (buffers) variant.bufferWordBuffers[kept] = std::move(variant.bufferWordBuffers[k]);
        ++kept;
    }
    variant.dataPositions.resize(kept);
    variant.dataSlots.resize(kept);
    if (buffers) variant.bufferWordBuffers.resize(kept);
}

void Driver::cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::uint64_t drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming, const std::vector<bool>& reused, const std::shared_ptr<DrawEntry>& entry, std::uint64_t shapeKey) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto insertStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto chargeInsert = [&] {
        if (!profile) return;
        std::lock_guard cacheLock(drawCacheMutex);
        drawEntryCounters.insertUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - insertStart).count();
    };
    if (useDrawEntries && !drawHit && !(drawParameters.indirect && indirectCpu) && (verifyHit || admitDrawKey(drawKey, registerKey && drawDataHits() ? shapeKey : 0))) {
        phaseTiming.Phase(DrawRowVectors);
        std::uint64_t unstable = 0, mismatches = 0, differingSameRuns = 0, differingWords = 0, differingRunsChanged = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& stageCapture = stageCaptures[i];
            if (reused[i]) {
                fresh[i] = matched[i];
                continue;
            }
            if (stageCapture.compiled == nullptr || !CacheableResult(*stageCapture.compiled)) continue;
            auto variant = std::make_shared<DispatchVariant>();
            variant->compiled = stageCapture.compiled;
            variant->shader = programs[i].snapshot;
            variant->forgetSerial = stageCapture.forgetSerial;
            variant->pushOffset = stageCapture.pushOffset;
            variant->prepared = stageCapture.prepared;
            if (vertexInfos[i]) variant->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(*vertexInfos[i]);

            std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
            for (const auto& read : decodeReads[i]) regions.push_back({read.address, std::as_bytes(std::span(read.bytes))});
            std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
            tagVariantRuns(*variant, regions, stageCapture.userDataUse, decodeReads[i].empty() && (!vertexInfos[i] || vertexInfos[i]->resourcesNum == 0), programs[i].hiddenUserWords, stageCapture.baseSlots);
            variant->userDataReads = UserDataReads(stageCapture.userDataUse, *stageCapture.compiled, decodeReads[i].empty() && (!vertexInfos[i] || vertexInfos[i]->resourcesNum == 0), programs[i].hiddenUserWords);
            tagBufferWords(*variant, stageCapture.bufferBaseWords);
            const auto leafCounts = drawStageDataHits() ? tagDataLeaves(*variant, stageCapture.dataLeaves, stageCapture.otherReads) : DataWordPositionCounts{};
            if (drawMissDiagnosisEnabled()) {
                std::lock_guard cacheLock(drawCacheMutex);
                auto& diagnosis = drawMissDiagnosis;
                diagnosis.leavesOffered += stageCapture.dataLeaves.size();
                diagnosis.leavesUnmapped += leafCounts.unmapped;
                diagnosis.leavesMismatched += leafCounts.mismatched;
                diagnosis.leavesAliased += leafCounts.aliased;
                diagnosis.leavesTagged += static_cast<std::uint64_t>(std::count_if(variant->dataSlots.begin(), variant->dataSlots.end(), [](std::uint32_t slot) { return slot != DispatchVariant::NoFlatBinding; }));
                ++diagnosis.stagesTagged;
                diagnosis.baseSlotsOffered += stageCapture.baseSlots.size();
                diagnosis.nestedTagged += variant->nestedBases.size();
                diagnosis.nestedRuns += static_cast<std::uint64_t>(std::count_if(variant->runNested.begin(), variant->runNested.end(), [](std::int16_t nested) { return nested >= 0; }));
            }
            const auto sameContent = [](const DispatchVariant& a, const DispatchVariant& b) {
                const auto flatten = [](const DispatchVariant& source) {
                    std::vector<std::pair<std::uint64_t, std::uint32_t>> flat;
                    std::size_t word = 0;
                    for (const auto& [begin, end] : source.runs) {
                        for (auto address = begin; address < end && word < source.words.size(); address += sizeof(std::uint32_t)) flat.emplace_back(address, source.words[word++]);
                    }
                    std::sort(flat.begin(), flat.end());
                    return flat;
                };
                return (a.runs == b.runs && a.words == b.words) || flatten(a) == flatten(b);
            };
            const auto sameBindings = [&](const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b) {
                if (a.bindings.size() != b.bindings.size()) return false;
                for (std::size_t binding = 0; binding < a.bindings.size(); ++binding) {
                    if (a.bindings[binding].role == ShaderRecompiler::DescriptorRole::ShaderData) continue;
                    if (a.bindings[binding].guestDescriptor != b.bindings[binding].guestDescriptor) return false;
                }
                return true;
            };
            const auto sameForwardedWords = [](const ShaderRecompiler::RecompileResult& a, const std::vector<std::byte>& push, const ShaderRecompiler::RecompileResult& b, std::uint32_t hidden) {
                if (push.size() != b.pushConstants.size() || a.shaderDataUserWords != b.shaderDataUserWords) return false;
                for (std::size_t slot = 0; slot < a.shaderDataUserWords.size() && (slot + 1) * sizeof(std::uint32_t) <= push.size(); ++slot) {
                    if (a.shaderDataUserWords[slot] >= hidden && std::memcmp(push.data() + slot * sizeof(std::uint32_t), b.pushConstants.data() + slot * sizeof(std::uint32_t), sizeof(std::uint32_t)) != 0) return false;
                }
                return true;
            };
            variant->inputExports = stageCapture.inputExports;
            if (stageCapture.capture != nullptr && (!variant->dataPositions.empty() || (drawStageDataHits() && !stageCapture.capture->readTrace.leaves.empty()))) recordStageDataPositions(*variant, *stageCapture.capture, decodeReads[i]);
            if (traceDrawCache() && entry != nullptr && matched[i] == nullptr && i < entry->stages.size()) {
                const auto& variants = entry->stages[i];
                const auto front = std::find_if(variants.begin(), variants.end(), [&](const std::shared_ptr<DispatchVariant>& kept) { return kept->pushOffset == variant->pushOffset; });
                if (front != variants.end() && (*front)->runs == variant->runs && (*front)->words.size() == variant->words.size()) {
                    ++differingSameRuns;
                    for (std::size_t w = 0; w < variant->words.size(); ++w) differingWords += (*front)->words[w] != variant->words[w] ? 1u : 0u;
                } else if (front != variants.end()) {
                    ++differingRunsChanged;
                }
            }
            if (verifyHit && matched[i] != nullptr && (!sameContent(*matched[i], *variant) || (programs[i].hiddenUserWords == 0 && !sameBindings(*matched[i]->compiled, *variant->compiled)) || !sameForwardedWords(*matched[i]->compiled, matched[i]->patchedPushConstants.empty() ? matched[i]->compiled->pushConstants : matched[i]->patchedPushConstants, *variant->compiled, programs[i].hiddenUserWords))) {
                ++mismatches;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit captured differently: %zu runs / %zu words matched, %zu / %zu fresh\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
            }
            if (insertCompare()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
                if (!captureStable(stageCapture.regions)) {
                    ++unstable;
                    continue;
                }
            }
            fresh[i] = std::move(variant);
        }
        bool stashed = false;
        if (registerKey && decode != nullptr && !verifyHit && relocatedOnlyEntries()) {
            std::lock_guard cacheLock(drawCacheMutex);
            auto& pending = drawRelocationPending;
            if (!drawExactKeySeen && pending.key == drawKey && pending.relocation != nullptr && pending.aliases.structure != 0) {
                pending.decode = decode;
                pending.stages = fresh;
                ++drawEntryCounters.stashed;
                stashed = true;
            }
        }
        if (!stashed) insertDrawEntry(drawKey, fresh, registerKey ? decode : nullptr, registerKey && drawDataHits() ? shapeKey : 0);
        std::uint64_t dataInserts = 0, dataPositions = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (fresh[i] == nullptr || reused[i] || fresh[i]->dataPositions.empty()) continue;
            ++dataInserts;
            dataPositions += fresh[i]->dataPositions.size();
        }
        if (unstable != 0 || mismatches != 0 || differingSameRuns != 0 || differingRunsChanged != 0 || dataInserts != 0) {
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.stageDataInserts += dataInserts;
            drawEntryCounters.stageDataPositions += dataPositions;
            drawEntryCounters.unstable += unstable;
            drawEntryCounters.verifyMismatches += mismatches;
            drawEntryCounters.differingSameRuns += differingSameRuns;
            drawEntryCounters.differingWords += differingWords;
            drawEntryCounters.differingRunsChanged += differingRunsChanged;
        }
        chargeInsert();
        phaseTiming.Phase(DrawRowKeyLookupValidate);
        return;
    }
    if (useDrawEntries && !drawHit) chargeInsert();
}

}

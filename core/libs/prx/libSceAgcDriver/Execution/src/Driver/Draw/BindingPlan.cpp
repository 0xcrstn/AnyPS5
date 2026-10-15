#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include <algorithm>
#include <string>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

bool SameBinding(const ShaderRecompiler::DescriptorBinding& a, const ShaderRecompiler::DescriptorBinding& b) {
    if (a.kind != b.kind || a.role != b.role || a.descriptorSet != b.descriptorSet || a.binding != b.binding || a.count != b.count || a.readOnly != b.readOnly || a.imageShape != b.imageShape) return false;
    if (a.guestDescriptor != b.guestDescriptor || a.samplerDepthCompare != b.samplerDepthCompare || a.imageWritten != b.imageWritten || a.imageDepthCompare != b.imageDepthCompare || a.imageAtomic != b.imageAtomic || a.imageAtomic64 != b.imageAtomic64) return false;
    if (a.bufferAtomic != b.bufferAtomic || a.bufferWritten != b.bufferWritten || a.samplerUnnormalized != b.samplerUnnormalized || a.samplerIntegerBorder != b.samplerIntegerBorder || a.imageUnnormalized != b.imageUnnormalized || a.imageUnnormalizedBaseLevel != b.imageUnnormalizedBaseLevel) return false;
    if (a.imageSamplers != b.imageSamplers || a.bufferRead != b.bufferRead || a.bufferReadRanges.size() != b.bufferReadRanges.size()) return false;
    for (std::size_t index = 0; index < a.bufferReadRanges.size(); ++index) {
        const auto& left = a.bufferReadRanges[index];
        const auto& right = b.bufferReadRanges[index];
        if (left.bounded != right.bounded || left.unbound != right.unbound || left.ranges != right.ranges) return false;
    }
    return true;
}

bool SameAttribute(const ShaderRecompiler::VertexAttribute& a, const ShaderRecompiler::VertexAttribute& b) {
    return a.location == b.location && a.components == b.components && a.fetchIndex == b.fetchIndex && a.formatComponents == b.formatComponents && std::memcmp(&a.resource, &b.resource, sizeof(a.resource)) == 0;
}

std::string HexText(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

}

void ThrowStageMismatch(std::uint32_t queue, std::uint64_t submission, std::size_t packetOffset, std::size_t stage, std::uint64_t program, SnapshotPart part) {
    const auto index = static_cast<std::size_t>(part) < SnapshotParts ? static_cast<std::size_t>(part) : 0;
    throw std::runtime_error("AGC driver: binding plan verify: queue " + std::to_string(queue) + " submission " + std::to_string(submission) + " packet at dword " + std::to_string(packetOffset) + ": stage " + std::to_string(stage) + " (program " + HexText(program) + ") differs from the full capture and materialize, first in: " + SnapshotPartNames[index]);
}

bool SameStageResult(const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b) {
    if (!(a.spirv == b.spirv) || a.variantId != b.variantId || a.specializationId != b.specializationId || a.pushConstants != b.pushConstants || a.poisonedSrtReads != b.poisonedSrtReads) return false;
    if (a.workgroupMemoryDwords != b.workgroupMemoryDwords || a.bdaWrites != b.bdaWrites || a.shaderDataUserWords != b.shaderDataUserWords || a.bufferBaseSlots != b.bufferBaseSlots) return false;
    if (a.bindings.size() != b.bindings.size() || a.vertexAttributes.size() != b.vertexAttributes.size() || a.specialization.size() != b.specialization.size()) return false;
    for (std::size_t index = 0; index < a.bindings.size(); ++index) {
        if (!SameBinding(a.bindings[index], b.bindings[index])) return false;
    }
    for (std::size_t index = 0; index < a.vertexAttributes.size(); ++index) {
        if (!SameAttribute(a.vertexAttributes[index], b.vertexAttributes[index])) return false;
    }
    for (std::size_t index = 0; index < a.specialization.size(); ++index) {
        if (!(a.specialization[index] == b.specialization[index])) return false;
    }
    return true;
}

bool SameSnapshot(const ShaderRecompiler::ResourceSnapshot& a, const ShaderRecompiler::ResourceSnapshot& b) {
    return a.buffers == b.buffers && a.images == b.images && a.samplers == b.samplers && a.flattenedSrt == b.flattenedSrt && a.userData == b.userData && a.uniformFill == b.uniformFill && a.srtPoison == b.srtPoison && a.nullRootReads == b.nullRootReads;
}

SnapshotPart FirstSnapshotDifference(const ShaderRecompiler::ResourceCapture& full, const ShaderRecompiler::ResourceSnapshot& planned) {
    const auto& a = full.snapshot;
    if (a.buffers != planned.buffers) return SnapshotPart::Buffer;
    if (a.images != planned.images) return SnapshotPart::Image;
    if (a.samplers != planned.samplers) return SnapshotPart::Sampler;
    if (a.flattenedSrt != planned.flattenedSrt) {
        const auto& pure = full.plan != nullptr ? full.plan->pureFlatSlots : std::vector<std::uint8_t>{};
        for (std::size_t slot = 0; slot < a.flattenedSrt.size() && slot < planned.flattenedSrt.size(); ++slot) {
            if (a.flattenedSrt[slot] != planned.flattenedSrt[slot]) return slot < pure.size() && pure[slot] != 0u ? SnapshotPart::FlatData : SnapshotPart::FlatOther;
        }
        return SnapshotPart::FlatOther;
    }
    if (a.userData != planned.userData) return SnapshotPart::UserData;
    if (!(a.uniformFill == planned.uniformFill)) return SnapshotPart::UniformFill;
    if (a.srtPoison != planned.srtPoison) return SnapshotPart::Poison;
    if (a.nullRootReads != planned.nullRootReads) return SnapshotPart::NullRoots;
    return SnapshotPart::None;
}

bool Driver::bindingPlan() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_BINDING_PLAN");
        return text != nullptr && std::strcmp(text, "0") != 0;
    }();
    return enabled;
}

bool Driver::verifyBindingPlan() {
    static const bool verify = [] {
        const char* text = std::getenv("APS5_VERIFY_BINDING_PLAN");
        return text != nullptr && std::strcmp(text, "0") != 0;
    }();
    return verify;
}

void Driver::planDrawDecode(const QueueState& queue, const Submission& submission, std::uint64_t deviceSerial, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches, bool profile) {
    static const bool userWordsKeyed = std::getenv("APS5_BINDING_PLAN_KEY_USER_WORDS") != nullptr;
    const auto key = userWordsKeyed ? drawRegisterKey(queue, *submission.shaders, deviceSerial) : drawDecodeKey(queue, *submission.shaders, deviceSerial);
    const bool verify = verifyBindingPlan() || verifyDrawRecipe();
    const auto freshDecode = [&] {
        std::vector<Graphics::RegisterRead> reads;
        struct ReadScope {
            explicit ReadScope(std::vector<Graphics::RegisterRead>* log) { Graphics::RegisterReadLog() = log; }
            ~ReadScope() { Graphics::RegisterReadLog() = nullptr; }
        } scope(verify ? &reads : nullptr);
        auto fresh = decodeDraw(queue, submission);
        for (const auto read : reads) {
            require(Graphics::DrawKeyCovers(read), "binding plan decode read a register its key does not cover");
        }
        return fresh;
    };
    {
        std::lock_guard lock(bindingPlanMutex);
        ++bindingPlanCounters.draws;
        if (auto* found = plannedDecodes.Find(key); found != nullptr && DecodeProgramsCurrent(**found, *submission.shaders)) decode = *found;
    }
    if (decode != nullptr) {
        patches.clear();
        const auto& programs = decode->programs;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& program = programs[i];
            for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
                const auto now = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
                if (now != program.userData[index]) patches.push_back({i, index, now});
            }
        }
        bool mismatch = false;
        if (verify) {
            auto patched = std::make_shared<DrawDecode>(*decode);
            for (const auto& patch : patches) patched->programs[patch.program].userData[patch.index] = patch.value;
            mismatch = !sameDecode(*patched, *freshDecode());
        }
        std::lock_guard lock(bindingPlanMutex);
        ++bindingPlanCounters.decodeHits;
        if (!patches.empty()) ++bindingPlanCounters.decodesPatched;
        if (verify) ++bindingPlanCounters.verifiedDecodes;
        if (mismatch) {
            ++bindingPlanCounters.decodeMismatches;
            throw std::runtime_error("AGC driver: binding plan verify: queue " + std::to_string(submission.queue) + " submission " + std::to_string(submission.serial) + " packet at dword " + std::to_string(currentPacketOffset()) + ": the decode kept for key " + HexText(key) + " differs from a fresh decode after patching " + std::to_string(patches.size()) + " user words");
        }
        if (profile) reportBindingPlan();
        return;
    }
    std::shared_ptr<const DrawDecode> fresh = freshDecode();
    decode = fresh;
    std::lock_guard lock(bindingPlanMutex);
    ++bindingPlanCounters.decodeMisses;
    plannedDecodes.Put(key, std::move(fresh));
    if (profile) reportBindingPlan();
}

BindingPlanCounters Driver::PlanCounters() {
    std::lock_guard lock(bindingPlanMutex);
    return bindingPlanCounters;
}

void Driver::notePlannedStage(bool verified, bool mismatch, SnapshotPart part, bool templateHit, ShaderRecompiler::SnapshotWordCounts words) {
    std::lock_guard lock(bindingPlanMutex);
    ++bindingPlanCounters.stages;
    if (templateHit) ++bindingPlanCounters.templateHits;
    bindingPlanCounters.keyWords += words.keyWords;
    bindingPlanCounters.valueWords += words.valueWords;
    if (verified) ++bindingPlanCounters.verifiedStages;
    if (mismatch) {
        ++bindingPlanCounters.stageMismatches;
        ++bindingPlanCounters.mismatchParts[static_cast<std::size_t>(part) < SnapshotParts ? static_cast<std::size_t>(part) : 0];
    }
}

void Driver::reportBindingPlan() {
    auto& counters = bindingPlanCounters;
    const auto now = std::chrono::steady_clock::now();
    if (now - counters.lastReport < std::chrono::seconds(10)) return;
    std::string parts;
    for (std::size_t part = 1; part < SnapshotParts; ++part) {
        if (counters.mismatchParts[part] == 0) continue;
        parts += std::string(parts.empty() ? " (first in: " : ", ") + SnapshotPartNames[part] + " " + std::to_string(counters.mismatchParts[part]);
    }
    if (!parts.empty()) parts += ")";
    const auto walks = ShaderRecompiler::CompiledWalkCounts();
    const auto reads = ShaderMemory::DirectReads();
    static ShaderRecompiler::SrtCompiledWalkCounts walksBefore;
    static ShaderMemory::DirectReadCounts readsBefore{};
    std::string refusals;
    for (std::size_t reason = 1; reason < ShaderRecompiler::SrtCompiledWalkRefusals; ++reason) {
        const auto count = walks.refusals[reason] - walksBefore.refusals[reason];
        if (count == 0) continue;
        refusals += std::string(refusals.empty() ? " (" : ", ") + ShaderRecompiler::SrtCompiledWalkRefusalNames[reason] + " " + std::to_string(count);
    }
    if (!refusals.empty()) refusals += ")";
    AgcDriver::ProfilePrint_nid_no_patch("[binding-plan] (10 s) %llu draws: decodes kept %llu (%llu with user words patched), made %llu; %llu stages captured without the draw cache, %llu refilled from a specialization-key template (descriptor words in the key %llu, value words passed as data %llu); verify: %llu decodes compared, %llu differed; %llu stages compared, %llu differed%s; compiled walks %llu (%llu with bindless tables on the interpreter), fell back %llu (%llu on an inaccessible read), uncompiled %llu%s; direct reads: %llu captures, %llu words direct, %llu through the hook, page states %llu looked up, %llu rebuilt\n", static_cast<unsigned long long>(counters.draws), static_cast<unsigned long long>(counters.decodeHits), static_cast<unsigned long long>(counters.decodesPatched), static_cast<unsigned long long>(counters.decodeMisses), static_cast<unsigned long long>(counters.stages), static_cast<unsigned long long>(counters.templateHits), static_cast<unsigned long long>(counters.keyWords), static_cast<unsigned long long>(counters.valueWords), static_cast<unsigned long long>(counters.verifiedDecodes), static_cast<unsigned long long>(counters.decodeMismatches), static_cast<unsigned long long>(counters.verifiedStages), static_cast<unsigned long long>(counters.stageMismatches), parts.c_str(), static_cast<unsigned long long>(walks.runs - walksBefore.runs), static_cast<unsigned long long>(walks.indirectImages - walksBefore.indirectImages), static_cast<unsigned long long>(walks.fallbacks - walksBefore.fallbacks), static_cast<unsigned long long>(walks.fallbacksInaccessible - walksBefore.fallbacksInaccessible), static_cast<unsigned long long>(walks.uncompiled - walksBefore.uncompiled), refusals.c_str(), static_cast<unsigned long long>(reads.captures - readsBefore.captures), static_cast<unsigned long long>(reads.words - readsBefore.words), static_cast<unsigned long long>(reads.hookedWords - readsBefore.hookedWords), static_cast<unsigned long long>(reads.pageStateLookups - readsBefore.pageStateLookups), static_cast<unsigned long long>(reads.pageStateMisses - readsBefore.pageStateMisses));
    walksBefore = walks;
    readsBefore = reads;
    const auto storage = ShaderRecompiler::GetMaterializeValuesCounts();
    AgcDriver::ProfilePrint_nid_no_patch("[binding-plan-storage] process cumulative: %llu pooled results created, %llu storage reuses, %llu releases not retained; currently %llu bytes retained\n", static_cast<unsigned long long>(storage.storageCreated), static_cast<unsigned long long>(storage.storageReused), static_cast<unsigned long long>(storage.storageRefused), static_cast<unsigned long long>(storage.storageRetainedBytes));
    counters = {};
    counters.lastReport = now;
}

}

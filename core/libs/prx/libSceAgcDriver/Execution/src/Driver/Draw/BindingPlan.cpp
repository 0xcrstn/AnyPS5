#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
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
    static const bool verify = std::getenv("APS5_VERIFY_BINDING_PLAN") != nullptr;
    return verify;
}

void Driver::planDrawDecode(const QueueState& queue, const Submission& submission, std::uint64_t deviceSerial, std::shared_ptr<const DrawDecode>& decode, bool profile) {
    const auto key = drawRegisterKey(queue, *submission.shaders, deviceSerial);
    {
        std::lock_guard lock(bindingPlanMutex);
        ++bindingPlanCounters.draws;
        if (auto* found = plannedDecodes.Find(key); found != nullptr && DecodeProgramsCurrent(**found, *submission.shaders)) decode = *found;
    }
    if (decode != nullptr) {
        bool mismatch = false;
        const bool verify = verifyBindingPlan();
        if (verify) mismatch = !sameDecode(*decode, *decodeDraw(queue, submission));
        std::lock_guard lock(bindingPlanMutex);
        ++bindingPlanCounters.decodeHits;
        if (verify) ++bindingPlanCounters.verifiedDecodes;
        if (mismatch) {
            ++bindingPlanCounters.decodeMismatches;
            static std::atomic<std::uint64_t> reports{0};
            if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[binding-plan] verify: the decode kept for register key 0x%llx differs from a fresh decode\n", static_cast<unsigned long long>(key));
        }
        if (profile) reportBindingPlan();
        return;
    }
    std::shared_ptr<const DrawDecode> fresh = decodeDraw(queue, submission);
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

void Driver::notePlannedStage(bool verified, bool mismatch, SnapshotPart part) {
    std::lock_guard lock(bindingPlanMutex);
    ++bindingPlanCounters.stages;
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
    AgcDriver::ProfilePrint_nid_no_patch("[binding-plan] (10 s) %llu draws: decodes kept %llu, made %llu; %llu stages captured without the draw cache; verify: %llu decodes compared, %llu differed; %llu stages compared, %llu differed%s; compiled walks %llu (%llu with bindless tables on the interpreter), fell back %llu (%llu on an inaccessible read), uncompiled %llu%s; direct reads: %llu captures, %llu words direct, %llu through the hook, page states %llu looked up, %llu rebuilt\n", static_cast<unsigned long long>(counters.draws), static_cast<unsigned long long>(counters.decodeHits), static_cast<unsigned long long>(counters.decodeMisses), static_cast<unsigned long long>(counters.stages), static_cast<unsigned long long>(counters.verifiedDecodes), static_cast<unsigned long long>(counters.decodeMismatches), static_cast<unsigned long long>(counters.verifiedStages), static_cast<unsigned long long>(counters.stageMismatches), parts.c_str(), static_cast<unsigned long long>(walks.runs - walksBefore.runs), static_cast<unsigned long long>(walks.indirectImages - walksBefore.indirectImages), static_cast<unsigned long long>(walks.fallbacks - walksBefore.fallbacks), static_cast<unsigned long long>(walks.fallbacksInaccessible - walksBefore.fallbacksInaccessible), static_cast<unsigned long long>(walks.uncompiled - walksBefore.uncompiled), refusals.c_str(), static_cast<unsigned long long>(reads.captures - readsBefore.captures), static_cast<unsigned long long>(reads.words - readsBefore.words), static_cast<unsigned long long>(reads.hookedWords - readsBefore.hookedWords), static_cast<unsigned long long>(reads.pageStateLookups - readsBefore.pageStateLookups), static_cast<unsigned long long>(reads.pageStateMisses - readsBefore.pageStateMisses));
    walksBefore = walks;
    readsBefore = reads;
    counters = {};
    counters.lastReport = now;
}

}

#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
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

void ThrowIncrementalMismatch(std::uint32_t queue, std::uint64_t submission, std::size_t packetOffset, std::size_t stage, std::uint64_t program) {
    throw std::runtime_error("AGC driver: incremental bindings verify: queue " + std::to_string(queue) + " submission " + std::to_string(submission) + " packet at dword " + std::to_string(packetOffset) + ": stage " + std::to_string(stage) + " (program " + HexText(program) + ") kept values that differ from a full values capture of the same guest words");
}

void ThrowPlanInstanceMismatch(std::uint32_t queue, std::uint64_t submission, std::size_t packetOffset, std::size_t stage, std::uint64_t program, const ShaderMemory::PlanCapture& capture, const ShaderRecompiler::ResourceSnapshot& full, const ShaderRecompiler::ResourceSnapshot& served) {
    std::string where = "a part outside the descriptors, flat SRT and user data";
    const auto descriptors = [&](const char* kind, const std::vector<ShaderRecompiler::DescriptorValue>& a, const std::vector<ShaderRecompiler::DescriptorValue>& b) {
        if (a.size() != b.size()) {
            where = std::string(kind) + " count " + std::to_string(b.size()) + " (full " + std::to_string(a.size()) + ")";
            return true;
        }
        for (std::size_t index = 0; index < a.size(); ++index) {
            if (a[index].dwordCount != b[index].dwordCount) {
                where = std::string(kind) + " " + std::to_string(index) + " width " + std::to_string(b[index].dwordCount) + " (full " + std::to_string(a[index].dwordCount) + ")";
                return true;
            }
            for (std::size_t word = 0; word < a[index].dwords.size(); ++word) {
                if (a[index].dwords[word] == b[index].dwords[word]) continue;
                where = std::string(kind) + " " + std::to_string(index) + " word " + std::to_string(word) + ": served " + HexText(b[index].dwords[word]) + ", full " + HexText(a[index].dwords[word]);
                return true;
            }
        }
        return false;
    };
    const auto words = [&](const char* kind, const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
        for (std::size_t index = 0; index < a.size() || index < b.size(); ++index) {
            if (index < a.size() && index < b.size() && a[index] == b[index]) continue;
            where = std::string(kind) + " " + std::to_string(index) + ": served " + (index < b.size() ? HexText(b[index]) : std::string("none")) + ", full " + (index < a.size() ? HexText(a[index]) : std::string("none"));
            return true;
        }
        return false;
    };
    if (!descriptors("buffer descriptor", full.buffers, served.buffers) && !descriptors("image descriptor", full.images, served.images) && !descriptors("sampler", full.samplers, served.samplers) && !words("flat SRT slot", full.flattenedSrt, served.flattenedSrt) && !words("user data word", full.userData, served.userData)) {
        if (!(full.uniformFill == served.uniformFill)) where = "uniform fill";
        else if (full.srtPoison != served.srtPoison) where = "poisoned reads";
        else if (full.nullRootReads != served.nullRootReads) where = "null-root reads";
    }
    throw std::runtime_error("AGC driver: incremental plans verify: queue " + std::to_string(queue) + " submission " + std::to_string(submission) + " packet at dword " + std::to_string(packetOffset) + ": stage " + std::to_string(stage) + " (program " + HexText(program) + ") " + ShaderMemory::PlanOutcomeNames[static_cast<std::size_t>(capture.outcome)] + " (" + ShaderMemory::PlanFallbackNames[static_cast<std::size_t>(capture.fallback)] + ") differs from a full values capture of the same guest words, first in " + where);
}

void ThrowStageMismatch(std::uint32_t queue, std::uint64_t submission, std::size_t packetOffset, std::size_t stage, std::uint64_t program, SnapshotPart part) {
    const auto index = static_cast<std::size_t>(part) < SnapshotParts ? static_cast<std::size_t>(part) : 0;
    throw std::runtime_error("AGC driver: binding plan verify: queue " + std::to_string(queue) + " submission " + std::to_string(submission) + " packet at dword " + std::to_string(packetOffset) + ": stage " + std::to_string(stage) + " (program " + HexText(program) + ") differs from the full capture and materialize, first in: " + SnapshotPartNames[index]);
}

bool SameStageResult(const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b) {
    if (!(a.spirv == b.spirv) || a.variantId != b.variantId || a.specializationId != b.specializationId || a.pushConstants != b.pushConstants || a.poisonedSrtReads != b.poisonedSrtReads) return false;
    if (a.workgroupMemoryDwords != b.workgroupMemoryDwords || a.bdaWrites != b.bdaWrites || a.bdaAtomics != b.bdaAtomics || a.bdaCoherent != b.bdaCoherent || a.bdaSpinWait != b.bdaSpinWait || a.shaderDataUserWords != b.shaderDataUserWords || a.bufferBaseSlots != b.bufferBaseSlots) return false;
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
        return text == nullptr || std::strcmp(text, "0") != 0;
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

bool Driver::incrementalBindings() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_INCREMENTAL_BINDINGS");
        return text == nullptr || std::strcmp(text, "0") != 0;
    }();
    return enabled;
}

bool Driver::verifyIncrementalBindings() {
    static const bool verify = [] {
        const char* text = std::getenv("APS5_VERIFY_INCREMENTAL_BINDINGS");
        return text != nullptr && std::strcmp(text, "0") != 0;
    }();
    return verify;
}

ShaderMemory::IncrementalState& Driver::incrementalBinding(const std::shared_ptr<const ShaderRecompiler::SourceHandle>& handle, std::span<const std::uint32_t> userData) {
    struct Way {
        std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
        std::vector<std::uint32_t> userData;
        ShaderMemory::IncrementalState state;
        std::uint64_t used = 0;
    };
    constexpr std::size_t Ways = 4;
    constexpr std::size_t Shaders = 1024;
    struct Table {
        std::unordered_map<const ShaderRecompiler::SourceHandle*, std::array<Way, Ways>> shaders;
        std::uint64_t clock = 0;
    };
    struct TableStorage {};
    auto& table = HostThreadLocal<Table, TableStorage>();
    auto found = table.shaders.find(handle.get());
    if (found == table.shaders.end()) {
        if (table.shaders.size() >= Shaders) table.shaders.clear();
        found = table.shaders.try_emplace(handle.get()).first;
    }
    auto& ways = found->second;
    Way* chosen = nullptr;
    std::size_t best = 0;
    Way* victim = &ways.front();
    for (auto& way : ways) {
        if (way.handle == nullptr) {
            if (victim->handle != nullptr) victim = &way;
            continue;
        }
        if (victim->handle != nullptr && way.used < victim->used) victim = &way;
        if (way.handle != handle) continue;
        std::size_t same = 0;
        for (std::size_t index = 0; index < way.userData.size() && index < userData.size(); ++index) same += way.userData[index] == userData[index] ? 1u : 0u;
        if (chosen == nullptr || same > best) {
            chosen = &way;
            best = same;
        }
    }
    if (chosen == nullptr || (best == 0 && victim->handle == nullptr)) {
        *victim = Way{};
        victim->handle = handle;
        chosen = victim;
    }
    chosen->used = ++table.clock;
    chosen->userData.assign(userData.begin(), userData.end());
    return chosen->state;
}

void Driver::noteIncrementalStage(const ShaderRecompiler::SrtTapeInstance& instance, bool verified, bool stale) {
    std::lock_guard lock(bindingPlanMutex);
    auto& counters = bindingPlanCounters;
    ++counters.incrementalStages;
    counters.keptReads += instance.keptReads;
    counters.freshReads += instance.freshReads;
    counters.keptValues += instance.keptValues;
    if (verified) ++counters.incrementalVerified;
    if (stale) ++counters.incrementalStale;
}

bool Driver::incrementalPlans() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_INCREMENTAL_PLANS");
        return text == nullptr || std::strcmp(text, "0") != 0;
    }();
    return enabled;
}

bool Driver::verifyIncrementalPlans() {
    static const bool verify = [] {
        const char* text = std::getenv("APS5_VERIFY_INCREMENTAL_PLANS");
        return text != nullptr && std::strcmp(text, "0") != 0;
    }();
    return verify;
}

ShaderMemory::PlanInstance& Driver::resourcePlanInstance(const std::shared_ptr<const ShaderRecompiler::SourceHandle>& handle, std::uint32_t pushOffset, std::span<const std::uint32_t> userData) {
    struct Way {
        std::uint32_t pushOffset = 0;
        ShaderMemory::PlanInstance instance;
        std::uint64_t used = 0;
    };
    constexpr std::size_t Ways = 4;
    constexpr std::size_t Shaders = 1024;
    struct Table {
        std::unordered_map<const ShaderRecompiler::SourceHandle*, std::array<Way, Ways>> shaders;
        std::uint64_t clock = 0;
    };
    struct PlanTableStorage {};
    auto& table = HostThreadLocal<Table, PlanTableStorage>();
    auto found = table.shaders.find(handle.get());
    if (found == table.shaders.end()) {
        if (table.shaders.size() >= Shaders) table.shaders.clear();
        found = table.shaders.try_emplace(handle.get()).first;
    }
    auto& ways = found->second;
    Way* chosen = nullptr;
    std::size_t best = 0;
    Way* victim = &ways.front();
    for (auto& way : ways) {
        if (way.instance.handle == nullptr) {
            if (victim->instance.handle != nullptr) victim = &way;
            continue;
        }
        if (victim->instance.handle != nullptr && way.used < victim->used) victim = &way;
        if (way.instance.handle != handle || way.pushOffset != pushOffset) continue;
        std::size_t same = 0;
        for (std::size_t index = 0; index < way.instance.userData.size() && index < userData.size(); ++index) same += way.instance.userData[index] == userData[index] ? 1u : 0u;
        if (chosen == nullptr || same > best) {
            chosen = &way;
            best = same;
        }
    }
    if (chosen == nullptr || (best == 0 && victim->instance.handle == nullptr)) {
        *victim = Way{};
        victim->pushOffset = pushOffset;
        victim->instance.handle = handle;
        chosen = victim;
    }
    chosen->used = ++table.clock;
    return chosen->instance;
}

void Driver::notePlanInstance(const ShaderMemory::PlanCapture& capture, bool verified, bool stale) {
    std::lock_guard lock(bindingPlanMutex);
    auto& counters = bindingPlanCounters;
    ++counters.planStages;
    counters.planNodesKept += capture.nodesKept;
    counters.planNodesFresh += capture.nodesFresh;
    ++counters.planOutcomes[static_cast<std::size_t>(capture.outcome)];
    ++counters.planFallbacks[static_cast<std::size_t>(capture.fallback)];
    if (verified) ++counters.planVerified;
    if (stale) ++counters.planStale;
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
    if (counters.incrementalStages != 0) AgcDriver::ProfilePrint_nid_no_patch("[binding-plan-incremental] (10 s) %llu stages: %llu guest words kept, %llu read, %llu values kept; verify: %llu stages compared, %llu served stale words a CPU store had changed within the collect epoch\n", static_cast<unsigned long long>(counters.incrementalStages), static_cast<unsigned long long>(counters.keptReads), static_cast<unsigned long long>(counters.freshReads), static_cast<unsigned long long>(counters.keptValues), static_cast<unsigned long long>(counters.incrementalVerified), static_cast<unsigned long long>(counters.incrementalStale));
    if (counters.planStages != 0) {
        std::string outcomes;
        for (std::size_t outcome = 0; outcome < ShaderMemory::PlanOutcomes; ++outcome) outcomes += std::string(outcomes.empty() ? "" : ", ") + ShaderMemory::PlanOutcomeNames[outcome] + " " + std::to_string(counters.planOutcomes[outcome]);
        std::string fallbacks;
        for (std::size_t reason = 1; reason < ShaderMemory::PlanFallbacks; ++reason) {
            if (counters.planFallbacks[reason] == 0) continue;
            fallbacks += std::string(fallbacks.empty() ? "" : ", ") + ShaderMemory::PlanFallbackNames[reason] + " " + std::to_string(counters.planFallbacks[reason]);
        }
        AgcDriver::ProfilePrint_nid_no_patch("[binding-plan-instances] (10 s) %llu stages: %s; not reused because: %s; nodes kept %llu, re-read %llu; verify: %llu stages compared, %llu served words a CPU store had changed within the collect epoch\n", static_cast<unsigned long long>(counters.planStages), outcomes.c_str(), fallbacks.empty() ? "nothing" : fallbacks.c_str(), static_cast<unsigned long long>(counters.planNodesKept), static_cast<unsigned long long>(counters.planNodesFresh), static_cast<unsigned long long>(counters.planVerified), static_cast<unsigned long long>(counters.planStale));
    }
    counters = {};
    counters.lastReport = now;
}

}

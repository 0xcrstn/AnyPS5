#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace AgcDriver::DriverDetail {

bool Driver::drawMissDiagnosisEnabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr && std::getenv("APS5_NO_DRAW_MISS_DIAGNOSIS") == nullptr;
    return enabled;
}

bool DrawMissDiagnosis::AddressLike(std::uint32_t before, std::uint32_t after) {
    if (before == 0 || after == 0 || before == after || ((before ^ after) & 3u) != 0) return false;
    const auto delta = before > after ? before - after : after - before;
    return delta < (1u << 26u);
}

DrawStageMiss DrawMissDiagnosis::Classify(const DispatchVariant& variant, std::uint32_t before, std::uint32_t after) {
    const auto value = before != 0 ? before : after;
    if (value == 0 || variant.compiled == nullptr) return DrawStageMiss::Unbound;
    const auto& compiled = *variant.compiled;
    std::optional<DrawStageMiss> found;
    const auto rank = [](DrawStageMiss reason) { return static_cast<std::size_t>(reason); };
    const auto consider = [&](DrawStageMiss reason) {
        if (!found || rank(reason) < rank(*found)) found = reason;
    };
    for (const auto& binding : compiled.bindings) {
        for (std::size_t word = 0; word < binding.guestDescriptor.size(); ++word) {
            if (binding.guestDescriptor[word] != value) continue;
            switch (binding.role) {
            case ShaderRecompiler::DescriptorRole::GuestBuffers:
                consider(word % 4u == 0 ? DrawStageMiss::BufferBase : DrawStageMiss::BufferWord);
                break;
            case ShaderRecompiler::DescriptorRole::GuestImages:
            case ShaderRecompiler::DescriptorRole::GuestSamplers:
                consider(DrawStageMiss::ImageWord);
                break;
            case ShaderRecompiler::DescriptorRole::FlattenedSrt:
                consider(DrawStageMiss::FlatWord);
                break;
            case ShaderRecompiler::DescriptorRole::ShaderData:
                consider(DrawStageMiss::ShaderData);
                break;
            default:
                break;
            }
        }
    }
    if (found) return *found;
    for (std::size_t at = 0; at + sizeof(std::uint32_t) <= compiled.pushConstants.size(); at += sizeof(std::uint32_t)) {
        std::uint32_t word = 0;
        std::memcpy(&word, compiled.pushConstants.data() + at, sizeof(word));
        if (word == value) return DrawStageMiss::Constant;
    }
    for (const auto& constant : compiled.specialization) {
        if (constant.value == value) return DrawStageMiss::Constant;
    }
    return DrawStageMiss::Unbound;
}

void Driver::noteDrawKeyMiss(DrawKeyMiss reason) {
    ++drawMissDiagnosis.keys[static_cast<std::size_t>(reason)];
}

void Driver::diagnoseStructureMiss(const DrawAliasKeys& aliases) {
    auto& diagnosis = drawMissDiagnosis;
    const auto learned = diagnosis.learned.find(drawRelocationPending.identity);
    if (learned == diagnosis.learned.end()) return noteDrawKeyMiss(DrawKeyMiss::NewStructure);
    const auto& before = learned->second;
    if (before.shape != aliases.parts.shape) return noteDrawKeyMiss(DrawKeyMiss::RegisterState);
    const auto valueOf = [](const DrawStructureParts& parts, std::uint32_t offset) -> std::optional<std::uint32_t> {
        const auto found = std::find_if(parts.words.begin(), parts.words.end(), [&](const std::pair<std::uint32_t, std::uint32_t>& word) { return word.first == offset; });
        if (found == parts.words.end()) return std::nullopt;
        return found->second;
    };
    std::optional<std::pair<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>> differing;
    for (const auto& [offset, value] : aliases.parts.words) {
        const auto old = valueOf(before, offset);
        if (old && *old == value) continue;
        differing = std::make_pair(offset, std::make_pair(old.value_or(0), value));
        break;
    }
    if (!differing) {
        for (const auto& [offset, value] : before.words) {
            if (valueOf(aliases.parts, offset)) continue;
            differing = std::make_pair(offset, std::make_pair(value, 0u));
            break;
        }
    }
    if (!differing) return noteDrawKeyMiss(DrawKeyMiss::StructureDropped);
    const auto reg = differing->first;
    const auto& relocation = drawRelocationPending.relocation;
    const auto listed = [&](const std::vector<std::uint32_t>& list) { return std::find(list.begin(), list.end(), reg) != list.end(); };
    if (relocation != nullptr && listed(relocation->banned)) noteDrawKeyMiss(DrawKeyMiss::ValueWord);
    else if (relocation != nullptr && listed(relocation->read)) noteDrawKeyMiss(DrawKeyMiss::ReadWord);
    else noteDrawKeyMiss(DrawKeyMiss::OtherWord);
    if (DrawMissDiagnosis::AddressLike(differing->second.first, differing->second.second)) ++diagnosis.wordAddressLike;
}

void Driver::diagnoseStageMiss(const DrawAliasKeys& aliases, std::size_t stage, std::uint32_t userDataBase, std::uint64_t donorKey, bool refused) {
    auto& diagnosis = drawMissDiagnosis;
    const auto note = [&](DrawStageMiss reason) { ++diagnosis.stages[static_cast<std::size_t>(reason)]; };
    if (refused) return note(DrawStageMiss::Refused);
    if (donorKey == 0) return note(DrawStageMiss::NoStageKey);
    if (diagnosis.evictedDonors.Find(donorKey) != nullptr) return note(DrawStageMiss::DonorEvicted);
    const auto* sample = diagnosis.samples.Find(DrawMissDiagnosis::SampleKey(aliases.structure, stage));
    if (sample == nullptr || sample->variant == nullptr) return note(DrawStageMiss::NoDonor);
    const auto& now = aliases.raw[DrawAliasKeys::Stage(userDataBase)];
    if (now.size() != sample->raw.size()) return note(DrawStageMiss::ShapeChanged);
    std::size_t first = now.size();
    std::size_t differing = 0;
    for (std::size_t word = 0; word < now.size(); ++word) {
        if (now[word] == sample->raw[word]) continue;
        if (differing++ == 0) first = word;
    }
    if (differing == 0) return note(DrawStageMiss::DonorDropped);
    note(DrawMissDiagnosis::Classify(*sample->variant, sample->raw[first], now[first]));
    if (DrawMissDiagnosis::AddressLike(sample->raw[first], now[first])) ++diagnosis.contentAddressLike;
    if (differing == 1) ++diagnosis.oneWord;
    else if (differing <= 4) ++diagnosis.fewWords;
    else ++diagnosis.manyWords;
}

void Driver::reportDrawMisses() {
    auto& diagnosis = drawMissDiagnosis;
    const auto key = [&](DrawKeyMiss reason) { return static_cast<unsigned long long>(diagnosis.keys[static_cast<std::size_t>(reason)]); };
    const auto stage = [&](DrawStageMiss reason) { return static_cast<unsigned long long>(diagnosis.stages[static_cast<std::size_t>(reason)]); };
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    AgcDriver::ProfilePrint_nid_no_patch("[draw-miss] exact-key misses (10 s): shader set not learned %llu, relocation idle %llu, pointer unreadable %llu; unknown structure: new %llu, register state %llu, banned pointer word %llu, read user word %llu, other user word %llu (address-like %llu), structure dropped %llu; relocated %llu\n", key(DrawKeyMiss::Unlearned), key(DrawKeyMiss::Idle), key(DrawKeyMiss::Unreadable), key(DrawKeyMiss::NewStructure), key(DrawKeyMiss::RegisterState), key(DrawKeyMiss::ValueWord), key(DrawKeyMiss::ReadWord), key(DrawKeyMiss::OtherWord), count(diagnosis.wordAddressLike), key(DrawKeyMiss::StructureDropped), key(DrawKeyMiss::Relocated));
    AgcDriver::ProfilePrint_nid_no_patch("[draw-miss] relocated stages without a donor (10 s): no stage key %llu, donor evicted %llu, no donor for the structure %llu, relocation shape changed %llu, same content but donor dropped %llu, refused %llu; first differing content word: buffer base %llu, other buffer word %llu, image or sampler word %llu, flattened SRT word %llu, shader data word %llu, constant %llu, unbound %llu (address-like %llu; 1 word differs %llu, 2-4 %llu, more %llu)\n", stage(DrawStageMiss::NoStageKey), stage(DrawStageMiss::DonorEvicted), stage(DrawStageMiss::NoDonor), stage(DrawStageMiss::ShapeChanged), stage(DrawStageMiss::DonorDropped), stage(DrawStageMiss::Refused), stage(DrawStageMiss::BufferBase), stage(DrawStageMiss::BufferWord), stage(DrawStageMiss::ImageWord), stage(DrawStageMiss::FlatWord), stage(DrawStageMiss::ShaderData), stage(DrawStageMiss::Constant), stage(DrawStageMiss::Unbound), count(diagnosis.contentAddressLike), count(diagnosis.oneWord), count(diagnosis.fewWords), count(diagnosis.manyWords));
    const auto puts = diagnosis.donorPuts;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-miss] stage donors (10 s): %llu lookups, %llu found; %llu puts (%llu new keys, %llu evictions), ~%.1f KiB per donor put, %zu held of %zu\n", count(diagnosis.donorLookups), count(diagnosis.donorFound), count(puts), count(diagnosis.donorNew), count(diagnosis.donorEvictions), puts != 0 ? static_cast<double>(diagnosis.donorBytes) / static_cast<double>(puts) / 1024.0 : 0.0, drawStageDonors.Size(), MaxDrawAliases);
    diagnosis.donorLookups = diagnosis.donorFound = diagnosis.donorPuts = diagnosis.donorNew = diagnosis.donorEvictions = diagnosis.donorBytes = 0;
    diagnosis.keys = {};
    diagnosis.stages = {};
    diagnosis.wordAddressLike = diagnosis.contentAddressLike = diagnosis.oneWord = diagnosis.fewWords = diagnosis.manyWords = 0;
}

}

#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>

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

void Driver::diagnoseValidationMiss(const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions) {
    std::vector<std::uint32_t> live;
    std::size_t position = 0, differing = 0, firstRun = 0, firstPosition = 0;
    std::uint32_t before = 0, after = 0;
    std::uint64_t dataWords = 0, flatWords = 0, otherWords = 0;
    bool unreadable = false;
    const std::vector<std::uint32_t>* flat = nullptr;
    if (variant.compiled != nullptr) {
        for (const auto& binding : variant.compiled->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt) flat = &binding.guestDescriptor;
        }
    }
    for (std::size_t run = 0; run < regions.size(); ++run) {
        const auto& region = regions[run];
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        live.resize(count);
        if (GuestMemory::CopyMapped(region.guestAddress, std::as_writable_bytes(std::span(live))) != GuestMemory::Compare::Equal) {
            unreadable = true;
            break;
        }
        for (std::size_t word = 0; word < count; ++word) {
            std::uint32_t stored = 0;
            std::memcpy(&stored, region.bytes.data() + word * sizeof(std::uint32_t), sizeof(stored));
            if (stored == live[word]) continue;
            if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), static_cast<std::uint32_t>(position + word))) ++dataWords;
            else if (flat != nullptr && std::find(flat->begin(), flat->end(), stored) != flat->end()) ++flatWords;
            else ++otherWords;
            if (differing++ == 0) {
                firstRun = run;
                firstPosition = position + word;
                before = stored;
                after = live[word];
            }
        }
        position += count;
    }
    std::lock_guard cacheLock(drawCacheMutex);
    auto& diagnosis = drawMissDiagnosis;
    ++diagnosis.validationMisses;
    if (unreadable) {
        ++diagnosis.validationUnreadable;
        return;
    }
    if (differing == 0) {
        ++diagnosis.validationNone;
        return;
    }
    diagnosis.validationDataWords += dataWords;
    diagnosis.validationFlatWords += flatWords;
    diagnosis.validationOtherWords += otherWords;
    if (flatWords == 0 && otherWords == 0) ++diagnosis.validationOnlyData;
    if (firstRun < variant.runBases.size() && variant.runBases[firstRun] >= 0) ++diagnosis.validationRelocatedRun;
    else ++diagnosis.validationAbsoluteRun;
    if (std::binary_search(variant.dataPositions.begin(), variant.dataPositions.end(), static_cast<std::uint32_t>(firstPosition))) ++diagnosis.validationDataWord;
    ++diagnosis.validationRoles[static_cast<std::size_t>(DrawMissDiagnosis::Classify(variant, before, after))];
    if (DrawMissDiagnosis::AddressLike(before, after)) ++diagnosis.validationAddressLike;
    if (differing == 1) ++diagnosis.validationOneWord;
    else if (differing <= 4) ++diagnosis.validationFewWords;
    else ++diagnosis.validationManyWords;
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
    std::uint64_t heldBytes = 0, heldOptions = 0, cyclingKeys = 0;
    drawStageDonors.ForEach([&](const DrawStageDonor& donor) {
        heldBytes += donor.Bytes();
        heldOptions += donor.options.size();
        if (donor.cycling) ++cyclingKeys;
    });
    const auto role = [&](DrawStageMiss reason) { return count(diagnosis.validationRoles[static_cast<std::size_t>(reason)]); };
    AgcDriver::ProfilePrint_nid_no_patch("[draw-miss] stage donor replacements (10 s): same content %llu, content seen before under the key %llu, new content %llu; %llu options tried, %llu alternate options hit; held: %llu options in %llu keys cycling, ~%.1f MiB\n", count(diagnosis.donorSame), count(diagnosis.donorRecurring), count(diagnosis.donorFresh), count(diagnosis.donorOptionsTried), count(diagnosis.donorAlternateHits), count(heldOptions), count(cyclingKeys), static_cast<double>(heldBytes) / (1024.0 * 1024.0));
    AgcDriver::ProfilePrint_nid_no_patch("[draw-miss] cached stages that differed on validation (10 s): %llu (no differing word %llu, unreadable %llu); first differing word in a run read through a user pointer %llu, at a fixed address %llu, a data word that could not be patched %llu; role: buffer base %llu, other buffer word %llu, image or sampler word %llu, flattened SRT word %llu, shader data word %llu, constant %llu, unbound %llu (address-like %llu; 1 word differs %llu, 2-4 %llu, more %llu)\n", count(diagnosis.validationMisses), count(diagnosis.validationNone), count(diagnosis.validationUnreadable), count(diagnosis.validationRelocatedRun), count(diagnosis.validationAbsoluteRun), count(diagnosis.validationDataWord), role(DrawStageMiss::BufferBase), role(DrawStageMiss::BufferWord), role(DrawStageMiss::ImageWord), role(DrawStageMiss::FlatWord), role(DrawStageMiss::ShaderData), role(DrawStageMiss::Constant), role(DrawStageMiss::Unbound), count(diagnosis.validationAddressLike), count(diagnosis.validationOneWord), count(diagnosis.validationFewWords), count(diagnosis.validationManyWords));
    diagnosis.donorLookups = diagnosis.donorFound = diagnosis.donorPuts = diagnosis.donorNew = diagnosis.donorEvictions = diagnosis.donorBytes = 0;
    diagnosis.donorSame = diagnosis.donorRecurring = diagnosis.donorFresh = diagnosis.donorOptionsTried = diagnosis.donorAlternateHits = 0;
    diagnosis.validationMisses = diagnosis.validationNone = diagnosis.validationUnreadable = diagnosis.validationRelocatedRun = diagnosis.validationAbsoluteRun = diagnosis.validationDataWord = diagnosis.validationAddressLike = diagnosis.validationOneWord = diagnosis.validationFewWords = diagnosis.validationManyWords = 0;
    diagnosis.validationRoles = {};
    AgcDriver::ProfilePrint_nid_no_patch("[draw-miss] differing words of those stages (10 s): %llu data positions, %llu other words found in the flattened SRT, %llu other words; %llu stages differed only in data positions; tagging: %llu stages captured, %llu pure flat-SRT leaves offered, %llu tagged as data, skipped: %llu outside the captured runs, %llu unlike the flattened SRT, %llu also read by the walk\n", count(diagnosis.validationDataWords), count(diagnosis.validationFlatWords), count(diagnosis.validationOtherWords), count(diagnosis.validationOnlyData), count(diagnosis.stagesTagged), count(diagnosis.leavesOffered), count(diagnosis.leavesTagged), count(diagnosis.leavesUnmapped), count(diagnosis.leavesMismatched), count(diagnosis.leavesAliased));
    diagnosis.validationDataWords = diagnosis.validationFlatWords = diagnosis.validationOtherWords = diagnosis.validationOnlyData = 0;
    diagnosis.stagesTagged = diagnosis.leavesOffered = diagnosis.leavesTagged = diagnosis.leavesUnmapped = diagnosis.leavesMismatched = diagnosis.leavesAliased = 0;
    diagnosis.keys = {};
    diagnosis.stages = {};
    diagnosis.wordAddressLike = diagnosis.contentAddressLike = diagnosis.oneWord = diagnosis.fewWords = diagnosis.manyWords = 0;
}

}

#include "ControlFlow/GraphBuilder.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace AgcDriver::DriverDetail {

namespace {

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

bool Bit(std::uint64_t mask, std::size_t index) {
    return index < 64 && ((mask >> index) & 1u) != 0;
}

std::size_t WordBucket(std::size_t words) {
    return words <= 1 ? 0 : words <= 4 ? 1 : 2;
}

}

bool Driver::sameResourceBindings(const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b) {
    if (&a == &b) return true;
    if (a.bindings.size() != b.bindings.size() || a.pushConstants != b.pushConstants) return false;
    for (std::size_t index = 0; index < a.bindings.size(); ++index) {
        const auto& left = a.bindings[index];
        const auto& right = b.bindings[index];
        if (left.kind != right.kind || left.role != right.role || left.binding != right.binding || left.count != right.count) return false;
        const bool data = left.role == ShaderRecompiler::DescriptorRole::ShaderData || left.role == ShaderRecompiler::DescriptorRole::FlattenedSrt;
        if (!data && left.guestDescriptor != right.guestDescriptor) return false;
    }
    return true;
}

bool Driver::dispatchRelocationEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DISPATCH_RELOCATION") == nullptr;
    return enabled;
}

bool Driver::dispatchMissDiagnosisEnabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_DRAW") != nullptr && std::getenv("APS5_NO_DISPATCH_MISS_DIAGNOSIS") == nullptr;
    return enabled;
}

std::uint64_t Driver::userDataCallWords(std::span<const std::uint32_t> code, std::size_t userWords) {
    if (std::none_of(code.begin(), code.end(), [](std::uint32_t word) { return (word & 0xff80ff00u) == 0xbe802100u; })) return 0;
    const auto program = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(code);
    const ShaderRecompiler::SwappcInfo swappc{false, 0, static_cast<std::uint32_t>(userWords)};
    std::uint64_t words = 0;
    for (const auto& call : ShaderRecompiler::FindUserDataCalls(program, swappc)) {
        for (const auto slot : {call.userDataRegister, call.userDataRegister + 1u}) words |= slot < 64 ? std::uint64_t {1} << slot : ~std::uint64_t {0};
    }
    return words;
}

std::uint64_t Driver::dispatchStructureKey(const DispatchKeyParts& parts, const DispatchShape& shape) {
    std::uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    mix(parts.identity);
    mix(shape.relocatable);
    mix(parts.userData.size());
    for (std::size_t index = 0; index < parts.userData.size(); ++index) {
        const auto value = parts.userData[index];
        if (Bit(shape.relocatable, index)) mix((value & 3u) | (value == 0 ? 4u : 0u));
        else mix(value);
    }
    return key == 0 ? 1 : key;
}

void Driver::learnDispatchDonor(const DispatchKeyParts& parts, const std::shared_ptr<DispatchVariant>& variant) {
    if (parts.identity == 0 || variant == nullptr || variant->compiled == nullptr) return;
    auto& counters = dispatchRelocationCounters;
    auto* shape = dispatchShapes.Find(parts.identity);
    if (shape == nullptr) {
        dispatchShapes.Put(parts.identity, DispatchShape{variant->relocatableWords, variant->userDataReads, parts.userData.size(), {}});
        shape = dispatchShapes.Find(parts.identity);
    } else {
        const auto relocatable = shape->relocatable | variant->relocatableWords;
        const auto read = shape->read | variant->userDataReads;
        if (relocatable != shape->relocatable || read != shape->read) {
            shape->relocatable = relocatable;
            shape->read = read;
            ++counters.shapeChanged;
        }
    }
    if (dispatchMissDiagnosisEnabled()) shape->lastUserData.assign(parts.userData.begin(), parts.userData.end());
    std::uint64_t evicted = 0;
    const bool added = dispatchDonors.Put(dispatchStructureKey(parts, *shape), DispatchDonor{variant, std::vector<std::uint32_t>(parts.userData.begin(), parts.userData.end()), parts.identity}, &evicted);
    ++counters.donorPuts;
    if (added) ++counters.donorNew;
    if (evicted != 0) {
        ++counters.donorEvictions;
        if (dispatchMissDiagnosisEnabled()) evictedDispatchDonors.Put(evicted, true);
    }
}

void Driver::noteDispatchMiss(DispatchMiss reason) {
    ++dispatchRelocationCounters.misses[static_cast<std::size_t>(reason)];
    ++dispatchCacheTotals.relocationMisses;
}

void Driver::diagnoseDispatchIdentity(const DispatchKeyParts& parts) {
    auto& counters = dispatchRelocationCounters;
    if (lastDispatchKeys.size() > 65536) lastDispatchKeys.clear();
    const auto [found, inserted] = lastDispatchKeys.try_emplace(parts.program, DispatchKeyLast{parts.components});
    auto reason = DispatchIdentityMiss::FirstSight;
    if (!inserted) {
        reason = DispatchIdentityMiss::SameComponents;
        constexpr std::array<DispatchIdentityMiss, static_cast<std::size_t>(DispatchKeyComponent::Count)> reasons{DispatchIdentityMiss::WaveSize, DispatchIdentityMiss::PartialThreads, DispatchIdentityMiss::Registers, DispatchIdentityMiss::Shader};
        for (std::size_t component = 0; component < parts.components.size(); ++component) {
            if (found->second.components[component] == parts.components[component]) continue;
            reason = reasons[component];
            break;
        }
        found->second.components = parts.components;
    }
    ++counters.identity[static_cast<std::size_t>(reason)];
}

void Driver::diagnoseDispatchWords(const DispatchKeyParts& parts, const DispatchShape& shape, std::uint64_t structure) {
    auto& counters = dispatchRelocationCounters;
    if (evictedDispatchDonors.Find(structure) != nullptr) ++counters.donorEvicted;
    const auto& last = shape.lastUserData;
    std::size_t differing = 0;
    auto first = DispatchWordMiss::None;
    bool addressLike = false;
    for (std::size_t index = 0; index < parts.userData.size(); ++index) {
        const auto before = index < last.size() ? last[index] : 0u;
        const auto after = parts.userData[index];
        if (index < last.size() && before == after) continue;
        const auto reason = Bit(shape.relocatable, index) ? DispatchWordMiss::ReadBase : index < 64 && !Bit(shape.read, index) ? DispatchWordMiss::Unread : DispatchWordMiss::ReadValue;
        if (first == DispatchWordMiss::None || static_cast<std::size_t>(reason) < static_cast<std::size_t>(first)) first = reason;
        if (DrawMissDiagnosis::AddressLike(before, after)) addressLike = true;
        ++differing;
    }
    if (last.empty()) first = DispatchWordMiss::None;
    ++counters.words[static_cast<std::size_t>(first)];
    if (addressLike) ++counters.wordAddressLike;
    if (differing != 0) ++counters.wordCounts[WordBucket(differing)];
}

void Driver::diagnoseDispatchContent(const DispatchVariant& relocated) {
    auto& counters = dispatchRelocationCounters;
    std::size_t position = 0, differing = 0;
    std::optional<DrawStageMiss> first;
    bool addressLike = false;
    std::vector<std::uint32_t> live;
    for (const auto& [begin, end] : relocated.runs) {
        const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
        live.resize(count);
        if (GuestMemory::CopyMapped(begin, std::as_writable_bytes(std::span(live))) != GuestMemory::Compare::Equal) {
            if (!first) first = DrawStageMiss::Unbound;
            position += count;
            continue;
        }
        for (std::size_t i = 0; i < count && position + i < relocated.words.size(); ++i) {
            const auto before = relocated.words[position + i];
            if (before == live[i] || std::binary_search(relocated.dataPositions.begin(), relocated.dataPositions.end(), static_cast<std::uint32_t>(position + i))) continue;
            const auto reason = DrawMissDiagnosis::Classify(relocated, before, live[i]);
            if (!first || static_cast<std::size_t>(reason) < static_cast<std::size_t>(*first)) first = reason;
            if (DrawMissDiagnosis::AddressLike(before, live[i])) addressLike = true;
            ++differing;
        }
        position += count;
    }
    if (first) ++counters.content[static_cast<std::size_t>(*first)];
    if (addressLike) ++counters.contentAddressLike;
    if (differing != 0) ++counters.contentCounts[WordBucket(differing)];
}

std::shared_ptr<DispatchVariant> Driver::relocatedDispatchVariant(const DispatchDonor& donor, std::span<const std::uint32_t> userData, std::uint64_t& moves) {
    struct Move {
        std::uint32_t index, from, to;
    };
    std::vector<Move> moved;
    const auto& variant = *donor.variant;
    for (std::uint32_t index = 0; index < userData.size(); ++index) {
        const auto now = userData[index];
        const auto from = donor.userData[index];
        if (now == from) continue;
        if (!Bit(variant.relocatableWords, index) || from == 0 || now == 0 || ((from ^ now) & 3u) != 0) return nullptr;
        moved.push_back({index, from, now});
    }
    auto copy = cloneVariant(variant);
    for (const auto& move : moved) {
        const auto delta = AlignedWord(move.to) - AlignedWord(move.from);
        for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
            if (copy->runBases[run] != static_cast<std::int8_t>(move.index)) continue;
            copy->runs[run].first += delta;
            copy->runs[run].second += delta;
        }
        const auto& forwarded = variant.compiled->shaderDataUserWords;
        for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
            if (forwarded[slot] == move.index) copy->shaderDataPatches.emplace_back(slot, move.to);
        }
    }
    moves = moved.size();
    return copy;
}

void Driver::patchDispatchShaderData(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty()) return;
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [slot, value] : variant.shaderDataPatches) {
        const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
        if (at + sizeof(std::uint32_t) <= rebased->pushConstants.size()) std::memcpy(rebased->pushConstants.data() + at, &value, sizeof(value));
        for (auto& binding : rebased->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
        }
    }
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
}

bool Driver::relocateDispatch(std::uint64_t address, const Submission& submission, std::uint64_t key, const DispatchKeyParts& parts, const std::shared_ptr<const ShaderSnapshot>& registeredShader, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::shared_ptr<DispatchEntry>& missedEntry, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit) {
    const bool diagnose = dispatchMissDiagnosisEnabled();
    DispatchDonor donor;
    {
        std::lock_guard cacheLock(dispatchCacheMutex);
        ++dispatchRelocationCounters.exactMisses;
        const auto* shape = dispatchShapes.Find(parts.identity);
        if (shape == nullptr) {
            noteDispatchMiss(DispatchMiss::Unlearned);
            if (diagnose) diagnoseDispatchIdentity(parts);
            return false;
        }
        const auto structure = dispatchStructureKey(parts, *shape);
        const auto* found = dispatchDonors.Find(structure);
        if (found == nullptr || found->identity != parts.identity || found->variant == nullptr || found->variant->compiled == nullptr || found->variant->shader != registeredShader || found->userData.size() != parts.userData.size()) {
            noteDispatchMiss(DispatchMiss::NoDonor);
            if (diagnose) diagnoseDispatchWords(parts, *shape, structure);
            return false;
        }
        donor = *found;
    }
    std::uint64_t moves = 0;
    auto relocated = relocatedDispatchVariant(donor, parts.userData, moves);
    if (relocated == nullptr) {
        std::lock_guard cacheLock(dispatchCacheMutex);
        noteDispatchMiss(DispatchMiss::Refused);
        return false;
    }
    std::vector<ShaderRecompiler::MemoryRegion> regions;
    appendEntryRegions(*relocated, regions);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> live;
    std::uint64_t imagesFlushed = 0, runsSynced = 0;
    EntryOutcome outcome;
    {
        const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DispatchCache);
        std::optional<SampledReadScope> sampling;
        outcome = validateVariant(address, submission.queue, *relocated, regions, imagesFlushed, runsSynced, sampling, &live);
    }
    std::shared_ptr<DispatchVariant> hit;
    if (outcome == EntryOutcome::Equal) hit = std::move(relocated);
    else if (outcome == EntryOutcome::EqualData) hit = patchBufferWords(*relocated, live);
    if (hit == nullptr) {
        std::lock_guard cacheLock(dispatchCacheMutex);
        noteDispatchMiss(outcome == EntryOutcome::Differing ? DispatchMiss::ContentDiffers : outcome == EntryOutcome::EqualData ? DispatchMiss::DataRefused : DispatchMiss::Gate);
        if (diagnose && outcome == EntryOutcome::Differing) diagnoseDispatchContent(*relocated);
        return false;
    }
    patchDispatchShaderData(*hit);
    regions.clear();
    appendEntryRegions(*hit, regions);
    captured.reserve(memory.size() + regions.size());
    captured.assign(memory.begin(), memory.end());
    captured.insert(captured.end(), regions.begin(), regions.end());
    compiledResult = hit->compiled;
    liveWords = hit->words;
    dataHit = outcome == EntryOutcome::EqualData;
    keepVariant = hit;
    std::lock_guard cacheLock(dispatchCacheMutex);
    auto& counters = dispatchRelocationCounters;
    ++counters.hits;
    if (dataHit) ++counters.patchedHits;
    if (moves != 0) ++counters.movedHits;
    counters.moves += moves;
    ++dispatchCacheTotals.relocatedHits;
    if (dataHit) ++dispatchCacheTotals.patchedHits;
    ++dispatchCacheHits;
    entryCounters.imagesFlushed += imagesFlushed;
    entryCounters.runsSynced += runsSynced;
    std::shared_ptr<DispatchVariant> attached;
    storeDispatchVariant(key, hit, missedEntry, attached);
    if (profile && std::chrono::steady_clock::now() - entryCounters.lastReport > std::chrono::seconds(10)) {
        entryCounters.lastReport = std::chrono::steady_clock::now();
        reportDispatchCache(entryCounters);
    }
    return true;
}

void Driver::reportDispatchRelocation() {
    auto& counters = dispatchRelocationCounters;
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto miss = [&](DispatchMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    const auto identity = [&](DispatchIdentityMiss reason) { return count(counters.identity[static_cast<std::size_t>(reason)]); };
    const auto word = [&](DispatchWordMiss reason) { return count(counters.words[static_cast<std::size_t>(reason)]); };
    const auto content = [&](DrawStageMiss reason) { return count(counters.content[static_cast<std::size_t>(reason)]); };
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-miss] exact-key misses (10 s): %llu; relocated hits %llu (%llu with moved user pointers, %llu moves; %llu with patched buffer or data words); misses: shader configuration not learned %llu, no donor for the structure %llu, refused %llu, relocated content differs %llu, data words not patchable %llu, gate %llu\n", count(counters.exactMisses), count(counters.hits), count(counters.movedHits), count(counters.moves), count(counters.patchedHits), miss(DispatchMiss::Unlearned), miss(DispatchMiss::NoDonor), miss(DispatchMiss::Refused), miss(DispatchMiss::ContentDiffers), miss(DispatchMiss::DataRefused), miss(DispatchMiss::Gate));
    if (dispatchMissDiagnosisEnabled()) {
        AgcDriver::ProfilePrint_nid_no_patch("[dispatch-miss] not learned, by key component that differs from the program's last lookup: first sight %llu, wave size %llu, partial threads %llu, registers %llu, registered shader %llu, none (configuration evicted or never inserted) %llu\n", identity(DispatchIdentityMiss::FirstSight), identity(DispatchIdentityMiss::WaveSize), identity(DispatchIdentityMiss::PartialThreads), identity(DispatchIdentityMiss::Registers), identity(DispatchIdentityMiss::Shader), identity(DispatchIdentityMiss::SameComponents));
        AgcDriver::ProfilePrint_nid_no_patch("[dispatch-miss] no donor, by user word that differs from the configuration's last insert: read value word %llu, pointer word changed alignment or null %llu, unread word only %llu, none %llu (donor evicted %llu; address-like %llu; 1 word differs %llu, 2-4 %llu, more %llu); relocated content differs, first differing word: buffer base %llu, other buffer word %llu, image or sampler word %llu, flattened SRT word %llu, shader data word %llu, constant %llu, unbound %llu (address-like %llu; 1 word %llu, 2-4 %llu, more %llu)\n", word(DispatchWordMiss::ReadValue), word(DispatchWordMiss::ReadBase), word(DispatchWordMiss::Unread), word(DispatchWordMiss::None), count(counters.donorEvicted), count(counters.wordAddressLike), count(counters.wordCounts[0]), count(counters.wordCounts[1]), count(counters.wordCounts[2]), content(DrawStageMiss::BufferBase), content(DrawStageMiss::BufferWord), content(DrawStageMiss::ImageWord), content(DrawStageMiss::FlatWord), content(DrawStageMiss::ShaderData), content(DrawStageMiss::Constant), content(DrawStageMiss::Unbound), count(counters.contentAddressLike), count(counters.contentCounts[0]), count(counters.contentCounts[1]), count(counters.contentCounts[2]));
    }
    AgcDriver::ProfilePrint_nid_no_patch("[dispatch-miss] donors (10 s): %llu puts (%llu new structures, %llu evictions), %zu held of %zu; %zu shader configurations, %llu shapes widened\n", count(counters.donorPuts), count(counters.donorNew), count(counters.donorEvictions), dispatchDonors.Size(), MaxDispatchDonors, dispatchShapes.Size(), count(counters.shapeChanged));
    counters = DispatchRelocationCounters{};
}

DispatchCacheTotals Driver::DispatchCacheCounters() {
    std::lock_guard cacheLock(dispatchCacheMutex);
    return dispatchCacheTotals;
}

}

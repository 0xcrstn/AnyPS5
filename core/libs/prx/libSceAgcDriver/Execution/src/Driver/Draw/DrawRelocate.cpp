#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t MaxContentWords = 64;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

}

std::uint64_t UserDataReads(const ShaderRecompiler::SrtUserDataUse& use, const ShaderRecompiler::RecompileResult& compiled, bool decodeFree, std::uint32_t hiddenWords) {
    const auto walked = use.asBase | use.asValue;
    if (walked == 0 || !decodeFree || hiddenWords >= 64) return ~std::uint64_t {0};
    auto reads = walked | (hiddenWords == 0 ? 0 : ~std::uint64_t {0} >> (64u - hiddenWords));
    for (const auto word : compiled.shaderDataUserWords) reads |= word < 64u ? std::uint64_t {1} << word : ~std::uint64_t {0};
    return reads;
}

bool Driver::relocatedOnlyEntries() {
    static const bool enabled = std::getenv("APS5_DRAW_EXACT_RELOCATED_ENTRIES") == nullptr;
    return enabled;
}

bool Driver::drawRelocationEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_RELOCATION") == nullptr;
    return enabled;
}

std::uint64_t Driver::relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable, std::vector<std::uint32_t>* raw) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
    mix(value & 3u);
    if (value == 0) {
        readable = false;
        return hash;
    }
    std::array<std::uint32_t, MaxContentWords> content;
    for (const auto& [offset, words] : word.reads) {
        const auto count = std::min<std::size_t>(words, MaxContentWords);
        const auto address = AlignedWord(value) + static_cast<std::uint64_t>(offset);
        if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content).first(count))) != GuestMemory::Compare::Equal) {
            readable = false;
            return hash;
        }
        mix(static_cast<std::uint64_t>(offset));
        for (std::size_t i = 0; i < count; ++i) {
            const auto keyed = std::find(word.dataOffsets.begin(), word.dataOffsets.end(), offset + static_cast<std::int64_t>(i) * 4) != word.dataOffsets.end() ? 0u : content[i];
            mix(keyed);
            if (raw != nullptr) raw->push_back(keyed);
        }
    }
    return hash;
}

void Driver::tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, bool eligible, std::uint32_t hiddenWords, std::span<const std::pair<std::uint32_t, std::uint64_t>> baseSlots) {
    std::uint64_t relocatable = eligible && drawRelocationEnabled() ? use.asBase & ~use.asValue : 0;
    if (hiddenWords != 0) relocatable &= hiddenWords >= 64 ? 0 : ~((std::uint64_t {1} << hiddenWords) - 1);
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        const auto drop = [&](std::int32_t origin) {
            if (origin >= 0) relocatable &= ~(std::uint64_t {1} << origin);
        };
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (i + 1 < reads.size() && reads[i].first == reads[i + 1].first) {
                drop(reads[i].second);
                drop(reads[i + 1].second);
            }
            if (reads[i].second >= 0 && ((reads[i].first & 3u) != 0 || !covered(reads[i].first))) drop(reads[i].second);
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint32_t>> nested;
    std::vector<std::pair<std::uint64_t, std::int16_t>> dependents;
    if (drawRelocationEnabled() && eligible && !baseSlots.empty() && use.readBases.size() == use.reads.size()) {
        for (const auto& [slot, pointer] : baseSlots) {
            if ((pointer & 3u) != 0 || !covered(pointer) || nested.size() >= 1024) continue;
            bool usable = true, any = false;
            std::vector<std::uint64_t> addresses;
            for (std::size_t i = 0; usable && i < use.reads.size(); ++i) {
                if (use.reads[i].first == pointer && use.readBases[i] != 0) usable = false;
                if (use.readBases[i] != pointer) continue;
                any = true;
                const auto address = use.reads[i].first;
                if ((address & 3u) != 0 || !covered(address) || address == pointer) usable = false;
                for (std::size_t j = 0; usable && j < use.reads.size(); ++j) {
                    if (use.reads[j].first == address && (use.readBases[j] != pointer || use.reads[j].second >= 0)) usable = false;
                }
                addresses.push_back(address);
            }
            if (!usable || !any) continue;
            if (std::any_of(dependents.begin(), dependents.end(), [&](const std::pair<std::uint64_t, std::int16_t>& dependent) { return dependent.first == pointer || std::find(addresses.begin(), addresses.end(), dependent.first) != addresses.end(); })) continue;
            if (std::any_of(nested.begin(), nested.end(), [&](const std::pair<std::uint64_t, std::uint32_t>& base) { return std::find(addresses.begin(), addresses.end(), base.first) != addresses.end(); })) continue;
            const auto index = static_cast<std::int16_t>(nested.size());
            nested.emplace_back(pointer, slot);
            for (const auto address : addresses) dependents.emplace_back(address, index);
        }
        std::sort(dependents.begin(), dependents.end());
        dependents.erase(std::unique(dependents.begin(), dependents.end()), dependents.end());
    }
    const auto key = [&](std::uint64_t address) -> std::int32_t {
        if (!dependents.empty()) {
            const auto found = std::lower_bound(dependents.begin(), dependents.end(), std::make_pair(address, std::numeric_limits<std::int16_t>::min()));
            if (found != dependents.end() && found->first == address) return 64 + found->second;
        }
        if (relocatable == 0) return -1;
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0) return -1;
        return (relocatable >> found->second & 1u) != 0 ? found->second : -1;
    };
    variant.relocatableWords = relocatable;
    const bool split = relocatable != 0 || !nested.empty();
    for (const auto& region : regions) {
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        const auto offset = variant.words.size();
        variant.words.resize(offset + count);
        std::memcpy(variant.words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        for (std::size_t base = 0; base < nested.size(); ++base) {
            const auto pointer = nested[base].first;
            if (pointer >= region.guestAddress && pointer - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size()) variant.nestedBases.emplace_back(static_cast<std::uint32_t>(offset + (pointer - region.guestAddress) / sizeof(std::uint32_t)), nested[base].second);
        }
        if (!split) {
            variant.runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            continue;
        }
        std::size_t start = 0;
        auto current = count != 0 ? key(region.guestAddress) : -1;
        for (std::size_t word = 1; word <= count; ++word) {
            const auto next = word < count ? key(region.guestAddress + word * sizeof(std::uint32_t)) : -2;
            if (next == current) continue;
            variant.runs.emplace_back(region.guestAddress + start * sizeof(std::uint32_t), region.guestAddress + word * sizeof(std::uint32_t));
            variant.runBases.push_back(current >= 0 && current < 64 ? static_cast<std::int8_t>(current) : std::int8_t {-1});
            variant.runNested.push_back(current >= 64 ? static_cast<std::int16_t>(current - 64) : std::int16_t {-1});
            start = word;
            current = next;
        }
    }
    if (variant.nestedBases.size() != nested.size()) {
        variant.nestedBases.clear();
        variant.runNested.clear();
    }
}

std::shared_ptr<DispatchVariant> Driver::followNestedBases(const DispatchVariant& variant, bool& refused) {
    refused = false;
    if (variant.nestedBases.empty() || variant.runNested.size() != variant.runs.size()) return nullptr;
    std::shared_ptr<DispatchVariant> copy;
    std::shared_ptr<ShaderRecompiler::RecompileResult> rebased;
    for (std::size_t base = 0; base < variant.nestedBases.size(); ++base) {
        const auto [position, slot] = variant.nestedBases[base];
        std::uint64_t address = 0;
        std::uint32_t first = 0;
        bool located = false;
        for (const auto& [begin, end] : variant.runs) {
            const auto count = static_cast<std::uint32_t>((end - begin) / sizeof(std::uint32_t));
            if (position >= first && position < first + count) {
                address = begin + static_cast<std::uint64_t>(position - first) * sizeof(std::uint32_t);
                located = true;
                break;
            }
            first += count;
        }
        if (!located || position >= variant.words.size()) continue;
        std::uint32_t live = 0;
        if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(&live, 1))) != GuestMemory::Compare::Equal) continue;
        const auto stored = variant.words[position];
        if (live == stored) continue;
        if (copy == nullptr) {
            copy = cloneVariant(variant);
            rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
        }
        const auto delta = static_cast<std::int64_t>(live & ~3u) - static_cast<std::int64_t>(stored & ~3u);
        for (std::size_t run = 0; run < copy->runs.size(); ++run) {
            if (copy->runNested[run] != static_cast<std::int16_t>(base)) continue;
            copy->runs[run].first = static_cast<std::uint64_t>(static_cast<std::int64_t>(copy->runs[run].first) + delta);
            copy->runs[run].second = static_cast<std::uint64_t>(static_cast<std::int64_t>(copy->runs[run].second) + delta);
        }
        copy->words[position] = live;
        for (auto& binding : rebased->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::FlattenedSrt || slot >= binding.guestDescriptor.size()) continue;
            if (binding.guestDescriptor[slot] != stored) {
                refused = true;
                return nullptr;
            }
            binding.guestDescriptor[slot] = live;
        }
    }
    if (copy == nullptr) return nullptr;
    copy->compiled = std::move(rebased);
    copy->dataPatched = true;
    return copy;
}

void Driver::learnPendingRelocation() {
    auto pending = std::move(drawRelocationPending);
    drawRelocationPending = {};
    if (pending.key == 0) return;
    std::shared_ptr<const DrawDecode> decode;
    std::vector<std::shared_ptr<DispatchVariant>> fronts;
    if (pending.decode != nullptr) {
        decode = pending.decode;
        fronts = std::move(pending.stages);
    } else {
        const auto inserted = drawCache.find(pending.key);
        if (inserted == drawCache.end() || inserted->second->decode == nullptr) return;
        decode = inserted->second->decode;
        for (const auto& variants : inserted->second->stages) fronts.push_back(variants.empty() ? nullptr : variants.front());
    }
    const auto& programs = decode->programs;
    if (fronts.size() != programs.size()) return;
    const auto known = drawRelocations.find(pending.identity);
    static const DrawRelocation none;
    const DrawRelocation* view = known != drawRelocations.end() ? known->second.get() : &none;
    std::shared_ptr<DrawRelocation> owned;
    const auto writable = [&]() -> DrawRelocation& {
        if (owned == nullptr) {
            owned = std::make_shared<DrawRelocation>(*view);
            view = owned.get();
        }
        return *owned;
    };
    if (known == drawRelocations.end()) writable();
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (fronts[i] == nullptr) continue;
        const auto& variant = *fronts[i];
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size() && index < 64; ++index) {
            const auto reg = program.userDataBase + index - program.hiddenUserWords;
            const bool readHere = (variant.userDataReads >> index & 1u) != 0;
            const bool knownRead = std::find(view->read.begin(), view->read.end(), reg) != view->read.end();
            const bool knownUnread = std::find(view->unread.begin(), view->unread.end(), reg) != view->unread.end();
            if (readHere && !knownRead) {
                auto& shape = writable();
                shape.read.push_back(reg);
                if (const auto unread = std::find(shape.unread.begin(), shape.unread.end(), reg); unread != shape.unread.end()) shape.unread.erase(unread);
            } else if (!readHere && !knownRead && !knownUnread) {
                writable().unread.push_back(reg);
            }
            const auto heldAt = std::find_if(view->words.begin(), view->words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; });
            const bool held = heldAt != view->words.end();
            const auto heldIndex = static_cast<std::size_t>(heldAt - view->words.begin());
            if ((variant.relocatableWords >> index & 1u) == 0) {
                if (!held) continue;
                auto& shape = writable();
                shape.words.erase(shape.words.begin() + static_cast<std::ptrdiff_t>(heldIndex));
                shape.banned.push_back(reg);
                continue;
            }
            if (std::find(view->banned.begin(), view->banned.end(), reg) != view->banned.end()) continue;
            if (held && variant.dataPositions.empty() && variant.nestedBases.empty()) continue;
            RelocatableUserWord word{reg, {}};
            const auto base = AlignedWord(program.userData[index]);
            std::uint32_t position = 0;
            for (std::size_t run = 0; run < variant.runs.size(); ++run) {
                const auto count = static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
                if (run < variant.runBases.size() && variant.runBases[run] == static_cast<std::int8_t>(index)) {
                    word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), count);
                    for (const auto data : variant.dataPositions) {
                        if (data >= position && data < position + count) word.dataOffsets.push_back(static_cast<std::int64_t>(variant.runs[run].first - base) + static_cast<std::int64_t>(data - position) * 4);
                    }
                    for (const auto& nested : variant.nestedBases) {
                        if (nested.first >= position && nested.first < position + count) word.dataOffsets.push_back(static_cast<std::int64_t>(variant.runs[run].first - base) + static_cast<std::int64_t>(nested.first - position) * 4);
                    }
                }
                position += count;
            }
            if (held) {
                const auto& offsets = view->words[heldIndex].dataOffsets;
                const bool fresh = std::any_of(word.dataOffsets.begin(), word.dataOffsets.end(), [&](std::int64_t offset) { return std::find(offsets.begin(), offsets.end(), offset) == offsets.end(); });
                if (!fresh) continue;
                auto& merged = writable().words[heldIndex].dataOffsets;
                for (const auto offset : word.dataOffsets) {
                    if (std::find(merged.begin(), merged.end(), offset) == merged.end()) merged.push_back(offset);
                }
                std::sort(merged.begin(), merged.end());
                continue;
            }
            writable().words.push_back(std::move(word));
        }
    }
    if (owned != nullptr) {
        std::sort(owned->words.begin(), owned->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
        drawRelocations[pending.identity] = std::move(owned);
    }
    if (pending.aliases.structure == 0) return;
    DrawDecodeDonor donor{decode, std::vector<std::uint64_t>(programs.size(), 0)};
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (fronts[i] == nullptr || fronts[i]->prepared == nullptr) continue;
        const auto& variant = fronts[i];
        const auto& program = programs[i];
        std::uint64_t key = 0xcbf29ce484222325ull;
        const auto mix = [&](std::uint64_t value) {
            key ^= value;
            key *= 0x100000001b3ull;
        };
        mix(reinterpret_cast<std::uintptr_t>(variant->prepared.get()));
        mix(variant->pushOffset);
        mix(program.userData.size());
        for (std::uint32_t index = 0; index < program.userData.size(); ++index) {
            const bool relocatable = pending.relocation != nullptr && index >= program.hiddenUserWords && std::any_of(pending.relocation->words.begin(), pending.relocation->words.end(), [&](const RelocatableUserWord& word) { return word.reg == program.userDataBase + index - program.hiddenUserWords; });
            const bool unread = index < 64 && (variant->userDataReads >> index & 1u) == 0;
            if (!relocatable && !unread && index >= program.hiddenUserWords) mix(program.userData[index]);
        }
        donor.stageKeys[i] = key == 0 ? 1 : key;
        std::uint64_t evicted = 0;
        bool added = false;
        putStageDonor(pending.aliases.Key(donor.stageKeys[i], program.userDataBase), variant, program.userData, &evicted, &added, true);
        if (drawMissDiagnosisEnabled()) {
            auto& diagnosis = drawMissDiagnosis;
            ++diagnosis.donorPuts;
            diagnosis.donorBytes += variantBytes(*variant) + program.userData.size() * sizeof(std::uint32_t);
            if (added) ++diagnosis.donorNew;
            if (evicted != 0) {
                ++diagnosis.donorEvictions;
                diagnosis.evictedDonors.Put(evicted, true);
            }
        }
        if (drawMissDiagnosisEnabled()) drawMissDiagnosis.samples.Put(DrawMissDiagnosis::SampleKey(pending.aliases.structure, i), {pending.aliases.raw[DrawAliasKeys::Stage(program.userDataBase)], variant});
    }
    if (drawMissDiagnosisEnabled()) drawMissDiagnosis.learned[pending.identity] = pending.aliases.parts;
    const auto* held = drawDecodes.Find(pending.aliases.structure);
    if (held != nullptr && held->stageKeys.size() == donor.stageKeys.size()) {
        for (std::size_t i = 0; i < donor.stageKeys.size(); ++i) {
            if (donor.stageKeys[i] == 0) donor.stageKeys[i] = held->stageKeys[i];
        }
    }
    drawDecodes.Put(pending.aliases.structure, std::move(donor));
}

void Driver::relocateDrawEntry(const QueueState& queue, const DrawAliasKeys& aliases, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches) {
    auto& counters = drawRelocationCounters;
    ++counters.lookups;
    const auto* known = drawDecodes.Find(aliases.structure);
    if (known == nullptr || known->decode == nullptr) {
        ++counters.unknownStructure;
        if (drawMissDiagnosisEnabled()) diagnoseStructureMiss(aliases);
        return;
    }
    if (drawMissDiagnosisEnabled()) noteDrawKeyMiss(DrawKeyMiss::Relocated);
    const auto shared = known->decode;
    const auto& stageKeys = known->stageKeys;
    const auto& programs = shared->programs;
    auto relocated = std::make_shared<DrawEntry>();
    relocated->decode = shared;
    relocated->stages.resize(programs.size());
    std::vector<DrawUserPatch> current;
    bool complete = true;
    drawStageDonorKeys.assign(programs.size(), 0);
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        if (i < shared->roles.size() && shared->roles[i] == ShaderRecompiler::ProgramRole::GeometryBack) continue;
        const auto donorKey = i < stageKeys.size() && stageKeys[i] != 0 ? aliases.Key(stageKeys[i], program.userDataBase) : 0;
        const auto* found = donorKey != 0 ? drawStageDonors.Find(donorKey) : nullptr;
        if (drawMissDiagnosisEnabled() && donorKey != 0) {
            ++drawMissDiagnosis.donorLookups;
            if (found != nullptr) ++drawMissDiagnosis.donorFound;
        }
        if (found != nullptr) drawStageDonorKeys[i] = donorKey;
        std::vector<std::uint32_t> now(program.userData.size(), 0);
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            now[index] = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
            if (now[index] != program.userData[index]) current.push_back({i, index, now[index]});
        }
        struct Move {
            std::uint32_t index, from, to;
        };
        bool usable = false;
        const std::span<const DrawStageOption> options = found != nullptr ? std::span<const DrawStageOption>(found->options) : std::span<const DrawStageOption>();
        for (const auto& option : options) {
            if (option.variant == nullptr || option.variant->compiled == nullptr || option.userData.size() != program.userData.size()) continue;
            usable = true;
            if (drawMissDiagnosisEnabled()) ++drawMissDiagnosis.donorOptionsTried;
            std::vector<Move> moves;
            bool allowed = true;
            for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
                if (now[index] == option.userData[index]) continue;
                if (index < 64 && (option.variant->userDataReads >> index & 1u) == 0) {
                    ++counters.unreadMoves;
                    continue;
                }
                const auto from = option.userData[index];
                if (index >= 64 || (option.variant->relocatableWords >> index & 1u) == 0 || from == 0 || now[index] == 0 || ((from ^ now[index]) & 3u) != 0) allowed = false;
                moves.push_back({index, from, now[index]});
            }
            if (!allowed) continue;
            if (moves.empty()) {
                relocated->stages[i].push_back(option.variant);
                continue;
            }
            const auto& variant = option.variant;
            auto copy = cloneVariant(*variant);
            for (const auto& move : moves) {
                const auto delta = AlignedWord(move.to) - AlignedWord(move.from);
                for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
                    if (copy->runBases[run] != static_cast<std::int8_t>(move.index)) continue;
                    copy->runs[run].first += delta;
                    copy->runs[run].second += delta;
                }
                const auto& forwarded = variant->compiled->shaderDataUserWords;
                for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
                    if (forwarded[slot] == move.index) copy->shaderDataPatches.emplace_back(slot, move.to);
                }
            }
            relocated->stages[i].push_back(std::move(copy));
        }
        if (relocated->stages[i].empty()) {
            if (usable) ++counters.refused;
            else ++counters.missingStage;
            if (drawMissDiagnosisEnabled()) diagnoseStageMiss(aliases, i, program.userDataBase, donorKey, usable);
            complete = false;
        }
    }
    if (complete) ++counters.donors;
    patches = std::move(current);
    entry = std::move(relocated);
    decode = shared;
}

std::uint64_t DrawStageContent(const DispatchVariant& variant) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    std::size_t data = 0;
    for (std::size_t position = 0; position < variant.words.size(); ++position) {
        while (data < variant.dataPositions.size() && variant.dataPositions[data] < position) ++data;
        const bool masked = (data < variant.dataPositions.size() && variant.dataPositions[data] == position) || std::any_of(variant.nestedBases.begin(), variant.nestedBases.end(), [&](const std::pair<std::uint32_t, std::uint32_t>& nested) { return nested.first == position; });
        hash = (hash ^ (masked ? 0u : variant.words[position])) * 0x100000001b3ull;
    }
    hash = (hash ^ reinterpret_cast<std::uintptr_t>(variant.prepared.get())) * 0x100000001b3ull;
    return hash == 0 ? 1 : hash;
}

std::uint64_t DrawStageDonor::Bytes() const {
    std::uint64_t bytes = sizeof(DrawStageDonor);
    for (const auto& option : options) {
        bytes += sizeof(DrawStageOption) + option.userData.size() * sizeof(std::uint32_t);
        if (option.variant != nullptr) bytes += sizeof(DispatchVariant) + option.variant->words.size() * sizeof(std::uint32_t) + option.variant->runs.size() * sizeof(option.variant->runs[0]) + option.variant->dataPositions.size() * sizeof(std::uint32_t) * 2;
    }
    return bytes;
}

void Driver::putStageDonor(std::uint64_t key, const std::shared_ptr<DispatchVariant>& variant, const std::vector<std::uint32_t>& userData, std::uint64_t* evicted, bool* added, bool count) {
    const auto content = DrawStageContent(*variant);
    auto* held = drawStageDonors.Find(key);
    if (held == nullptr) {
        DrawStageDonor donor;
        donor.options.push_back({variant, userData, content});
        donor.recent[0] = content;
        donor.next = 1;
        const bool inserted = drawStageDonors.Put(key, std::move(donor), evicted);
        if (added != nullptr) *added = inserted;
        return;
    }
    if (added != nullptr) *added = false;
    auto& options = held->options;
    const auto same = std::find_if(options.begin(), options.end(), [&](const DrawStageOption& option) { return option.content == content; });
    const bool known = same != options.end();
    const bool recurring = std::find(held->recent.begin(), held->recent.end(), content) != held->recent.end();
    if (count && drawMissDiagnosisEnabled()) {
        auto& diagnosis = drawMissDiagnosis;
        if (known) ++diagnosis.donorSame;
        else if (recurring) ++diagnosis.donorRecurring;
        else ++diagnosis.donorFresh;
    }
    if (known) options.erase(same);
    else if (recurring) held->cycling = true;
    if (!known && !held->cycling) options.clear();
    options.insert(options.begin(), {variant, userData, content});
    if (options.size() > DrawStageDonor::Options) options.resize(DrawStageDonor::Options);
    if (!recurring) {
        held->recent[held->next] = content;
        held->next = (held->next + 1) % DrawStageDonor::Recent;
    }
}

std::shared_ptr<DispatchVariant> Driver::cloneVariant(const DispatchVariant& variant) {
    auto copy = std::make_shared<DispatchVariant>();
    copy->runs = variant.runs;
    copy->runBases = variant.runBases;
    copy->relocatableWords = variant.relocatableWords;
    copy->userDataReads = variant.userDataReads;
    copy->words = variant.words;
    copy->forgetSerial = variant.forgetSerial;
    copy->compiled = variant.compiled;
    copy->shader = variant.shader;
    copy->dataPositions = variant.dataPositions;
    copy->dataSlots = variant.dataSlots;
    copy->flatBinding = variant.flatBinding;
    copy->pushOffset = variant.pushOffset;
    copy->prepared = variant.prepared;
    copy->bufferWordBuffers = variant.bufferWordBuffers;
    copy->patchedPushConstants = variant.patchedPushConstants;
    copy->shaderDataPatches = variant.shaderDataPatches;
    copy->vertexInfo = variant.vertexInfo;
    copy->inputExports = variant.inputExports;
    copy->runNested = variant.runNested;
    copy->nestedBases = variant.nestedBases;
    return copy;
}

void Driver::tagBufferWords(DispatchVariant& variant, std::span<const ShaderRecompiler::BufferBaseWord> bufferBaseWords) {
    if (!drawRelocationEnabled() || bufferBaseWords.empty() || variant.compiled == nullptr || !variant.dataPositions.empty()) return;
    const auto& compiled = *variant.compiled;
    using Patches = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
    std::vector<std::pair<std::uint32_t, Patches>> tagged;
    for (const auto& base : bufferBaseWords) {
        std::uint32_t position = 0;
        bool located = false;
        for (const auto& [begin, end] : variant.runs) {
            if (base.address >= begin && base.address < end && (base.address - begin) % sizeof(std::uint32_t) == 0) {
                position += static_cast<std::uint32_t>((base.address - begin) / sizeof(std::uint32_t));
                located = true;
                break;
            }
            position += static_cast<std::uint32_t>((end - begin) / sizeof(std::uint32_t));
        }
        if (!located || position >= variant.words.size() || variant.words[position] == 0) continue;
        const auto value = variant.words[position];
        Patches patches;
        std::size_t slots = 0, others = 0;
        for (std::size_t binding = 0; binding < compiled.bindings.size(); ++binding) {
            const auto& words = compiled.bindings[binding].guestDescriptor;
            for (std::size_t word = 0; word < words.size(); ++word) {
                if (words[word] != value) continue;
                const bool slot = std::any_of(compiled.bufferBaseSlots.begin(), compiled.bufferBaseSlots.end(), [&](const std::array<std::uint32_t, 3>& entry) { return entry[0] == base.buffer && entry[1] == binding && entry[2] == word; });
                const bool flat = compiled.bindings[binding].role == ShaderRecompiler::DescriptorRole::FlattenedSrt && base.flatOffset >= 0 && static_cast<std::int64_t>(word) == base.flatOffset;
                if (slot) ++slots;
                if (!slot && !flat) ++others;
                if (slot || flat) patches.emplace_back(static_cast<std::uint32_t>(binding), static_cast<std::uint32_t>(word));
            }
        }
        for (std::size_t at = 0; at + sizeof(std::uint32_t) <= compiled.pushConstants.size(); at += sizeof(std::uint32_t)) {
            std::uint32_t word = 0;
            std::memcpy(&word, compiled.pushConstants.data() + at, sizeof(word));
            if (word == value) ++others;
        }
        for (const auto& constant : compiled.specialization) {
            if (constant.value == value) ++others;
        }
        const auto expected = static_cast<std::size_t>(std::count_if(compiled.bufferBaseSlots.begin(), compiled.bufferBaseSlots.end(), [&](const std::array<std::uint32_t, 3>& entry) { return entry[0] == base.buffer; }));
        if (slots == 0 || slots != expected || others != 0) continue;
        if (std::any_of(tagged.begin(), tagged.end(), [&](const auto& entry) { return entry.first == position; })) continue;
        tagged.emplace_back(position, std::move(patches));
    }
    std::sort(tagged.begin(), tagged.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [position, patches] : tagged) {
        variant.dataPositions.push_back(position);
        variant.bufferWordBuffers.push_back(std::move(patches));
        variant.dataSlots.push_back(DispatchVariant::NoFlatBinding);
    }
}

DataWordPositionCounts Driver::tagDataLeaves(DispatchVariant& variant, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads) {
    if (!drawRelocationEnabled() || !dataHits() || leaves.empty() || variant.compiled == nullptr || variant.dataPositions.size() != variant.dataSlots.size()) return {};
    const auto& bindings = variant.compiled->bindings;
    const auto flat = std::find_if(bindings.begin(), bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::FlattenedSrt; });
    if (flat == bindings.end()) return {};
    std::vector<std::uint32_t> positions, slots;
    const auto counts = DataWordPositions(variant.runs, leaves, otherReads, variant.words, flat->guestDescriptor, positions, slots);
    struct Tagged {
        std::uint32_t position, slot;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> patches;
    };
    std::vector<Tagged> merged;
    for (std::size_t k = 0; k < variant.dataPositions.size(); ++k) merged.push_back({variant.dataPositions[k], variant.dataSlots[k], variant.bufferWordBuffers[k]});
    const auto buffers = merged.size();
    for (std::size_t k = 0; k < positions.size(); ++k) {
        if (std::any_of(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(buffers), [&](const Tagged& entry) { return entry.position == positions[k]; })) continue;
        merged.push_back({positions[k], slots[k], {}});
    }
    if (merged.size() == buffers) return counts;
    std::stable_sort(merged.begin(), merged.end(), [](const Tagged& a, const Tagged& b) { return a.position < b.position; });
    variant.dataPositions.clear();
    variant.dataSlots.clear();
    variant.bufferWordBuffers.clear();
    for (auto& entry : merged) {
        variant.dataPositions.push_back(entry.position);
        variant.dataSlots.push_back(entry.slot);
        variant.bufferWordBuffers.push_back(std::move(entry.patches));
    }
    variant.flatBinding = static_cast<std::uint32_t>(flat - bindings.begin());
    return counts;
}

std::shared_ptr<DispatchVariant> Driver::patchBufferWords(const DispatchVariant& variant, std::span<const std::pair<std::uint32_t, std::uint32_t>> live) {
    if (live.empty() || variant.compiled == nullptr || variant.dataPositions.size() != variant.bufferWordBuffers.size() || variant.dataPositions.size() != variant.dataSlots.size()) return nullptr;
    auto copy = cloneVariant(variant);
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [position, fresh] : live) {
        const auto found = std::lower_bound(variant.dataPositions.begin(), variant.dataPositions.end(), position);
        if (found == variant.dataPositions.end() || *found != position || position >= copy->words.size()) return nullptr;
        const auto stored = copy->words[position];
        const auto k = static_cast<std::size_t>(found - variant.dataPositions.begin());
        if (variant.dataSlots[k] != DispatchVariant::NoFlatBinding) {
            if (variant.flatBinding >= rebased->bindings.size()) return nullptr;
            auto& descriptor = rebased->bindings[variant.flatBinding].guestDescriptor;
            for (auto at = k; at < variant.dataPositions.size() && variant.dataPositions[at] == position; ++at) {
                const auto slot = variant.dataSlots[at];
                if (slot == DispatchVariant::NoFlatBinding || slot >= descriptor.size() || descriptor[slot] != stored) return nullptr;
                descriptor[slot] = fresh;
            }
            copy->words[position] = fresh;
            continue;
        }
        if (stored == 0 || fresh == 0 || ((stored ^ fresh) & 3u) != 0) return nullptr;
        for (const auto& [binding, word] : variant.bufferWordBuffers[k]) {
            if (binding >= rebased->bindings.size() || word >= rebased->bindings[binding].guestDescriptor.size() || rebased->bindings[binding].guestDescriptor[word] != stored) return nullptr;
            rebased->bindings[binding].guestDescriptor[word] = fresh;
        }
        copy->words[position] = fresh;
    }
    copy->compiled = std::move(rebased);
    copy->dataPatched = true;
    return copy;
}

void Driver::applyShaderDataPatches(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty()) return;
    const auto patch = [&](std::vector<std::byte>& push) {
        for (const auto& [slot, value] : variant.shaderDataPatches) {
            const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
            if (at + sizeof(std::uint32_t) <= push.size()) std::memcpy(push.data() + at, &value, sizeof(value));
        }
    };
    const bool stored = std::any_of(variant.compiled->bindings.begin(), variant.compiled->bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
    if (!stored) {
        if (variant.patchedPushConstants.empty()) variant.patchedPushConstants = variant.compiled->pushConstants;
        patch(variant.patchedPushConstants);
        variant.shaderDataPatches.clear();
        return;
    }
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    patch(rebased->pushConstants);
    for (const auto& [slot, value] : variant.shaderDataPatches) {
        for (auto& binding : rebased->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
        }
    }
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
}

void Driver::reportDrawRelocation() {
    auto& counters = drawRelocationCounters;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-relocate] %llu alias lookups: %llu donors with moved user words, %llu refusals, %llu hits through a relocated entry; misses: %llu unknown structure, %llu stages without a donor; %llu unread user words ignored; %zu shader sets learned, %zu stage donors held\n", static_cast<unsigned long long>(counters.lookups), static_cast<unsigned long long>(counters.donors), static_cast<unsigned long long>(counters.refused), static_cast<unsigned long long>(counters.hits), static_cast<unsigned long long>(counters.unknownStructure), static_cast<unsigned long long>(counters.missingStage), static_cast<unsigned long long>(counters.unreadMoves), drawRelocations.size(), drawStageDonors.Size());
    counters = {};
}

}

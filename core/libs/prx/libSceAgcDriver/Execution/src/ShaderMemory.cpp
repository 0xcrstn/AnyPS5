#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AgcDriver {
namespace {

struct CaptureProfile {
    std::atomic<std::uint64_t> captures{0};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> pages{0};
    // The whole CaptureResources call: the plan lookup and the materialization are one call.
    std::atomic<std::uint64_t> captureNanoseconds{0};
    // Pages read word by word because recorded GPU work writes them, and the word reads of those
    // pages that waited (a read taking longer than WordWaitNanoseconds went through a hook sync),
    // were read raw on the driver's evidence, or were read both ways (verify) and differed.
    std::atomic<std::uint64_t> pagesWordwise{0};
    std::atomic<std::uint64_t> wordWaits{0};
    std::atomic<std::uint64_t> wordWaitNanoseconds{0};
    std::atomic<std::uint64_t> wordsRaw{0};
    std::atomic<std::uint64_t> wordsVerified{0};
    std::atomic<std::uint64_t> wordMismatches{0};
    // Words served from the driver's known values (never profile-gated: the [copy] line reads them).
    std::atomic<std::uint64_t> wordsKnown{0};
    std::atomic<std::uint64_t> wordsKnownVerified{0};
    std::atomic<std::uint64_t> wordsKnownMismatches{0};
    // Hook reads not reported to the observer because no GPU wait happened across them.
    std::atomic<std::uint64_t> observationsSkipped{0};
    // Sub-phases of a capture: the source lookup (the stage inputs, the recompiler's key over the
    // code and the plan, timed by CaptureResources itself: ResourceCapture::sourceNanoseconds),
    // whole-page fetches, per-word fetches (their hook waits included) and the GPU waits the flush
    // hook made anywhere inside the capture (Recorder::ThreadWaitedMs). The walk itself is what
    // remains of the capture time after the lookup, the fetches and the specialization
    // (ResourceMaterializer).
    std::atomic<std::uint64_t> resolveNanoseconds{0};
    std::atomic<std::uint64_t> pageReadNanoseconds{0};
    std::atomic<std::uint64_t> wordReadNanoseconds{0};
    std::atomic<std::uint64_t> hookWaitNanoseconds{0};
    // The driver's per-shader source handle memo: captures served from it, and resolves it made.
    std::atomic<std::uint64_t> handleHits{0};
    std::atomic<std::uint64_t> handleMisses{0};
};

struct MemoProfile {
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> patched{0};
    std::atomic<std::uint64_t> relocated{0};
    std::atomic<std::uint64_t> dataPatched{0};
    std::atomic<std::uint64_t> descriptorWords{0};
    std::atomic<std::uint64_t> nestedBases{0};
    std::array<std::atomic<std::uint64_t>, 8> roles{};
    std::atomic<std::uint64_t> warmWalks{0};
    std::atomic<std::uint64_t> warmNanoseconds{0};
    std::atomic<std::uint64_t> relocationRefused{0};
    std::atomic<std::uint64_t> absent{0};
    std::atomic<std::uint64_t> valueWords{0};
    std::atomic<std::uint64_t> pointerWords{0};
    std::atomic<std::uint64_t> memory{0};
    std::atomic<std::uint64_t> replayed{0};
    std::atomic<std::uint64_t> hitNanoseconds{0};
    std::atomic<std::uint64_t> missNanoseconds{0};
    std::atomic<std::uint64_t> evaluationNanoseconds{0};
    std::atomic<std::uint64_t> pageSetupNanoseconds{0};
    std::atomic<std::uint64_t> pageReadNanoseconds{0};
    std::atomic<std::uint64_t> missReads{0};
    std::atomic<std::uint64_t> missPages{0};
    std::atomic<std::uint64_t> values{0};
    std::atomic<std::uint64_t> laneEvaluators{0};
    std::atomic<std::uint64_t> phis{0};
    std::mutex reportMutex;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

MemoProfile& MemoTotals() {
    static MemoProfile profile;
    return profile;
}

std::atomic<std::uint64_t> memoizedCaptures{0};

bool CaptureMemoEnabled() {
    static const bool enabled = std::getenv("APS5_NO_CAPTURE_MEMO") == nullptr;
    return enabled;
}

constexpr std::size_t MemoWays = 4;
constexpr std::size_t MemoShaders = 1024;

struct MemoEntry {
    std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
    std::uint64_t shaderBase = 0;
    std::vector<std::uint32_t> userData;
    std::vector<ShaderMemory::RecordedAccess> accesses;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    std::vector<std::pair<std::uint64_t, std::int32_t>> origins;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> dataLeaves;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> baseWords;
    std::vector<std::uint64_t> accessBases;
    bool dataLeavesBuilt = false;
};

void ClearDerived(MemoEntry& entry) {
    entry.origins.clear();
    entry.dataLeaves.clear();
    entry.baseWords.clear();
    entry.accessBases.clear();
    entry.dataLeavesBuilt = false;
}

enum ReadRole : std::uint8_t { RoleDescriptor = 1, RoleCondition = 2, RoleUniform = 4, RoleAddress = 8, RoleFlat = 16, RoleBaseOnly = 32 };

bool RawRead(const ShaderRecompiler::IrResourcePlan& plan, ShaderRecompiler::IrValue& value) {
    const auto opcode = value.Opcode();
    if (opcode != ShaderRecompiler::IrOpcode::LoadAddressU32 && opcode != ShaderRecompiler::IrOpcode::ReadConstBuffer) return false;
    const auto index = value.Flags<ShaderRecompiler::MemoryFlags>().index;
    if (index >= plan.memoryInfo.size()) return false;
    const auto kind = plan.memoryInfo[index].kind;
    return (opcode == ShaderRecompiler::IrOpcode::LoadAddressU32 && kind == ShaderRecompiler::ResourceKind::ScalarAddress) || (opcode == ShaderRecompiler::IrOpcode::ReadConstBuffer && kind == ShaderRecompiler::ResourceKind::ScalarBuffer);
}

std::unordered_map<const void*, std::uint8_t> ComputeReadRoles(const ShaderRecompiler::IrResourcePlan& plan) {
    std::unordered_map<const void*, std::uint8_t> roles;
    std::vector<ShaderRecompiler::IrValue*> raws;
    std::unordered_set<const void*> rawSeen;
    const auto mark = [&](ShaderRecompiler::IrValue* root, std::uint8_t role) {
        if (root == nullptr) return;
        std::vector<ShaderRecompiler::IrValue*> pending{root};
        std::unordered_set<const void*> visited;
        while (!pending.empty()) {
            auto* value = pending.back()->Resolve();
            pending.pop_back();
            if (!visited.insert(value).second) continue;
            if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
                const auto* slot = value->Argument(1)->Resolve();
                if (slot->HasImmediate() && slot->Type() == ShaderRecompiler::IrType::U32 && slot->ImmediateU32() < plan.srtReads.size() && plan.srtReads[slot->ImmediateU32()].value != nullptr) pending.push_back(plan.srtReads[slot->ImmediateU32()].value);
                continue;
            }
            if (RawRead(plan, *value)) {
                roles[value] |= role;
                if (rawSeen.insert(value).second) raws.push_back(value);
                continue;
            }
            for (std::size_t i = 0; i < value->ArgumentCount(); ++i) {
                if (value->Argument(i) != nullptr) pending.push_back(value->Argument(i));
            }
        }
    };
    for (const auto& source : plan.descriptorSources) {
        for (std::uint32_t i = 0; i < source.dwordCount && i < source.dwords.size(); ++i) mark(source.dwords[i], RoleDescriptor);
    }
    for (const auto& block : plan.controlFlow) mark(block.condition, RoleCondition);
    if (plan.uniformFill.fill.kind != ShaderRecompiler::UniformFillKind::None) {
        for (std::uint32_t i = 0; i < plan.uniformFill.fill.words && i < plan.uniformFill.values.size(); ++i) mark(plan.uniformFill.values[i], RoleUniform);
    }
    for (const auto& read : plan.srtReads) mark(read.value, RoleFlat);
    for (std::size_t next = 0; next < raws.size(); ++next) {
        auto* raw = raws[next];
        auto* handle = raw->ArgumentCount() != 0 && raw->Argument(0) != nullptr ? raw->Argument(0)->Resolve() : nullptr;
        if (handle != nullptr) {
            for (std::size_t i = 0; i < handle->ArgumentCount(); ++i) {
                if (handle->Argument(i) == nullptr) continue;
                auto* argument = handle->Argument(i)->Resolve();
                if (argument->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && argument->ArgumentCount() >= 2 && argument->Argument(1) != nullptr) {
                    const auto* slot = argument->Argument(1)->Resolve();
                    if (slot->HasImmediate() && slot->Type() == ShaderRecompiler::IrType::U32 && slot->ImmediateU32() < plan.srtReads.size() && plan.srtReads[slot->ImmediateU32()].value != nullptr) argument = plan.srtReads[slot->ImmediateU32()].value->Resolve();
                }
                if (i == 0 && RawRead(plan, *argument)) {
                    roles[argument] |= RoleBaseOnly;
                    if (rawSeen.insert(argument).second) raws.push_back(argument);
                    continue;
                }
                mark(argument, RoleAddress);
            }
        }
        for (std::size_t i = 1; i < raw->ArgumentCount(); ++i) mark(raw->Argument(i), RoleAddress);
    }
    return roles;
}

std::uint8_t ReadRoles(const ShaderRecompiler::ResourceCapture& capture, std::uint64_t address) {
    struct RoleCache {
        std::unordered_map<const ShaderRecompiler::IrResourcePlan*, std::pair<std::weak_ptr<const ShaderRecompiler::IrResourcePlan>, std::unordered_map<const void*, std::uint8_t>>> plans;
    };
    struct RoleCacheStorage {};
    auto& cache = HostThreadLocal<RoleCache, RoleCacheStorage>();
    auto found = cache.plans.find(capture.plan.get());
    if (found == cache.plans.end() || found->second.first.lock() != capture.plan) {
        if (cache.plans.size() >= 512) cache.plans.clear();
        found = cache.plans.insert_or_assign(capture.plan.get(), std::pair(std::weak_ptr<const ShaderRecompiler::IrResourcePlan>(capture.plan), ComputeReadRoles(*capture.plan))).first;
    }
    const auto& roles = found->second.second;
    const auto& use = capture.userDataUse;
    std::uint8_t result = 0;
    for (std::size_t read = 0; read < use.reads.size() && read < use.readInstructions.size(); ++read) {
        if (use.reads[read].first != address) continue;
        const auto role = roles.find(use.readInstructions[read]);
        result |= role != roles.end() ? role->second : std::uint8_t{0x80};
    }
    return result;
}

bool FeedsDescriptorDirectly(const ShaderRecompiler::ResourceCapture& capture, std::uint64_t address) {
    const auto& use = capture.userDataUse;
    const auto& plan = *capture.plan;
    for (std::size_t read = 0; read < use.reads.size() && read < use.readInstructions.size(); ++read) {
        if (use.reads[read].first != address) continue;
        const auto* instruction = use.readInstructions[read];
        for (const auto& source : plan.descriptorSources) {
            for (std::uint32_t dword = 0; dword < source.dwordCount && dword < source.dwords.size(); ++dword) {
                if (source.dwords[dword] == nullptr) continue;
                ShaderRecompiler::IrValue* value = source.dwords[dword]->Resolve();
                if (value->Opcode() == ShaderRecompiler::IrOpcode::ReadConst && value->ArgumentCount() == 2 && value->Argument(1) != nullptr) {
                    const auto* slot = value->Argument(1)->Resolve();
                    if (slot->HasImmediate() && slot->Type() == ShaderRecompiler::IrType::U32 && slot->ImmediateU32() < plan.srtReads.size() && plan.srtReads[slot->ImmediateU32()].value != nullptr) value = plan.srtReads[slot->ImmediateU32()].value->Resolve();
                }
                if (value == instruction) return true;
            }
        }
    }
    return false;
}

bool UsesImageTables(const ShaderRecompiler::IrResourcePlan& plan) {
    return std::any_of(plan.descriptorSources.begin(), plan.descriptorSources.end(), [](const ShaderRecompiler::DescriptorSource& source) { return source.tableColumn.has_value(); }) ||
        std::any_of(plan.info.images.begin(), plan.info.images.end(), [](const ShaderRecompiler::ImageResource& image) { return image.table != ShaderRecompiler::NoTable; }) ||
        std::any_of(plan.info.samplers.begin(), plan.info.samplers.end(), [](const ShaderRecompiler::SamplerResource& sampler) { return sampler.table != ShaderRecompiler::NoTable; });
}

const std::vector<std::pair<std::uint64_t, std::uint32_t>>& DataLeaves(MemoEntry& entry) {
    if (entry.dataLeavesBuilt) return entry.dataLeaves;
    entry.dataLeavesBuilt = true;
    const auto& trace = entry.capture->readTrace;
    const auto& plan = *entry.capture->plan;
    if (trace.leaves.empty() || UsesImageTables(plan)) return entry.dataLeaves;
    for (const auto& [slot, address] : trace.leaves) {
        if (slot >= plan.pureFlatSlots.size() || plan.pureFlatSlots[slot] == 0u || slot >= entry.capture->snapshot.flattenedSrt.size()) continue;
        if (std::binary_search(trace.otherReads.begin(), trace.otherReads.end(), address)) continue;
        entry.dataLeaves.emplace_back(address, slot);
    }
    std::sort(entry.dataLeaves.begin(), entry.dataLeaves.end());
    const auto& use = entry.capture->userDataUse;
    if (use.readBases.size() != use.reads.size() || use.readInstructions.size() != use.reads.size()) return entry.dataLeaves;
    for (const auto& [flatOffset, address] : entry.capture->baseSlots) {
        if (flatOffset >= entry.capture->snapshot.flattenedSrt.size()) continue;
        const void* instruction = nullptr;
        for (const auto& read : plan.srtReads) {
            if (read.flatOffset == flatOffset && read.value != nullptr) instruction = read.value->Resolve();
        }
        bool only = instruction != nullptr;
        for (std::size_t read = 0; only && read < use.reads.size(); ++read) {
            if (use.reads[read].first == address && use.readInstructions[read] != instruction) only = false;
        }
        if (only) entry.baseWords.emplace_back(address, flatOffset);
    }
    std::sort(entry.baseWords.begin(), entry.baseWords.end());
    if (entry.baseWords.empty()) return entry.dataLeaves;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bases;
    bases.reserve(use.reads.size());
    for (std::size_t read = 0; read < use.reads.size(); ++read) bases.emplace_back(use.reads[read].first, use.readBases[read]);
    std::sort(bases.begin(), bases.end());
    entry.accessBases.reserve(entry.accesses.size());
    for (const auto& access : entry.accesses) {
        auto first = std::lower_bound(bases.begin(), bases.end(), std::pair<std::uint64_t, std::uint64_t>{access.address, 0});
        std::uint64_t base = 0;
        bool seen = false;
        for (; first != bases.end() && first->first == access.address; ++first) {
            if (seen && first->second != base) {
                base = ~std::uint64_t{0};
                break;
            }
            base = first->second;
            seen = true;
        }
        entry.accessBases.push_back(base);
    }
    return entry.dataLeaves;
}

struct MemoShader {
    std::array<MemoEntry, MemoWays> ways;
    std::size_t next = 0;
};

struct MemoTable {
    std::unordered_map<const ShaderRecompiler::SourceHandle*, MemoShader> shaders;
    std::uint32_t depth = 0;
    const void* lastPlan = nullptr;
};

struct MemoDepth {
    explicit MemoDepth(MemoTable& table) : table(table) { ++table.depth; }
    ~MemoDepth() { --table.depth; }
    MemoTable& table;
};

MemoTable& ThreadMemo() {
    struct MemoTableStorage {};
    return HostThreadLocal<MemoTable, MemoTableStorage>();
}

bool AllWordsRead(const ShaderRecompiler::SrtUserDataUse& use) {
    return use.asValue == ~std::uint64_t{0};
}

bool WordRead(const ShaderRecompiler::SrtUserDataUse& use, std::size_t word) {
    return AllWordsRead(use) || (word < ShaderRecompiler::SrtUserDataUse::TrackedWords && (((use.asBase | use.asValue) >> word) & 1u) != 0u);
}

enum class MemoMatch { Other, Exact, UnreadWords, PointerWords, ValueWords, DataWords, NestedBases };

ShaderMemory::ReplayResult& scratchReplay() {
    struct ReplayStorage {};
    return HostThreadLocal<ShaderMemory::ReplayResult, ReplayStorage>();
}

std::vector<std::pair<std::size_t, std::uint32_t>>& scratchChanged() {
    struct ChangedStorage {};
    return HostThreadLocal<std::vector<std::pair<std::size_t, std::uint32_t>>, ChangedStorage>();
}

MemoMatch MatchUserData(const MemoEntry& entry, std::uint64_t shaderBase, std::span<const std::uint32_t> userData) {
    if (entry.capture == nullptr || entry.shaderBase != shaderBase || entry.userData.size() != userData.size()) return MemoMatch::Other;
    const auto& use = entry.capture->userDataUse;
    bool exact = true;
    bool pointers = false;
    for (std::size_t word = 0; word < userData.size(); ++word) {
        if (entry.userData[word] == userData[word]) continue;
        exact = false;
        if (!WordRead(use, word)) continue;
        if (AllWordsRead(use) || ((use.asValue >> word) & 1u) != 0u) return MemoMatch::ValueWords;
        pointers = true;
    }
    if (exact) return MemoMatch::Exact;
    return pointers ? MemoMatch::PointerWords : MemoMatch::UnreadWords;
}

constexpr std::uint64_t GuestAddressMask = 0x0000ffffffffffffull;

struct Relocation {
    std::uint64_t moved = 0;
    std::array<std::int64_t, ShaderRecompiler::SrtUserDataUse::TrackedWords> delta{};
};

bool RelocationFor(const MemoEntry& entry, std::span<const std::uint32_t> userData, Relocation& relocation) {
    const auto& use = entry.capture->userDataUse;
    relocation = {};
    if (UsesImageTables(*entry.capture->plan)) return false;
    for (std::size_t word = 0; word < userData.size(); ++word) {
        if (entry.userData[word] == userData[word] || !WordRead(use, word)) continue;
        if (AllWordsRead(use) || word >= ShaderRecompiler::SrtUserDataUse::TrackedWords || ((use.asValue >> word) & 1u) != 0u || ((use.asBase >> word) & 1u) == 0u) return false;
        if (entry.userData[word] == 0u || userData[word] == 0u) return false;
        relocation.moved |= std::uint64_t{1} << word;
        relocation.delta[word] = static_cast<std::int64_t>(userData[word] & ~3u) - static_cast<std::int64_t>(entry.userData[word] & ~3u);
    }
    return relocation.moved != 0;
}

bool Relocated(std::uint64_t address, std::int32_t origin, const Relocation& relocation, std::uint64_t& result) {
    if (origin < 0 || static_cast<std::uint32_t>(origin) >= ShaderRecompiler::SrtUserDataUse::TrackedWords || ((relocation.moved >> origin) & 1u) == 0u) {
        result = address;
        return true;
    }
    const auto delta = relocation.delta[static_cast<std::size_t>(origin)];
    if (delta < 0 ? address < static_cast<std::uint64_t>(-delta) : address > GuestAddressMask - static_cast<std::uint64_t>(delta)) return false;
    result = address + static_cast<std::uint64_t>(delta);
    return true;
}

const std::vector<std::pair<std::uint64_t, std::int32_t>>& Origins(MemoEntry& entry) {
    if (entry.origins.empty() && !entry.capture->userDataUse.reads.empty()) {
        entry.origins.assign(entry.capture->userDataUse.reads.begin(), entry.capture->userDataUse.reads.end());
        std::sort(entry.origins.begin(), entry.origins.end());
        entry.origins.erase(std::unique(entry.origins.begin(), entry.origins.end()), entry.origins.end());
    }
    return entry.origins;
}

bool RelocatedAddress(std::span<const std::pair<std::uint64_t, std::int32_t>> origins, std::uint64_t address, const Relocation& relocation, std::uint64_t& result) {
    const auto first = std::lower_bound(origins.begin(), origins.end(), address, [](const std::pair<std::uint64_t, std::int32_t>& entry, std::uint64_t value) { return entry.first < value; });
    if (first == origins.end() || first->first != address) {
        result = address;
        return true;
    }
    bool found = false;
    for (auto it = first; it != origins.end() && it->first == address; ++it) {
        std::uint64_t moved = 0;
        if (!Relocated(address, it->second, relocation, moved) || (found && moved != result)) return false;
        result = moved;
        found = true;
    }
    return true;
}

void RewriteGuardTail(ShaderRecompiler::ResourceCapture& capture) {
    auto& flattened = capture.snapshot.flattenedSrt;
    const auto& poison = capture.snapshot.srtPoison;
    if (poison.empty() || flattened.size() < poison.size() * 3u) return;
    const auto first = flattened.size() - poison.size() * 3u;
    for (std::size_t record = 0; record < poison.size(); ++record) {
        flattened[first + record * 3u + 1u] = static_cast<std::uint32_t>(poison[record].address);
        flattened[first + record * 3u + 2u] = static_cast<std::uint32_t>(poison[record].address >> 32u);
    }
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> RelocatedCapture(const MemoEntry& entry, std::span<const std::uint32_t> userData, const Relocation& relocation) {
    auto capture = std::make_shared<ShaderRecompiler::ResourceCapture>(*entry.capture);
    std::copy_n(userData.begin(), capture->snapshot.userData.size(), capture->snapshot.userData.begin());
    const auto origins = std::span(entry.origins);
    const auto move = [&](std::uint64_t& address) {
        std::uint64_t result = 0;
        if (!RelocatedAddress(origins, address, relocation, result)) return false;
        address = result;
        return true;
    };
    for (auto& [address, origin] : capture->userDataUse.reads) {
        if (!Relocated(address, origin, relocation, address)) return nullptr;
    }
    for (auto& [slot, address] : capture->readTrace.leaves) {
        if (!move(address)) return nullptr;
    }
    for (auto& address : capture->readTrace.otherReads) {
        if (!move(address)) return nullptr;
    }
    std::sort(capture->readTrace.otherReads.begin(), capture->readTrace.otherReads.end());
    capture->readTrace.otherReads.erase(std::unique(capture->readTrace.otherReads.begin(), capture->readTrace.otherReads.end()), capture->readTrace.otherReads.end());
    for (auto& word : capture->bufferBaseWords) {
        if (!move(word.address)) return nullptr;
    }
    for (auto& poison : capture->snapshot.srtPoison) {
        if (!move(poison.address)) return nullptr;
    }
    for (auto& base : capture->userDataUse.readBases) {
        if (base != 0 && !move(base)) return nullptr;
    }
    for (auto& [slot, address] : capture->baseSlots) {
        if (!move(address)) return nullptr;
    }
    RewriteGuardTail(*capture);
    return capture;
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> FollowedCapture(const MemoEntry& entry, std::span<const std::uint32_t> userData, const ShaderMemory::ReplayResult& result) {
    auto capture = std::make_shared<ShaderRecompiler::ResourceCapture>(*entry.capture);
    std::copy_n(userData.begin(), capture->snapshot.userData.size(), capture->snapshot.userData.begin());
    const auto patch = [&](std::span<const std::pair<std::uint64_t, std::uint32_t>> list, std::uint64_t address, std::uint32_t word) {
        auto first = std::lower_bound(list.begin(), list.end(), std::pair<std::uint64_t, std::uint32_t>{address, 0u});
        for (; first != list.end() && first->first == address; ++first) {
            if (first->second < capture->snapshot.flattenedSrt.size()) capture->snapshot.flattenedSrt[first->second] = word;
        }
    };
    for (const auto& [index, word] : result.changed) {
        const auto address = entry.accesses[index].address;
        patch(entry.dataLeaves, address, word);
        patch(entry.baseWords, address, word);
    }
    if (result.moved.empty()) return capture;
    if (result.addresses.size() != entry.accesses.size()) return nullptr;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> map;
    map.reserve(entry.accesses.size());
    for (std::size_t index = 0; index < entry.accesses.size(); ++index) map.emplace_back(entry.accesses[index].address, result.addresses[index]);
    std::sort(map.begin(), map.end());
    map.erase(std::unique(map.begin(), map.end()), map.end());
    for (std::size_t index = 1; index < map.size(); ++index) {
        if (map[index].first == map[index - 1].first) return nullptr;
    }
    bool consistent = true;
    const auto move = [&](std::uint64_t& address) {
        const auto found = std::lower_bound(map.begin(), map.end(), std::pair<std::uint64_t, std::uint64_t>{address, 0});
        if (found != map.end() && found->first == address) {
            address = found->second;
            return;
        }
        const auto& use = entry.capture->userDataUse;
        for (std::size_t read = 0; read < use.reads.size() && read < use.readBases.size(); ++read) {
            if (use.reads[read].first != address || use.readBases[read] == 0) continue;
            if (std::any_of(result.moved.begin(), result.moved.end(), [&](const std::pair<std::uint64_t, std::int64_t>& moved) { return moved.first == use.readBases[read]; })) consistent = false;
        }
    };
    for (auto& [address, origin] : capture->userDataUse.reads) move(address);
    for (auto& base : capture->userDataUse.readBases) {
        if (base != 0) move(base);
    }
    for (auto& [slot, address] : capture->readTrace.leaves) move(address);
    for (auto& address : capture->readTrace.otherReads) move(address);
    std::sort(capture->readTrace.otherReads.begin(), capture->readTrace.otherReads.end());
    capture->readTrace.otherReads.erase(std::unique(capture->readTrace.otherReads.begin(), capture->readTrace.otherReads.end()), capture->readTrace.otherReads.end());
    for (auto& word : capture->bufferBaseWords) move(word.address);
    for (auto& poison : capture->snapshot.srtPoison) move(poison.address);
    for (auto& [slot, address] : capture->baseSlots) move(address);
    if (!consistent) return nullptr;
    RewriteGuardTail(*capture);
    return capture;
}

double Per(std::uint64_t value, std::uint64_t count) {
    return count != 0 ? static_cast<double>(value) / static_cast<double>(count) : 0.0;
}

void ReportMemo() {
    auto& memo = MemoTotals();
    std::unique_lock lock(memo.reportMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - memo.lastReport < std::chrono::seconds(10)) return;
    memo.lastReport = now;
    const auto take = [](std::atomic<std::uint64_t>& counter) { return counter.exchange(0, std::memory_order_relaxed); };
    const auto total = take(memo.total);
    const auto hits = take(memo.hits);
    const auto patched = take(memo.patched);
    const auto relocated = take(memo.relocated);
    const auto dataPatched = take(memo.dataPatched);
    const auto descriptorWords = take(memo.descriptorWords);
    const auto nestedBases = take(memo.nestedBases);
    std::array<std::uint64_t, 8> roles{};
    for (std::size_t bit = 0; bit < roles.size(); ++bit) roles[bit] = take(memo.roles[bit]);
    const auto warmWalks = take(memo.warmWalks);
    const auto warmNs = take(memo.warmNanoseconds);
    const auto relocationRefused = take(memo.relocationRefused);
    const auto absent = take(memo.absent);
    const auto valueWords = take(memo.valueWords);
    const auto pointerWords = take(memo.pointerWords);
    const auto memory = take(memo.memory);
    const auto replayed = take(memo.replayed);
    const auto hitNs = take(memo.hitNanoseconds);
    const auto missNs = take(memo.missNanoseconds);
    const auto evaluationNs = take(memo.evaluationNanoseconds);
    const auto setupNs = take(memo.pageSetupNanoseconds);
    const auto pageNs = take(memo.pageReadNanoseconds);
    const auto reads = take(memo.missReads);
    const auto pages = take(memo.missPages);
    const auto values = take(memo.values);
    const auto laneEvaluators = take(memo.laneEvaluators);
    const auto phis = take(memo.phis);
    const auto served = hits + patched + relocated + dataPatched + nestedBases;
    const auto misses = total > served ? total - served : 0;
    AgcDriver::ProfilePrint_nid_no_patch("[capture-memo] (10 s) %llu prepared captures: %llu served from the memo (%llu with unread user words patched, %llu relocated to moved pointer words, %llu with changed pure SRT data words patched, %llu following moved base words read from guest memory; %.1f words replayed each, %.1f us each); %llu walked (%.1f us each: SRT evaluation %.1f with page setup %.1f and page reads %.1f inside; %.1f word reads, %.2f new pages, %.1f values evaluated, %.2f lane evaluators, %.1f phis each; %llu walks of the plan this thread walked last at %.1f us each), because: no entry %llu, read user words differ %llu (only pointer words %llu, %llu of them not relocatable), guest words differ %llu (first differing word a whole descriptor dword %llu; its roles: descriptor %llu, branch condition %llu, uniform fill %llu, address or offset %llu, flat SRT data %llu, low base word of reads %llu, probe %llu, untraced %llu)\n", static_cast<unsigned long long>(total), static_cast<unsigned long long>(served), static_cast<unsigned long long>(patched), static_cast<unsigned long long>(relocated), static_cast<unsigned long long>(dataPatched), static_cast<unsigned long long>(nestedBases), Per(replayed, served), Per(hitNs, served) / 1e3, static_cast<unsigned long long>(misses), Per(missNs, misses) / 1e3, Per(evaluationNs, misses) / 1e3, Per(setupNs, misses) / 1e3, Per(pageNs, misses) / 1e3, Per(reads, misses), Per(pages, misses), Per(values, misses), Per(laneEvaluators, misses), Per(phis, misses), static_cast<unsigned long long>(warmWalks), Per(warmNs, warmWalks) / 1e3, static_cast<unsigned long long>(absent), static_cast<unsigned long long>(valueWords + pointerWords), static_cast<unsigned long long>(pointerWords), static_cast<unsigned long long>(relocationRefused), static_cast<unsigned long long>(memory), static_cast<unsigned long long>(descriptorWords), static_cast<unsigned long long>(roles[0]), static_cast<unsigned long long>(roles[1]), static_cast<unsigned long long>(roles[2]), static_cast<unsigned long long>(roles[3]), static_cast<unsigned long long>(roles[4]), static_cast<unsigned long long>(roles[5]), static_cast<unsigned long long>(roles[6]), static_cast<unsigned long long>(roles[7]));
}

constexpr std::uint64_t WordWaitNanoseconds = 50000;

bool WordwisePages() {
    static const bool wordwise = std::getenv("APS5_NO_WORDWISE_CAPTURE") == nullptr;
    return wordwise;
}

bool CaptureProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

std::uint64_t NanosecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}

CaptureProfile& CaptureTotals() {
    static CaptureProfile profile;
    return profile;
}

std::atomic<ShaderMemory::WaitedMsProvider> waitedMsProvider{nullptr};
std::atomic<ShaderMemory::PendingGenerationProvider> pendingGenerationProvider{nullptr};

struct DirectProfile {
    std::atomic<std::uint64_t> captures{0};
    std::atomic<std::uint64_t> words{0};
    std::atomic<std::uint64_t> hookedWords{0};
    std::atomic<std::uint64_t> pageStateLookups{0};
    std::atomic<std::uint64_t> pageStateMisses{0};
};

DirectProfile& DirectTotals() {
    static DirectProfile profile;
    return profile;
}

bool DirectReadsEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DIRECT_CAPTURE_READS") == nullptr;
    return enabled;
}

constexpr std::uint8_t PageWhole = 1;
constexpr std::uint8_t PageWordwise = 2;
constexpr std::uint8_t PageInaccessible = 3;
constexpr std::size_t PageStateSlots = 1024;

struct PageState {
    std::uint64_t base = ~std::uint64_t{0};
    std::uint64_t allocation = 0;
    std::uint64_t pending = 0;
    ShaderMemory::PendingWriteQuery query = nullptr;
    ShaderMemory::PendingGenerationProvider provider = nullptr;
    std::uint8_t kind = 0;
};

struct PageStateCache {
    std::array<PageState, PageStateSlots> entries{};
    std::uint64_t serials = 0;
};

PageStateCache& ThreadPageStates() {
    struct PageStateCacheStorage {};
    return HostThreadLocal<PageStateCache, PageStateCacheStorage>();
}

double WaitedMs() {
    const auto provider = waitedMsProvider.load(std::memory_order_acquire);
    return provider != nullptr ? provider() : 0.0;
}

}

void ShaderMemory::SetWaitedMsProvider(WaitedMsProvider provider) {
    waitedMsProvider.store(provider, std::memory_order_release);
}

void ShaderMemory::SetPendingGenerationProvider(PendingGenerationProvider provider) {
    pendingGenerationProvider.store(provider, std::memory_order_release);
}

ShaderMemory::DirectReadCounts ShaderMemory::DirectReads() {
    auto& totals = DirectTotals();
    return {totals.captures.load(std::memory_order_relaxed), totals.words.load(std::memory_order_relaxed), totals.hookedWords.load(std::memory_order_relaxed), totals.pageStateLookups.load(std::memory_order_relaxed), totals.pageStateMisses.load(std::memory_order_relaxed)};
}

std::uint8_t ShaderMemory::directPageKind(std::uint64_t base, std::uint64_t allocation, PendingGenerationProvider provider, std::uint64_t pending) {
    ++directLookups;
    if (base == lastDirectBase && allocation == lastDirectAllocation && pending == lastDirectPending && provider == lastDirectProvider) return lastDirectKind;
    auto& cache = ThreadPageStates();
    auto& entry = cache.entries[static_cast<std::size_t>(base / PageBytes) % PageStateSlots];
    if (entry.base != base || entry.allocation != allocation || entry.pending != pending || entry.query != pendingWrite || entry.provider != provider) {
        ++directMisses;
        std::uint8_t kind = PageInaccessible;
        if (GuestMemory::Accessible(reinterpret_cast<const void*>(base), PageBytes)) {
            kind = pendingWrite == nullptr || pendingWrite(base, PageBytes, {}) != PendingWrite::None ? PageWordwise : PageWhole;
        }
        entry = {base, allocation, pending, pendingWrite, provider, kind};
    }
    lastDirectBase = base;
    lastDirectAllocation = allocation;
    lastDirectPending = pending;
    lastDirectProvider = provider;
    lastDirectKind = entry.kind;
    return entry.kind;
}

bool ShaderMemory::readDirect(std::uint64_t address, std::uint32_t* value) {
    if (!directIndex.empty()) {
        const auto mask = directIndex.size() - 1;
        for (auto slot = static_cast<std::size_t>((address >> 2u) * 0x9e3779b97f4a7c15ull >> 40u) & mask;; slot = (slot + 1) & mask) {
            const auto held = directIndex[slot];
            if (held == 0) break;
            if (directReads[held - 1].address == address) {
                *value = directReads[held - 1].word;
                recentReads.push_back(held - 1);
                if (recording != nullptr) recording->push_back({address, 0, *value});
                if (tapeInstance != nullptr) notePage(address & ~static_cast<std::uint64_t>(PageBytes - 1));
                return true;
            }
        }
    }
    const auto allocationBefore = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto provider = pendingGenerationProvider.load(std::memory_order_acquire);
    const auto pendingBefore = provider != nullptr ? provider() : 0;
    const auto kind = directPageKind(address & ~static_cast<std::uint64_t>(PageBytes - 1), allocationBefore, provider, provider != nullptr ? pendingBefore : instanceSerial);
    std::uint32_t word = 0;
    if (kind == PageWhole) {
        GuestMemory::CheckReadGuard(address, sizeof(word));
        std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
        if (allocationBefore != GuestAllocations::GuestAllocationsGeneration_nid_postfix() || provider != pendingGenerationProvider.load(std::memory_order_acquire) || (provider != nullptr && pendingBefore != provider())) {
            word = fetchWord(address, pendingWrite != nullptr);
            ++directHooked;
        } else {
            ++directWords;
        }
    } else {
        word = fetchWord(address, kind == PageWordwise && pendingWrite != nullptr);
        ++directHooked;
    }
    recordDirect(address, word);
    if (tapeInstance != nullptr) notePage(address & ~static_cast<std::uint64_t>(PageBytes - 1));
    *value = word;
    return true;
}

void ShaderMemory::recordDirect(std::uint64_t address, std::uint32_t word) {
    if ((directReads.size() + 1) * 2 > directIndex.size()) {
        directIndex.assign(directIndex.empty() ? 256 : directIndex.size() * 2, 0u);
        const auto mask = directIndex.size() - 1;
        for (std::size_t index = 0; index < directReads.size(); ++index) {
            auto slot = static_cast<std::size_t>((directReads[index].address >> 2u) * 0x9e3779b97f4a7c15ull >> 40u) & mask;
            while (directIndex[slot] != 0) slot = (slot + 1) & mask;
            directIndex[slot] = static_cast<std::uint32_t>(index + 1);
        }
    }
    directReads.push_back({address, word});
    const auto mask = directIndex.size() - 1;
    auto slot = static_cast<std::size_t>((address >> 2u) * 0x9e3779b97f4a7c15ull >> 40u) & mask;
    while (directIndex[slot] != 0) slot = (slot + 1) & mask;
    directIndex[slot] = static_cast<std::uint32_t>(directReads.size());
    recentReads.push_back(static_cast<std::uint32_t>(directReads.size() - 1));
    if (recording != nullptr) recording->push_back({address, 0, word});
}

bool ShaderMemory::stable(void* context, std::uint64_t address, std::uint32_t word) {
    return static_cast<ShaderMemory*>(context)->stableDirect(address, word);
}

bool ShaderMemory::stableDirect(std::uint64_t address, std::uint32_t word) {
    if (!direct || stableSince == 0 || address % sizeof(word) != 0) return false;
    if (!initial.empty() && address < initialHigh && address + sizeof(word) > initialLow) return false;
    if (!directIndex.empty()) {
        const auto mask = directIndex.size() - 1;
        for (auto slot = static_cast<std::size_t>((address >> 2u) * 0x9e3779b97f4a7c15ull >> 40u) & mask;; slot = (slot + 1) & mask) {
            const auto held = directIndex[slot];
            if (held == 0) break;
            if (directReads[held - 1].address != address) continue;
            if (directReads[held - 1].word != word) return false;
            recentReads.push_back(held - 1);
            notePage(address & ~static_cast<std::uint64_t>(PageBytes - 1));
            return true;
        }
    }
    const auto allocation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    if (allocation != stableAllocation) return false;
    const auto provider = pendingGenerationProvider.load(std::memory_order_acquire);
    const auto pending = provider != nullptr ? provider() : instanceSerial;
    const auto base = address & ~static_cast<std::uint64_t>(PageBytes - 1);
    if (directPageKind(base, allocation, provider, pending) != PageWhole || !pageUnchanged(base)) return false;
    recordDirect(address, word);
    keptWords.emplace_back(address, word);
    notePage(base);
    return true;
}

void ShaderMemory::notePage(std::uint64_t base) {
    if (std::find(runPages.begin(), runPages.end(), base) == runPages.end()) runPages.push_back(base);
}

bool ShaderMemory::pageUnchanged(std::uint64_t base) {
    for (const auto& [page, unchanged] : stablePages) {
        if (page == base) return unchanged;
    }
    const bool unchanged = GuestMemory::CollectWrites(base, PageBytes) != 0 && GuestMemory::UnchangedSince(base, PageBytes, stableSince);
    stablePages.emplace_back(base, unchanged);
    return unchanged;
}

std::vector<std::pair<std::uint64_t, std::uint32_t>> ShaderMemory::TakeKeptWords() {
    return std::exchange(keptWords, {});
}

bool ShaderMemory::probeDirect(std::uint64_t address, std::uint64_t end) {
    const auto allocation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto provider = pendingGenerationProvider.load(std::memory_order_acquire);
    const auto pending = provider != nullptr ? provider() : instanceSerial;
    while (address < end) {
        const auto base = address & ~static_cast<std::uint64_t>(PageBytes - 1);
        const auto pageEnd = std::min<std::uint64_t>(base + PageBytes, end);
        if (directPageKind(base, allocation, provider, pending) == PageInaccessible && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(pageEnd - address))) return false;
        address = pageEnd;
    }
    return true;
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::directRegions(std::span<const std::uint32_t> indices, bool withInitial) const {
    std::vector<DirectRead> reads;
    reads.reserve(indices.size());
    for (const auto index : indices) reads.push_back(directReads[index]);
    std::sort(reads.begin(), reads.end(), [](const DirectRead& a, const DirectRead& b) { return a.address < b.address; });
    reads.erase(std::unique(reads.begin(), reads.end(), [](const DirectRead& a, const DirectRead& b) { return a.address == b.address; }), reads.end());
    std::vector<ShaderRecompiler::MemoryRegion> result;
    result.reserve(reads.size() + (withInitial ? initial.size() : 0));
    std::unique_ptr<std::uint32_t[]> storage(reads.empty() ? nullptr : new std::uint32_t[reads.size()]);
    auto next = withInitial ? initial.begin() : initial.end();
    for (std::size_t index = 0; index < reads.size();) {
        const auto first = index;
        storage[index] = reads[index].word;
        ++index;
        while (index < reads.size() && reads[index].address / PageBytes == reads[first].address / PageBytes && reads[index].address == reads[index - 1].address + sizeof(std::uint32_t)) {
            storage[index] = reads[index].word;
            ++index;
        }
        while (next != initial.end() && next->first < reads[first].address) {
            result.push_back({next->first, next->second});
            ++next;
        }
        result.push_back({reads[first].address, std::as_bytes(std::span(storage.get() + first, index - first))});
    }
    for (; next != initial.end(); ++next) result.push_back({next->first, next->second});
    if (storage != nullptr) regionStorage.push_back(std::move(storage));
    return result;
}

ShaderMemory::ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> regions, PendingWriteQuery pendingWrite, PendingWriteObserver observe, HookWaitCounter hookWaits) : pendingWrite(pendingWrite), observe(observe), hookWaits(hookWaits) {
    const ShaderRecompiler::RequestMemoryView validated(regions);
    for (const auto& region : regions) {
        initial.emplace(region.guestAddress, region.bytes);
        initialLow = std::min(initialLow, region.guestAddress);
        initialHigh = std::max(initialHigh, region.guestAddress + region.bytes.size());
    }
}

ShaderMemory::~ShaderMemory() {
    if (readCount != 0) CaptureTotals().reads.fetch_add(readCount, std::memory_order_relaxed);
    if (pageCount != 0) CaptureTotals().pages.fetch_add(pageCount, std::memory_order_relaxed);
    if (direct) {
        auto& totals = DirectTotals();
        totals.words.fetch_add(directWords, std::memory_order_relaxed);
        totals.hookedWords.fetch_add(directHooked, std::memory_order_relaxed);
        totals.pageStateLookups.fetch_add(directLookups, std::memory_order_relaxed);
        totals.pageStateMisses.fetch_add(directMisses, std::memory_order_relaxed);
    }
}

std::uint32_t ShaderMemory::fetchWord(std::uint64_t address, bool wordwise) {
    auto& self = *this;
    std::uint32_t word = 0;
    auto policy = PendingWrite::Sync;
    if (wordwise) {
        policy = self.pendingWrite(address, sizeof(word), std::as_writable_bytes(std::span(&word, 1)));
        if (policy != PendingWrite::None && policy != PendingWrite::Sync && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(word))) policy = PendingWrite::Sync;
    }
    const auto hookWaits = [&] { return self.hookWaits != nullptr ? self.hookWaits() : 0; };
    const auto report = [&](std::uint64_t waitsBefore, bool unchanged) {
        if (self.hookWaits == nullptr || self.hookWaits() != waitsBefore) self.observe(address, unchanged);
        else ++CaptureTotals().observationsSkipped;
    };
    const auto fetchStarted = CaptureProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (policy == PendingWrite::KnownValue || policy == PendingWrite::VerifyKnownValue) {
        ++CaptureTotals().wordsKnown;
        if (policy == PendingWrite::VerifyKnownValue) {
            std::uint32_t waited = 0;
            const auto waitsBefore = hookWaits();
            GuestMemory::Read(address, std::as_writable_bytes(std::span(&waited, 1)), alignof(std::uint32_t));
            ++CaptureTotals().wordsKnownVerified;
            if (waited != word) ++CaptureTotals().wordsKnownMismatches;
            if (self.observe != nullptr) report(waitsBefore, waited == word);
            word = waited;
        }
    } else if (policy == PendingWrite::Raw || policy == PendingWrite::VerifyRaw) {
        std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
        ++CaptureTotals().wordsRaw;
        if (policy == PendingWrite::VerifyRaw) {
            std::uint32_t waited = 0;
            const auto waitsBefore = hookWaits();
            GuestMemory::Read(address, std::as_writable_bytes(std::span(&waited, 1)), alignof(std::uint32_t));
            ++CaptureTotals().wordsVerified;
            if (waited != word) ++CaptureTotals().wordMismatches;
            if (self.observe != nullptr) report(waitsBefore, waited == word);
            word = waited;
        }
    } else {
        const bool observed = wordwise && policy == PendingWrite::Sync && self.observe != nullptr && GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(word));
        std::uint32_t before = 0;
        if (observed) std::memcpy(&before, reinterpret_cast<const void*>(address), sizeof(before));
        const auto waitsBefore = observed ? hookWaits() : 0;
        const auto started = wordwise ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        GuestMemory::Read(address, std::as_writable_bytes(std::span(&word, 1)), alignof(std::uint32_t));
        if (wordwise) {
            const auto nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
            if (nanoseconds >= WordWaitNanoseconds) {
                ++CaptureTotals().wordWaits;
                CaptureTotals().wordWaitNanoseconds += nanoseconds;
            }
        }
        if (observed) report(waitsBefore, before == word);
    }
    if (CaptureProfiled()) CaptureTotals().wordReadNanoseconds += NanosecondsSince(fetchStarted);
    return word;
}

ShaderMemory::Page& ShaderMemory::page(std::uint64_t base) {
    if (base == lastBase) return *lastPage;
    const auto found = pages.find(base);
    if (found != pages.end()) {
        lastBase = base;
        lastPage = &found->second;
        return found->second;
    }
    auto& page = pages[base];
    lastBase = base;
    lastPage = &page;
    ++pageCount;
    const bool profile = CaptureProfiled();
    const auto setupStarted = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // A page is either mapped whole or read word by word where the guest mapped less than a page,
    // or where recorded GPU work writes into it (a whole-page read would wait for that work).
    if (GuestMemory::Accessible(reinterpret_cast<const void*>(base), PageBytes)) {
        if (WordwisePages() && pendingWrite != nullptr && pendingWrite(base, PageBytes, {}) != PendingWrite::None) {
            page.wordwise = true;
            ++CaptureTotals().pagesWordwise;
            if (profile) pageSetupNanoseconds += NanosecondsSince(setupStarted);
            return page;
        }
        const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (profile) pageSetupNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(started - setupStarted).count());
        GuestMemory::Read(base, std::as_writable_bytes(std::span(page.words)), sizeof(std::uint32_t));
        if (profile) {
            const auto nanoseconds = NanosecondsSince(started);
            CaptureTotals().pageReadNanoseconds += nanoseconds;
            pageReadNanoseconds += nanoseconds;
        }
        page.valid.set();
    } else if (profile) {
        pageSetupNanoseconds += NanosecondsSince(setupStarted);
    }
    return page;
}

bool ShaderMemory::read(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& self = *static_cast<ShaderMemory*>(context);
    if (address % sizeof(*value) != 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) {
        throw std::runtime_error("AGC driver: invalid shader memory read address");
    }
    ++self.readCount;
    if (!self.initial.empty() && address < self.initialHigh && address + sizeof(*value) > self.initialLow) {
        const auto next = self.initial.upper_bound(address);
        if (next != self.initial.begin()) {
            const auto previous = std::prev(next);
            const auto offset = address - previous->first;
            if (offset < previous->second.size()) {
                if (previous->second.size() - offset < sizeof(*value)) throw std::runtime_error("AGC driver: shader memory read crosses a snapshot boundary");
                std::memcpy(value, previous->second.data() + offset, sizeof(*value));
                if (self.recording != nullptr) self.recording->push_back({address, 0, *value});
                return true;
            }
        }
        if (next != self.initial.end() && next->first - address < sizeof(*value)) throw std::runtime_error("AGC driver: shader memory read overlaps a snapshot boundary");
    }
    if (self.direct) return self.readDirect(address, value);
    auto& page = self.page(address & ~static_cast<std::uint64_t>(PageBytes - 1));
    const auto index = static_cast<std::size_t>((address % PageBytes) / sizeof(*value));
    if (!page.valid.test(index)) {
        page.words[index] = self.fetchWord(address, page.wordwise);
        page.valid.set(index);
    }
    page.read.set(index);
    page.recent.set(index);
    *value = page.words[index];
    if (self.recording != nullptr) self.recording->push_back({address, 0, *value});
    return true;
}

bool ShaderMemory::overlapsInitial(std::uint64_t begin, std::uint64_t end) const {
    if (initial.empty()) return false;
    const auto after = initial.lower_bound(begin);
    if (after != initial.end() && after->first < end) return true;
    return after != initial.begin() && std::prev(after)->first + std::prev(after)->second.size() > begin;
}

bool ShaderMemory::readWords(void* context, std::uint64_t address, std::uint32_t* values, std::uint64_t count) {
    auto& self = *static_cast<ShaderMemory*>(context);
    while (count != 0) {
        const auto base = address & ~static_cast<std::uint64_t>(PageBytes - 1);
        const auto index = static_cast<std::size_t>((address % PageBytes) / sizeof(std::uint32_t));
        const auto words = static_cast<std::size_t>(std::min<std::uint64_t>(count, PageWords - index));
        const auto bytes = static_cast<std::uint64_t>(words) * sizeof(std::uint32_t);
        Page* page = nullptr;
        if (!self.direct && address % sizeof(std::uint32_t) == 0 && address <= std::numeric_limits<std::uint64_t>::max() - bytes && !self.overlapsInitial(address, address + bytes)) page = &self.page(base);
        if (page != nullptr && page->valid.all()) {
            std::memcpy(values, page->words.data() + index, bytes);
            std::bitset<PageWords> mask;
            mask.set();
            mask >>= PageWords - words;
            mask <<= index;
            page->read |= mask;
            page->recent |= mask;
            self.readCount += words;
            if (self.recording != nullptr) {
                for (std::size_t word = 0; word < words; ++word) self.recording->push_back({address + word * sizeof(std::uint32_t), 0, values[word]});
            }
        } else {
            for (std::size_t word = 0; word < words; ++word) {
                if (!read(context, address + word * sizeof(std::uint32_t), values + word)) return false;
            }
        }
        address += bytes;
        values += words;
        count -= words;
    }
    return true;
}

bool ShaderMemory::accessible(void* context, std::uint64_t address, std::uint64_t bytes) {
    auto& self = *static_cast<ShaderMemory*>(context);
    const bool result = self.probe(address, bytes);
    if (self.recording != nullptr) self.recording->push_back({address, bytes == 0 ? ~std::uint64_t{0} : bytes, result ? 1u : 0u});
    return result;
}

bool ShaderMemory::probe(std::uint64_t address, std::uint64_t bytes) {
    auto& self = *this;
    if (bytes == 0 || address > std::numeric_limits<std::uint64_t>::max() - bytes) return false;
    const auto end = address + bytes;
    while (address < end) {
        const auto next = address < self.initialHigh && end > self.initialLow ? self.initial.upper_bound(address) : self.initial.begin();
        if (next != self.initial.begin()) {
            const auto previous = std::prev(next);
            if (address - previous->first < previous->second.size()) {
                address = previous->first + previous->second.size();
                continue;
            }
        }
        const auto base = address & ~static_cast<std::uint64_t>(PageBytes - 1);
        const auto pageEnd = std::min<std::uint64_t>(base + PageBytes, end);
        if (self.direct) {
            if (!self.probeDirect(address, pageEnd)) return false;
            address = pageEnd;
            continue;
        }
        const auto& page = self.page(base);
        if (!page.wordwise && !page.valid.all() && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(pageEnd - address))) return false;
        address = pageEnd;
    }
    return true;
}

bool ShaderMemory::pending(void* context, std::uint64_t address, std::uint64_t bytes) {
    const auto& self = *static_cast<const ShaderMemory*>(context);
    if (self.pendingWrite == nullptr || bytes == 0) return false;
    const auto policy = self.pendingWrite(address, static_cast<std::size_t>(bytes), {});
    return policy == PendingWrite::Sync || policy == PendingWrite::VerifyRaw || policy == PendingWrite::VerifyKnownValue;
}

bool ShaderMemory::UsesImageTables(const ShaderRecompiler::IrResourcePlan& plan) {
    return AgcDriver::UsesImageTables(plan);
}

ShaderMemory::KnownValueCounts ShaderMemory::KnownValues() {
    auto& totals = CaptureTotals();
    return {totals.wordsKnown.load(std::memory_order_relaxed), totals.wordsKnownVerified.load(std::memory_order_relaxed), totals.wordsKnownMismatches.load(std::memory_order_relaxed)};
}

std::uint64_t ShaderMemory::MemoizedCaptures() {
    return memoizedCaptures.load(std::memory_order_relaxed);
}

bool ShaderMemory::replay(std::span<const RecordedAccess> accesses, const ReplayPlan* plan, ReplayResult* result) {
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Capture);
    const auto listed = [](std::span<const std::pair<std::uint64_t, std::uint32_t>> list, std::uint64_t address) {
        const auto found = std::lower_bound(list.begin(), list.end(), address, [](const std::pair<std::uint64_t, std::uint32_t>& entry, std::uint64_t value) { return entry.first < value; });
        return found != list.end() && found->first == address;
    };
    if (result != nullptr) {
        result->changed.clear();
        result->moved.clear();
        result->addresses.clear();
        result->mismatch = 0;
    }
    for (std::size_t index = 0; index < accesses.size(); ++index) {
        const auto& access = accesses[index];
        auto address = access.address;
        if (plan != nullptr && result != nullptr && !result->moved.empty() && index < plan->accessBases.size() && plan->accessBases[index] != 0) {
            const auto base = plan->accessBases[index];
            if (base == ~std::uint64_t{0}) return false;
            const auto moved = std::find_if(result->moved.begin(), result->moved.end(), [base](const std::pair<std::uint64_t, std::int64_t>& entry) { return entry.first == base; });
            if (moved != result->moved.end()) {
                const auto delta = moved->second;
                if (delta < 0 ? address < static_cast<std::uint64_t>(-delta) : address > GuestAddressMask - static_cast<std::uint64_t>(delta)) return false;
                address += static_cast<std::uint64_t>(delta);
            }
        }
        if (result != nullptr) result->addresses.push_back(address);
        if (access.bytes == 0) {
            std::uint32_t word = 0;
            if (!read(this, address, &word)) return false;
            if (word == access.value) continue;
            if (plan == nullptr || result == nullptr) return false;
            if (listed(plan->dataLeaves, access.address)) {
                result->changed.emplace_back(index, word);
                continue;
            }
            if (listed(plan->baseWords, access.address) && access.value != 0u && word != 0u) {
                result->moved.emplace_back(access.address, static_cast<std::int64_t>(word & ~3u) - static_cast<std::int64_t>(access.value & ~3u));
                result->changed.emplace_back(index, word);
                continue;
            }
            result->mismatch = access.address;
            return false;
        }
        if (probe(address, access.bytes == ~std::uint64_t{0} ? 0 : access.bytes) != (access.value != 0u)) return false;
    }
    return true;
}

void ShaderMemory::CountHandleMemo(bool hit) {
    if (!CaptureProfiled()) return;
    (hit ? CaptureTotals().handleHits : CaptureTotals().handleMisses).fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle) {
    return capture(request, handle, nullptr);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::PreparedShaderInvocation& invocation) {
    auto& table = ThreadMemo();
    if (!CaptureMemoEnabled() || invocation.Handle() == nullptr || table.depth != 0) return capture(invocation.Request(), nullptr, &invocation);
    const MemoDepth depth(table);
    const auto& request = invocation.Request();
    const auto& handle = invocation.Handle();
    const auto userData = request.context.userData;
    const auto shaderBase = request.shader.codeAddress;
    const bool profile = CaptureProfiled();
    auto& memo = MemoTotals();
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto found = table.shaders.find(handle.get());
    if (found == table.shaders.end()) {
        if (table.shaders.size() >= MemoShaders) table.shaders.clear();
        found = table.shaders.emplace(handle.get(), MemoShader{}).first;
    }
    auto& shader = found->second;
    MemoEntry* slot = nullptr;
    auto reason = MemoMatch::Other;
    const auto serve = [&](MemoEntry& entry, MemoMatch match, std::size_t replayed) {
        memoizedCaptures.fetch_add(1, std::memory_order_relaxed);
        if (profile) {
            ++memo.total;
            ++(match == MemoMatch::Exact ? memo.hits : match == MemoMatch::UnreadWords ? memo.patched : match == MemoMatch::DataWords ? memo.dataPatched : match == MemoMatch::NestedBases ? memo.nestedBases : memo.relocated);
            memo.replayed += replayed;
            memo.hitNanoseconds += NanosecondsSince(started);
            ReportMemo();
        }
        return entry.capture;
    };
    for (auto& entry : shader.ways) {
        if (entry.handle != handle) continue;
        const auto match = MatchUserData(entry, shaderBase, userData);
        if (match == MemoMatch::Exact || match == MemoMatch::UnreadWords) {
            auto& result = scratchReplay();
            DataLeaves(entry);
            const ReplayPlan plan{entry.dataLeaves, entry.baseWords, entry.accessBases};
            if (!replay(entry.accesses, &plan, &result)) {
                if (slot == nullptr) slot = &entry;
                if (profile) {
                    ++memo.memory;
                    if (result.mismatch != 0 && FeedsDescriptorDirectly(*entry.capture, result.mismatch)) ++memo.descriptorWords;
                    const auto roles = result.mismatch != 0 ? ReadRoles(*entry.capture, result.mismatch) : std::uint8_t{0x40};
                    for (std::size_t bit = 0; bit < memo.roles.size(); ++bit) {
                        if ((roles >> bit) & 1u) ++memo.roles[bit];
                    }
                }
                reason = MemoMatch::Exact;
                continue;
            }
            if (match == MemoMatch::UnreadWords || !result.changed.empty()) {
                auto patched = FollowedCapture(entry, userData, result);
                if (patched == nullptr) {
                    if (slot == nullptr) slot = &entry;
                    if (profile) ++memo.memory;
                    reason = MemoMatch::Exact;
                    continue;
                }
                for (const auto& [index, word] : result.changed) entry.accesses[index].value = word;
                if (!result.moved.empty()) {
                    for (std::size_t index = 0; index < entry.accesses.size(); ++index) entry.accesses[index].address = result.addresses[index];
                    ClearDerived(entry);
                }
                entry.capture = std::move(patched);
                entry.userData.assign(userData.begin(), userData.end());
            }
            return serve(entry, !result.moved.empty() ? MemoMatch::NestedBases : !result.changed.empty() ? MemoMatch::DataWords : match, entry.accesses.size());
        }
        if (match == MemoMatch::PointerWords) {
            Relocation relocation;
            std::vector<RecordedAccess> moved;
            bool relocatable = RelocationFor(entry, userData, relocation);
            if (relocatable) {
                const auto origins = std::span(Origins(entry));
                moved.reserve(entry.accesses.size());
                for (const auto& access : entry.accesses) {
                    std::uint64_t address = 0;
                    if (!RelocatedAddress(origins, access.address, relocation, address)) {
                        relocatable = false;
                        break;
                    }
                    moved.push_back({address, access.bytes, access.value});
                }
            }
            if (!relocatable) {
                if (profile) ++memo.relocationRefused;
                if (reason != MemoMatch::Exact) reason = MemoMatch::PointerWords;
                continue;
            }
            if (!replay(moved)) {
                if (slot == nullptr) slot = &entry;
                if (profile) ++memo.memory;
                reason = MemoMatch::Exact;
                continue;
            }
            auto capture = RelocatedCapture(entry, userData, relocation);
            if (capture == nullptr) {
                if (profile) ++memo.relocationRefused;
                if (reason != MemoMatch::Exact) reason = MemoMatch::PointerWords;
                continue;
            }
            ClearDerived(entry);
            entry.accesses = std::move(moved);
            entry.userData.assign(userData.begin(), userData.end());
            entry.capture = std::move(capture);
            return serve(entry, MemoMatch::PointerWords, entry.accesses.size());
        }
        if (match == MemoMatch::ValueWords && reason == MemoMatch::Other) reason = MemoMatch::ValueWords;
    }
    if (profile && reason != MemoMatch::Exact) {
        if (reason == MemoMatch::PointerWords) ++memo.pointerWords;
        else if (reason == MemoMatch::ValueWords) ++memo.valueWords;
        else ++memo.absent;
    }
    if (slot == nullptr) slot = &shader.ways[shader.next++ % MemoWays];
    slot->capture = nullptr;
    slot->accesses.clear();
    ClearDerived(*slot);
    const auto pagesBefore = profile ? pages.size() : 0;
    const auto setupBefore = pageSetupNanoseconds;
    const auto pageReadBefore = pageReadNanoseconds;
    const auto evaluationBefore = profile ? ShaderRecompiler::ResourceMaterializer::ThreadEvaluationNanoseconds() : 0;
    const auto countsBefore = profile ? ShaderRecompiler::ThreadSrtEvaluationCounts() : ShaderRecompiler::SrtEvaluationCounts{};
    recording = &slot->accesses;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> result;
    try {
        result = capture(request, nullptr, &invocation);
    } catch (...) {
        recording = nullptr;
        *slot = MemoEntry{};
        throw;
    }
    recording = nullptr;
    if (slot->handle != handle) slot->handle = handle;
    slot->shaderBase = shaderBase;
    slot->userData.assign(userData.begin(), userData.end());
    slot->capture = result;
    if (profile) {
        ++memo.total;
        memo.missReads += static_cast<std::uint64_t>(std::count_if(slot->accesses.begin(), slot->accesses.end(), [](const RecordedAccess& access) { return access.bytes == 0; }));
        memo.missPages += pages.size() - pagesBefore;
        memo.pageSetupNanoseconds += pageSetupNanoseconds - setupBefore;
        memo.pageReadNanoseconds += pageReadNanoseconds - pageReadBefore;
        memo.evaluationNanoseconds += ShaderRecompiler::ResourceMaterializer::ThreadEvaluationNanoseconds() - evaluationBefore;
        const auto counts = ShaderRecompiler::ThreadSrtEvaluationCounts();
        memo.values += counts.values - countsBefore.values;
        memo.laneEvaluators += counts.laneEvaluators - countsBefore.laneEvaluators;
        memo.phis += counts.phis - countsBefore.phis;
        const auto walkNs = NanosecondsSince(started);
        memo.missNanoseconds += walkNs;
        if (table.lastPlan == result->plan.get()) {
            ++memo.warmWalks;
            memo.warmNanoseconds += walkNs;
        }
        table.lastPlan = result->plan.get();
        ReportMemo();
    }
    return result;
}

void ShaderMemory::enableDirect() {
    if (!direct && DirectReadsEnabled() && WordwisePages() && pages.empty()) {
        direct = true;
        auto& cache = ThreadPageStates();
        instanceSerial = ++cache.serials;
        DirectTotals().captures.fetch_add(1, std::memory_order_relaxed);
    }
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::CaptureValues(const ShaderRecompiler::PreparedShaderInvocation& invocation) {
    enableDirect();
    return capture(invocation.Request(), nullptr, &invocation, true);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::CaptureValues(const ShaderRecompiler::PreparedShaderInvocation& invocation, IncrementalState& state) {
    enableDirect();
    const auto allocation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    stableSince = direct && state.allocation == allocation ? state.generation : 0;
    stableAllocation = allocation;
    stablePages.clear();
    keptWords.clear();
    runPages.clear();
    if (stableSince != 0) {
        for (const auto base : state.pages) stablePages.emplace_back(base, GuestMemory::CollectWrites(base, PageBytes) != 0 && GuestMemory::UnchangedSince(base, PageBytes, stableSince));
    }
    const auto generation = GuestMemory::TrackerGeneration();
    if (state.instance == nullptr) state.instance = std::make_shared<ShaderRecompiler::SrtTapeInstance>();
    tapeInstance = state.instance.get();
    struct Reset {
        ShaderMemory& memory;
        ~Reset() {
            memory.tapeInstance = nullptr;
            memory.stableSince = 0;
        }
    } reset{*this};
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> result;
    try {
        result = capture(invocation.Request(), nullptr, &invocation, true);
    } catch (...) {
        state = {};
        throw;
    }
    state.plan = result->plan;
    state.generation = generation;
    state.allocation = allocation;
    state.pages = runPages;
    return result;
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle, const ShaderRecompiler::PreparedShaderInvocation* invocation, bool valuesOnly) {
    // The capture's word and page reads (through `read`) are attributed to it ([hooksync], [guestmem]).
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Capture);
    const bool profile = CaptureProfiled();
    auto& totals = CaptureTotals();
    // The hook's GPU waits made anywhere inside the capture.
    const auto waitedBefore = profile ? WaitedMs() : 0.0;
    const auto started = std::chrono::steady_clock::now();
    ShaderRecompiler::SrtRuntime runtime;
    runtime.userData = request.context.userData;
    runtime.shaderBase = request.shader.codeAddress;
    runtime.userContext = this;
    runtime.readMemory = &read;
    runtime.readSpecializationMemory = &read;
    runtime.accessible = &accessible;
    runtime.pendingWrite = &pending;
    runtime.readWords = &readWords;
    if (valuesOnly && tapeInstance != nullptr) {
        runtime.tapeInstance = tapeInstance;
        runtime.stableWord = &stable;
    }
    auto capture = invocation != nullptr ? (valuesOnly ? invocation->CaptureValues(runtime) : invocation->Capture(runtime)) : handle != nullptr ? ShaderRecompiler::CaptureResources(request, runtime, *handle) : ShaderRecompiler::CaptureResources(request, runtime);
    if (profile) {
        totals.captureNanoseconds += NanosecondsSince(started);
        totals.resolveNanoseconds += capture->sourceNanoseconds;
        totals.hookWaitNanoseconds += static_cast<std::uint64_t>((WaitedMs() - waitedBefore) * 1e6);
        std::uint64_t initialBytes = 0;
        for (const auto& [address, bytes] : initial) initialBytes += bytes.size();
        if (++totals.captures % 500 == 0) {
            const auto captures = static_cast<double>(totals.captures.load());
            const auto captureNs = totals.captureNanoseconds.load();
            const auto resolveNs = totals.resolveNanoseconds.load();
            const auto pageNs = totals.pageReadNanoseconds.load();
            const auto wordNs = totals.wordReadNanoseconds.load();
            const auto specializationNs = ShaderRecompiler::ResourceMaterializer::SpecializationNanoseconds();
            const auto hookNs = totals.hookWaitNanoseconds.load();
            const auto walkNs = captureNs > resolveNs + pageNs + wordNs + specializationNs ? captureNs - resolveNs - pageNs - wordNs - specializationNs : 0;
            AgcDriver::ProfilePrint_nid_no_patch( "[capture] %llu captures: %llu word reads, %llu pages fetched (%llu read word by word over pending GPU writes: %llu words waited %.2f s, %llu read raw on evidence, %llu verified with %llu mismatches, %llu served from known values, %llu hook reads not observed: no GPU wait), capture (plan lookup + materialize) %.1f s, %llu KiB registered code this capture; sub-phases (s, us per capture): source resolve %.2f/%.1f, walk %.2f/%.1f, specialization (every materialize) %.2f/%.1f, page reads %.2f/%.1f, word reads %.2f/%.1f, hook GPU waits inside %.2f/%.1f; source handle memo %llu hits, %llu resolves\n", static_cast<unsigned long long>(totals.captures.load()), static_cast<unsigned long long>(totals.reads.load()), static_cast<unsigned long long>(totals.pages.load()), static_cast<unsigned long long>(totals.pagesWordwise.load()), static_cast<unsigned long long>(totals.wordWaits.load()), totals.wordWaitNanoseconds.load() / 1e9, static_cast<unsigned long long>(totals.wordsRaw.load()), static_cast<unsigned long long>(totals.wordsVerified.load()), static_cast<unsigned long long>(totals.wordMismatches.load()), static_cast<unsigned long long>(totals.wordsKnown.load()), static_cast<unsigned long long>(totals.observationsSkipped.load()), captureNs / 1e9, static_cast<unsigned long long>(initialBytes / 1024), resolveNs / 1e9, resolveNs / captures / 1e3, walkNs / 1e9, walkNs / captures / 1e3, specializationNs / 1e9, specializationNs / captures / 1e3, pageNs / 1e9, pageNs / captures / 1e3, wordNs / 1e9, wordNs / captures / 1e3, hookNs / 1e9, hookNs / captures / 1e3, static_cast<unsigned long long>(totals.handleHits.load()), static_cast<unsigned long long>(totals.handleMisses.load()));
        }
    }
    return capture;
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::Regions() const {
    if (direct) {
        std::vector<std::uint32_t> all(directReads.size());
        for (std::size_t index = 0; index < all.size(); ++index) all[index] = static_cast<std::uint32_t>(index);
        return directRegions(all, true);
    }
    std::vector<ShaderRecompiler::MemoryRegion> result;
    result.reserve(initial.size() + pages.size());
    auto next = initial.begin();
    // Both maps are ordered by address and never overlap, so a merge keeps the result sorted.
    for (const auto& [base, page] : pages) {
        while (next != initial.end() && next->first < base) {
            result.push_back({next->first, next->second});
            ++next;
        }
        for (std::size_t index = 0; index < PageWords;) {
            if (!page.read.test(index)) {
                ++index;
                continue;
            }
            const auto first = index;
            while (index < PageWords && page.read.test(index)) ++index;
            result.push_back({base + first * sizeof(std::uint32_t), std::as_bytes(std::span(page.words).subspan(first, index - first))});
        }
    }
    for (; next != initial.end(); ++next) result.push_back({next->first, next->second});
    return result;
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::TakeRecentRegions() {
    if (direct) {
        auto result = directRegions(recentReads, false);
        recentReads.clear();
        return result;
    }
    std::vector<ShaderRecompiler::MemoryRegion> result;
    for (auto& [base, page] : pages) {
        if (page.recent.none()) continue;
        for (std::size_t index = 0; index < PageWords;) {
            if (!page.recent.test(index)) {
                ++index;
                continue;
            }
            const auto first = index;
            while (index < PageWords && page.recent.test(index)) ++index;
            result.push_back({base + first * sizeof(std::uint32_t), std::as_bytes(std::span(page.words).subspan(first, index - first))});
        }
        page.recent.reset();
    }
    return result;
}

DataWordPositionCounts DataWordPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads, std::span<const std::uint32_t> words, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots) {
    DataWordPositionCounts counts;
    positions.clear();
    slots.clear();
    std::vector<std::size_t> prefix(runs.size() + 1, 0);
    for (std::size_t i = 0; i < runs.size(); ++i) prefix[i + 1] = prefix[i] + static_cast<std::size_t>((runs[i].second - runs[i].first) / sizeof(std::uint32_t));
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    for (const auto& [slot, address] : leaves) {
        if ((address & 3u) != 0) {
            ++counts.unmapped;
            continue;
        }
        if (std::binary_search(otherReads.begin(), otherReads.end(), address)) {
            ++counts.aliased;
            continue;
        }
        auto run = std::upper_bound(runs.begin(), runs.end(), address, [](std::uint64_t value, const std::pair<std::uint64_t, std::uint64_t>& candidate) { return value < candidate.first; });
        if (run == runs.begin() || address + sizeof(std::uint32_t) > (run - 1)->second) {
            ++counts.unmapped;
            continue;
        }
        --run;
        const auto position = prefix[static_cast<std::size_t>(run - runs.begin())] + static_cast<std::size_t>((address - run->first) / sizeof(std::uint32_t));
        if (position >= words.size() || slot >= flattenedSrt.size() || words[position] != flattenedSrt[slot]) {
            ++counts.mismatched;
            continue;
        }
        found.emplace_back(static_cast<std::uint32_t>(position), slot);
    }
    std::sort(found.begin(), found.end());
    positions.reserve(found.size());
    slots.reserve(found.size());
    for (const auto& [position, slot] : found) {
        positions.push_back(position);
        slots.push_back(slot);
    }
    return counts;
}

}

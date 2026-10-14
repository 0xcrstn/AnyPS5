#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWCACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWCACHE_HPP

#include "UserDataUse.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
struct ResourceSnapshot;
}

namespace AgcDriver::DriverDetail {

struct DrawProgram {
    ShaderRecompiler::ShaderBinary binary;
    std::uint32_t userDataBase;
    std::uint32_t firstUserSgpr = 8;
    std::vector<std::uint32_t> userData;
    std::array<ShaderRecompiler::MemoryRegion, 2> memory;

    std::shared_ptr<const ShaderSnapshot> snapshot;
    std::size_t codeOffset = 0;
    std::uint32_t hiddenUserWords = 0;
};

struct DrawDecode {
    Graphics::State state;
    ShaderRecompiler::ShaderPixelStageInfo pixel;
    std::vector<DrawProgram> programs;
    std::vector<ShaderRecompiler::ProgramRole> roles;
};

void DecodeGraphicsPrograms(DrawDecode& decoded, const QueueState& queue, const ShaderRegistry& registry, bool staticAbi, bool includeFragment);

struct PreparedGraphicsStage {
    std::shared_ptr<const ShaderSnapshot> snapshot;
    PreparedShaders::Entry entry;
    ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Vertex;
    std::uint32_t pushOffset = 0;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> registration;
};

std::vector<PreparedGraphicsStage> PrepareGraphicsStages(const DrawDecode& decoded, const ShaderRecompiler::SpirvTarget& target);

struct DrawRecipeRecord {
    std::vector<std::weak_ptr<const DispatchVariant>> stages;
    std::shared_ptr<const DrawRecipe> recipe;
    bool Matches(const std::vector<std::shared_ptr<DispatchVariant>>& variants) const;
    bool Expired() const;
};

struct DrawEntry {

    std::shared_ptr<const DrawDecode> decode;

    std::vector<std::vector<std::shared_ptr<DispatchVariant>>> stages;

    std::atomic<std::shared_ptr<const std::vector<DrawRecipeRecord>>> recipes;
    std::uint64_t touched = 0;
    std::list<std::uint64_t>::iterator order;
};

enum class DrawMiss : std::size_t { FrontDiffering, FragmentDiffering, OtherDiffering, Layout, Gate, Stages, NoVariant, Count };

struct DrawEntryCounters {
    std::uint64_t lookups = 0, absent = 0, hits = 0, stageValidations = 0, stageEqual = 0, variantsCompared = 0, inserts = 0, variantsInserted = 0, variantsEvicted = 0, present = 0, unstable = 0, touches = 0, verifyHits = 0, verifyMismatches = 0;
    std::uint64_t partialStages = 0, dataPatchedStages = 0, stashed = 0, nestedMoved = 0, nestedRefused = 0;
    double learnUs = 0, relocateUs = 0, stageValidateUs = 0, patchUs = 0, insertUs = 0, syncUs = 0, overlapUs = 0, labelUs = 0, compareUs = 0;
    std::uint64_t validatedRuns = 0, validatedWords = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawMiss::Count)> misses{};
    std::array<std::uint64_t, MaxDispatchVariants> variantHitsByRank{};
    double validateUs = 0;

    std::uint64_t registerKeyLookups = 0, registerKeyHits = 0, decodeSkipped = 0, decodePartial = 0, facadeMismatches = 0, verifyDecodes = 0, verifyDecodeMismatches = 0;
    double keyUs = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

class DrawKeySightings {
public:
    static constexpr std::size_t Ways = 4;
    explicit DrawKeySightings(std::size_t keys);
    bool SeenBefore(std::uint64_t key);

private:
    std::vector<std::uint64_t> keys;
    std::vector<std::uint8_t> filled;
    std::vector<std::uint8_t> next;
    std::size_t bucketMask = 0;
};

struct DrawValidateSplit {
    bool timed = false;
    double syncUs = 0, overlapUs = 0, labelUs = 0, compareUs = 0;
};

DrawValidateSplit& ThreadDrawValidateSplit();

struct BindingPlanCounters {
    std::uint64_t draws = 0, decodeHits = 0, decodeMisses = 0, decodesPatched = 0, stages = 0, templateHits = 0, keyWords = 0, valueWords = 0, verifiedStages = 0, stageMismatches = 0, verifiedDecodes = 0, decodeMismatches = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

bool SameStageResult(const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b);
bool SameSnapshot(const ShaderRecompiler::ResourceSnapshot& a, const ShaderRecompiler::ResourceSnapshot& b);

enum class DrawVerdict { Drawn, Nothing, Rejected };

struct StageCapture {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
    std::vector<ShaderRecompiler::MemoryRegion> regions;
    std::uint64_t forgetSerial = 0;
    std::uint32_t pushOffset = 0;
    ShaderRecompiler::SrtUserDataUse userDataUse;
    std::shared_ptr<const ShaderRecompiler::SourceHandle> prepared;
    std::vector<ShaderRecompiler::BufferBaseWord> bufferBaseWords;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> dataLeaves;
    std::vector<std::uint64_t> otherReads;
    std::vector<std::uint32_t> inputExports;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> baseSlots;
};

struct RelocatableUserWord {
    std::uint32_t reg;
    std::vector<std::pair<std::int64_t, std::uint32_t>> reads;
    std::vector<std::int64_t> dataOffsets;
};

struct DrawRelocation {
    std::vector<RelocatableUserWord> words;
    std::vector<std::uint32_t> banned;
    std::vector<std::uint32_t> unread;
    std::vector<std::uint32_t> read;
};

struct DrawUserPatch {
    std::size_t program;
    std::uint32_t index, value;
};

struct DrawStructureParts {
    std::uint64_t shape = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> words;
};

struct DrawAliasKeys {
    static constexpr std::size_t Stages = 3;
    std::uint64_t structure = 0;
    std::array<std::uint64_t, Stages> content{};
    std::array<std::vector<std::uint32_t>, Stages> raw;
    DrawStructureParts parts;
    static std::size_t Stage(std::uint32_t reg) { return reg >= 0x10cu ? 2 : reg >= 0x08cu ? 1 : 0; }
    std::uint64_t Key(std::uint64_t stageKey, std::uint32_t userDataBase) const { return (stageKey ^ (content[Stage(userDataBase)] + 0x9e3779b97f4a7c15ull)) * 0x100000001b3ull + userDataBase; }
};

struct DrawStageOption {
    std::shared_ptr<DispatchVariant> variant;
    std::vector<std::uint32_t> userData;
    std::uint64_t content = 0;
};

struct DrawStageDonor {
    static constexpr std::size_t Options = 4, Recent = 8;
    std::vector<DrawStageOption> options;
    std::array<std::uint64_t, Recent> recent{};
    std::size_t next = 0;
    bool cycling = false;
    std::uint64_t Bytes() const;
};

std::uint64_t DrawStageContent(const DispatchVariant& variant);

struct DrawDecodeDonor {
    std::shared_ptr<const DrawDecode> decode;
    std::vector<std::uint64_t> stageKeys;
};

template <typename TValue>
class RecentTable {
public:
    explicit RecentTable(std::size_t capacity) : capacity(capacity) {}
    TValue* Find(std::uint64_t key) {
        const auto found = entries.find(key);
        if (found == entries.end()) return nullptr;
        order.splice(order.begin(), order, found->second.second);
        return &found->second.first;
    }
    bool Put(std::uint64_t key, TValue value, std::uint64_t* evicted = nullptr) {
        if (const auto found = entries.find(key); found != entries.end()) {
            found->second.first = std::move(value);
            order.splice(order.begin(), order, found->second.second);
            return false;
        }
        while (!entries.empty() && entries.size() >= capacity) {
            if (evicted != nullptr) *evicted = order.back();
            entries.erase(order.back());
            order.pop_back();
        }
        order.push_front(key);
        entries.emplace(key, std::make_pair(std::move(value), order.begin()));
        return true;
    }
    std::size_t Size() const { return entries.size(); }
    template <typename TVisit>
    void ForEach(TVisit&& visit) const {
        for (const auto& [key, held] : entries) visit(held.first);
    }

private:
    std::size_t capacity;
    std::list<std::uint64_t> order;
    std::unordered_map<std::uint64_t, std::pair<TValue, std::list<std::uint64_t>::iterator>> entries;
};

struct DrawRelocationPending {
    std::uint64_t key = 0, identity = 0;
    DrawAliasKeys aliases;
    std::shared_ptr<const DrawRelocation> relocation;
    std::shared_ptr<const DrawDecode> decode;
    std::vector<std::shared_ptr<DispatchVariant>> stages;
};

struct DrawRelocationTrial {
    static constexpr std::uint32_t Window = 1024, MinimumHitShare = 16, IdleDraws = 16384;
    std::uint32_t lookups = 0, hits = 0, idle = 0;
    bool productive = false;
};

enum class DrawKeyMiss : std::size_t { Unlearned, Idle, Unreadable, NewStructure, RegisterState, ValueWord, ReadWord, OtherWord, StructureDropped, Relocated, Count };

enum class DrawStageMiss : std::size_t { NoStageKey, DonorEvicted, NoDonor, ShapeChanged, DonorDropped, Refused, BufferBase, BufferWord, ImageWord, FlatWord, ShaderData, Constant, Unbound, Count };

struct DrawStageSample {
    std::vector<std::uint32_t> raw;
    std::shared_ptr<const DispatchVariant> variant;
};

struct DrawMissDiagnosis {
    static constexpr std::size_t Samples = 4096;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawKeyMiss::Count)> keys{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrawStageMiss::Count)> stages{};
    std::uint64_t wordAddressLike = 0, contentAddressLike = 0, oneWord = 0, fewWords = 0, manyWords = 0;
    std::uint64_t donorLookups = 0, donorFound = 0, donorPuts = 0, donorNew = 0, donorEvictions = 0, donorBytes = 0, donorSame = 0, donorRecurring = 0, donorFresh = 0, donorOptionsTried = 0, donorAlternateHits = 0;
    std::uint64_t validationMisses = 0, validationNone = 0, validationUnreadable = 0, validationRelocatedRun = 0, validationAbsoluteRun = 0, validationDataWord = 0, validationAddressLike = 0, validationOneWord = 0, validationFewWords = 0, validationManyWords = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrawStageMiss::Count)> validationRoles{};
    std::uint64_t validationDataWords = 0, validationFlatWords = 0, validationOtherWords = 0, validationOnlyData = 0;
    std::uint64_t stagesTagged = 0, leavesOffered = 0, leavesTagged = 0, leavesUnmapped = 0, leavesMismatched = 0, leavesAliased = 0, baseSlotsOffered = 0, nestedTagged = 0, nestedRuns = 0;
    RecentTable<bool> evictedDonors{Samples * 16};
    std::unordered_map<std::uint64_t, DrawStructureParts> learned;
    RecentTable<DrawStageSample> samples{Samples};
    static std::uint64_t SampleKey(std::uint64_t structure, std::size_t stage) { return (structure ^ (0x9e3779b97f4a7c15ull * (stage + 1))) * 0x100000001b3ull; }
    static bool AddressLike(std::uint32_t before, std::uint32_t after);
    static DrawStageMiss Classify(const DispatchVariant& variant, std::uint32_t before, std::uint32_t after);
};

struct DrawRelocationCounters {
    std::uint64_t lookups = 0, donors = 0, refused = 0, hits = 0, unknownStructure = 0, missingStage = 0, unreadMoves = 0;
};

std::uint64_t UserDataReads(const ShaderRecompiler::SrtUserDataUse& use, const ShaderRecompiler::RecompileResult& compiled, bool decodeFree, std::uint32_t hiddenWords);

bool DecodeProgramsCurrent(const DrawDecode& decode, const ShaderRegistry& registry);

}

#endif

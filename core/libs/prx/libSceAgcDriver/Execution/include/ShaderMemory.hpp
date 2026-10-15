#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERMEMORY_HPP

#include "Recompiler.hpp"
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
struct SourceHandle;
class PreparedShaderInvocation;
struct IrResourcePlan;
struct ResourceSnapshot;
struct SrtTapeInstance;
}

namespace AgcDriver {

// Records the guest words a shader's resource analysis reads, as the memory regions its recompile
// request carries. Guest memory is fetched a 4 KiB page at a time; the regions report exactly the
// dwords read, so cache keys built from them do not change with unrelated bytes nearby. A page that
// `pendingWrite` reports as written by recorded GPU work is read word by word instead, so only a
// dword the capture needs waits for that work through the flush hook, not the whole page; a dword
// the query knows to be left unchanged by that work (Driver.cpp's written-element evidence) is read
// raw, without the hook. Debug aid: APS5_NO_WORDWISE_CAPTURE=1 fetches every page whole.
class ShaderMemory {
public:
    // The driver's answer for a range: nothing recorded writes it; recorded work writes it and the
    // read must go through the flush hook; recorded work writes it but leaves it unchanged, so the
    // bytes are read raw; VerifyRaw reads raw and through the hook, counting a difference;
    // KnownValue: recorded work writes it with bytes the driver already knows (a copy HLE's
    // destination, Driver.cpp's known-value ring entries), copied into `known` when the caller
    // passes a span of exactly `bytes` (empty: the answer alone); VerifyKnownValue serves them and
    // reads through the hook too, counting a difference.
    enum class PendingWrite : std::uint8_t { None, Sync, Raw, VerifyRaw, KnownValue, VerifyKnownValue };
    using PendingWriteQuery = PendingWrite (*)(std::uint64_t address, std::size_t bytes, std::span<std::byte> known);
    // Words served from known values by every capture, and the verified ones and their mismatches
    // (APS5_VERIFY_KNOWN_VALUES=1), for Driver.cpp's [copy] line.
    struct KnownValueCounts {
        std::uint64_t served, verified, mismatches;
    };
    static KnownValueCounts KnownValues();
    static bool UsesImageTables(const ShaderRecompiler::IrResourcePlan& plan);
    // Told, for a dword read that went through the hook, whether the bytes read before the hook's
    // wait were still there after it (the driver's evidence for later raw reads).
    using PendingWriteObserver = void (*)(std::uint64_t address, bool unchanged);
    // The calling thread's count of hook syncs that waited for unfinished GPU work: a read is only
    // observed when the count moved across it (against finished work both reads see the GPU's
    // bytes, and "unchanged" would be no evidence). Null observes every read that went through
    // the hook.
    using HookWaitCounter = std::uint64_t (*)();
    // The calling thread's GPU waits so far in milliseconds (Recorder::ThreadWaitedMs), set once
    // by the driver: the [capture] line charges the waits made inside a capture to it.
    using WaitedMsProvider = double (*)();
    static void SetWaitedMsProvider(WaitedMsProvider provider);
    using PendingGenerationProvider = std::uint64_t (*)();
    static void SetPendingGenerationProvider(PendingGenerationProvider provider);
    struct DirectReadCounts {
        std::uint64_t captures, words, hookedWords, pageStateLookups, pageStateMisses;
    };
    static DirectReadCounts DirectReads();
    explicit ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> initial, PendingWriteQuery pendingWrite = nullptr, PendingWriteObserver observe = nullptr, HookWaitCounter hookWaits = nullptr);
    ShaderMemory(const ShaderMemory&) = delete;
    ShaderMemory& operator=(const ShaderMemory&) = delete;
    ~ShaderMemory();
    // Returns what the capture resolved (plan, snapshot, specialization) for
    // ShaderRecompiler::Recompile(request, capture), which then skips its own materialization. One
    // result per call: the draw path captures several stages on one ShaderMemory. With `handle`
    // (the driver's memoized ShaderRecompiler::ResolveSource result) the capture skips the source
    // resolution.
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle = nullptr);
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> Capture(const ShaderRecompiler::PreparedShaderInvocation& invocation);
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> CaptureValues(const ShaderRecompiler::PreparedShaderInvocation& invocation);
    struct IncrementalState {
        std::shared_ptr<ShaderRecompiler::SrtTapeInstance> instance;
        std::shared_ptr<const void> plan;
        std::uint64_t generation = 0;
        std::uint64_t allocation = 0;
        std::vector<std::uint64_t> pages;
    };
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> CaptureValues(const ShaderRecompiler::PreparedShaderInvocation& invocation, IncrementalState& state);
    std::vector<std::pair<std::uint64_t, std::uint32_t>> TakeKeptWords();
    enum class PlanFallback : std::uint8_t { None, First, Inputs, Memory, Unrecorded, ImageTables, Specialization, Count };
    static constexpr std::size_t PlanFallbacks = static_cast<std::size_t>(PlanFallback::Count);
    static constexpr const char* PlanFallbackNames[PlanFallbacks] = {"none", "first evaluation", "root user data changed", "read range written, pending or remapped", "previous evaluation not recorded", "image tables", "specialization words changed"};
    enum class PlanOutcome : std::uint8_t { Reused, Patched, Reevaluated, Full, Count };
    static constexpr std::size_t PlanOutcomes = static_cast<std::size_t>(PlanOutcome::Count);
    static constexpr const char* PlanOutcomeNames[PlanOutcomes] = {"reused", "reused with data user words patched", "re-evaluated by node", "full capture"};
    struct PlanInstance {
        IncrementalState nodes;
        std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
        std::shared_ptr<const ShaderRecompiler::ResourceCapture> values;
        std::shared_ptr<const ShaderRecompiler::ResourceCapture> latest;
        std::shared_ptr<const ShaderRecompiler::IrResourcePlan> rootsPlan;
        std::vector<std::uint32_t> roots;
        bool allRoots = true;
        std::vector<std::uint32_t> userData;
        std::uint64_t shaderBase = 0;
        std::uint64_t forgetSerial = 0;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
    };
    struct PlanCapture {
        std::shared_ptr<const ShaderRecompiler::ResourceCapture> values;
        PlanOutcome outcome = PlanOutcome::Full;
        PlanFallback fallback = PlanFallback::None;
        std::uint64_t nodesKept = 0;
        std::uint64_t nodesFresh = 0;
    };
    PlanCapture CapturePlanned(const ShaderRecompiler::PreparedShaderInvocation& invocation, PlanInstance& instance, bool keepReads);
    static bool PlanReadsCurrent(const PlanInstance& instance);
    static bool SameSpecializationWords(const ShaderRecompiler::ResourceSnapshot& a, const ShaderRecompiler::ResourceSnapshot& b);
    static void PlanRoots(const ShaderRecompiler::IrResourcePlan& plan, std::vector<std::uint32_t>& roots, bool& allRoots);
    [[nodiscard]] std::vector<ShaderRecompiler::MemoryRegion> Regions() const;
    // The page regions read since the previous call (or construction), a word read again
    // included, the initial regions excluded: one stage's own reads on the draw path's shared
    // ShaderMemory (Regions() stays the union). The spans point into the pages, as Regions()'s do.
    [[nodiscard]] std::vector<ShaderRecompiler::MemoryRegion> TakeRecentRegions();
    // The driver's source handle memo outcomes, for the [capture] line (APS5_PROFILE_DRAW).
    static void CountHandleMemo(bool hit);
    static std::uint64_t MemoizedCaptures();
    struct RecordedAccess {
        std::uint64_t address;
        std::uint64_t bytes;
        std::uint32_t value;
    };
    struct ReplayPlan {
        std::span<const std::pair<std::uint64_t, std::uint32_t>> dataLeaves;
        std::span<const std::pair<std::uint64_t, std::uint32_t>> baseWords;
        std::span<const std::uint64_t> accessBases;
    };
    struct ReplayResult {
        std::vector<std::pair<std::size_t, std::uint32_t>> changed;
        std::vector<std::pair<std::uint64_t, std::int64_t>> moved;
        std::vector<std::uint64_t> addresses;
        std::uint64_t mismatch = 0;
    };

private:
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle, const ShaderRecompiler::PreparedShaderInvocation* invocation, bool valuesOnly = false);
    static constexpr std::size_t PageBytes = 4096;
    static constexpr std::size_t PageWords = PageBytes / sizeof(std::uint32_t);

    struct Page {
        std::array<std::uint32_t, PageWords> words{};
        std::bitset<PageWords> valid;
        std::bitset<PageWords> read;
        std::bitset<PageWords> recent;
        bool wordwise = false;
    };

    static bool read(void* context, std::uint64_t address, std::uint32_t* value);
    static bool readWords(void* context, std::uint64_t address, std::uint32_t* values, std::uint64_t count);
    static bool accessible(void* context, std::uint64_t address, std::uint64_t bytes);
    static bool pending(void* context, std::uint64_t address, std::uint64_t bytes);
    static bool stable(void* context, std::uint64_t address, std::uint32_t word);
    bool stableDirect(std::uint64_t address, std::uint32_t word);
    bool pageUnchanged(std::uint64_t base);
    void notePage(std::uint64_t base);
    void recordDirect(std::uint64_t address, std::uint32_t word);
    void enableDirect();
    Page& page(std::uint64_t base);
    std::uint32_t fetchWord(std::uint64_t address, bool wordwise);
    bool readDirect(std::uint64_t address, std::uint32_t* value);
    bool probeDirect(std::uint64_t address, std::uint64_t end);
    std::uint8_t directPageKind(std::uint64_t base, std::uint64_t allocation, PendingGenerationProvider provider, std::uint64_t pending);
    std::vector<ShaderRecompiler::MemoryRegion> directRegions(std::span<const std::uint32_t> indices, bool withInitial) const;
    struct DirectRead {
        std::uint64_t address;
        std::uint32_t word;
    };
    bool replay(std::span<const RecordedAccess> accesses, const ReplayPlan* plan = nullptr, ReplayResult* result = nullptr);
    PlanFallback planFallback(const PlanInstance& instance, std::span<const std::uint32_t> userData, std::uint64_t shaderBase) const;
    bool probe(std::uint64_t address, std::uint64_t bytes);
    [[nodiscard]] bool overlapsInitial(std::uint64_t begin, std::uint64_t end) const;

    // Regions given at construction (the registered shader's code and header), referenced as given:
    // the caller keeps them alive for as long as the capture is used.
    std::map<std::uint64_t, std::span<const std::byte>> initial;
    std::map<std::uint64_t, Page> pages;
    PendingWriteQuery pendingWrite = nullptr;
    PendingWriteObserver observe = nullptr;
    HookWaitCounter hookWaits = nullptr;
    std::vector<RecordedAccess>* recording = nullptr;
    std::uint64_t pageSetupNanoseconds = 0;
    std::uint64_t pageReadNanoseconds = 0;
    std::uint64_t readCount = 0;
    std::uint64_t pageCount = 0;
    std::uint64_t initialLow = ~std::uint64_t{0};
    std::uint64_t initialHigh = 0;
    std::uint64_t lastBase = ~std::uint64_t{0};
    Page* lastPage = nullptr;
    bool direct = false;
    std::uint64_t instanceSerial = 0;
    std::vector<DirectRead> directReads;
    std::vector<std::uint32_t> directIndex;
    std::vector<std::uint32_t> recentReads;
    mutable std::vector<std::unique_ptr<std::uint32_t[]>> regionStorage;
    std::uint64_t directWords = 0;
    std::uint64_t directHooked = 0;
    std::uint64_t directLookups = 0;
    std::uint64_t directMisses = 0;
    std::uint64_t lastDirectBase = ~std::uint64_t{0};
    std::uint64_t lastDirectAllocation = 0;
    std::uint64_t lastDirectPending = 0;
    PendingGenerationProvider lastDirectProvider = nullptr;
    std::uint8_t lastDirectKind = 0;
    ShaderRecompiler::SrtTapeInstance* tapeInstance = nullptr;
    std::uint64_t stableSince = 0;
    std::uint64_t stableAllocation = 0;
    std::vector<std::pair<std::uint64_t, bool>> stablePages;
    std::vector<std::uint64_t> runPages;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> keptWords;
};

// The positions, among a dispatch-cache variant's stored words, of the pure flat-SRT leaves a
// capture read (ShaderRecompiler::SrtReadTrace), for a hit that differs only there. `runs` are the
// variant's [begin, end) byte ranges in address order and `words` their dwords in that order;
// `leaves` are (flat offset, address) and `otherReads` the sorted addresses of every other read.
// A leaf is skipped when its address is among the other reads (a walk read aliases the dword,
// counted `aliased`), is not dword-aligned or lies in no run (counted `unmapped`), or its stored
// word differs from the flattened SRT's at that offset (counted `mismatched`). `positions`
// (sorted, may repeat) and `slots` (the flat offset of each) are parallel.
struct DataWordPositionCounts {
    std::uint64_t unmapped = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t aliased = 0;
};
DataWordPositionCounts DataWordPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads, std::span<const std::uint32_t> words, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots);

}

#endif

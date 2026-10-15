#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRIVER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRIVER_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/PacketHistory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchRelocation.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/PrepareWorkers.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CacheSizing.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferCopy.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/IndirectDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/WriteEvidence.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/PacketTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Coherence.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueScheduler.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
struct SnapshotWordCounts;
}

namespace AgcDriver::DriverDetail {

class Driver {
public:
    static Driver& Get();
    void ParkPacket(ParkedOp op);
    QueueScheduler& Scheduler() { return queueScheduler; }
    static std::uint64_t GpuMapImportThreads();
    ~Driver();
    void Shutdown();
    void WaitIdle();
    void CheckFailure();
    void ReportFailure(std::exception_ptr error);
    void Submit(const Packet* packet, std::uint32_t queue);
    void SuspendPoint();
    std::uint32_t ReserveEopFlip(const FlipInfo& info);
    void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
    void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
    void AttachWindow(const PresentationWindow& window);
    void Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque, void (*gpuReady)(void*), void* context);
    void ReleaseWindow(void* window);
    void RegisterShader(const Shader* shader);
    void ResolveShaderAbi(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive);
    void ResolveGraphicsStagesAbi(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive);
    void ResolveGraphicsAbi(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType);
    void SettleShaderPreparations(std::span<const Shader* const> stages);
    static void FoldDrawOffsets(const ShaderRecompiler::RecompileResult& result, const DrawProgram& program, Pm4::DrawParameters& parameters);
    static std::optional<Graphics::IndirectDrawPath> ClassifyIndirectDraw(const ShaderRecompiler::RecompileResult& result, const Graphics::State& graphics, const DrawProgram& frontProgram, const std::shared_ptr<VulkanDevice>& localDevice, Pm4::DrawParameters& drawParameters, bool traceIndirect);
    static std::string dumpRequest(std::uint64_t address, const ShaderRecompiler::RecompileRequest& request);
    DrawEntryCounters DrawCacheCounters();
    BindingPlanCounters PlanCounters();
    DispatchCacheTotals DispatchCacheCounters();
    struct DispatchCacheFootprint {
        std::size_t entries = 0;
        std::uint64_t variantBytes = 0;
    };
    DispatchCacheFootprint DispatchCacheSize();
    static std::uint64_t DispatchCacheBytes();

private:
    friend class SampledReadScope;
    friend struct PendingView;
    friend struct PacketTimer;
    void stop();
    Driver();
    void rethrowFailure() const;
    void checkStopping() const;
    static bool& onWorkerThread();
    static void copyCommands(Submission& submission, const std::uint32_t* guest, std::size_t words);
    static bool copySegment(Submission& submission, const std::uint32_t* guest, std::size_t words, std::size_t& budget);
    static void readRegisterLists(Submission& submission);
    void waitForFlipRoom(const Submission& submission);
    void holdFlipBehindWorker(const Submission& submission);
    bool awaitsTitle(std::uint64_t awaited) const;
    void noteAwaitingTitle(std::uint32_t queue, std::uint64_t awaited);
    void releaseFlipHold();
    void reserveOutputs(Submission& submission);
    void executeRewindTail(const Submission& stalled);
    void enqueue(Submission submission);
    void noteHeldAtSubmit(Submission& submission, std::size_t cursor);
    static void forgetUnfinishedWrites(QueueWorker& worker, const Submission& submission);
    static bool waitFree(const Submission& submission);
    bool queue0Before(std::uint64_t received) const;
    bool orderReleased(std::uint32_t queue, std::uint64_t received) const;
    void noteWaitBlocked(std::uint32_t queue, std::uint64_t awaited, bool blocked);
    void reportPresents(double waitedMs, std::size_t inFlight);
    static bool stampValidate();
    static bool dataHits();
    static bool verifyDataHits();
    static std::size_t dispatchVariants();
    static bool insertCompare();
    static std::size_t dispatchCacheEntries();
    static Graphics::CacheSizing::Limits dispatchCacheLimits();
    static Graphics::CacheSizing::Limits drawCacheLimits();
    void evictDispatchEntries();
    void evictDrawEntries();
    static std::uint64_t variantBytes(const DispatchVariant& variant);
    void accountVariant(const DispatchVariant& variant, bool added);
    void eraseDispatchEntry(std::unordered_map<std::uint64_t, std::shared_ptr<DispatchEntry>>::iterator it);
    void classifyDiffering(std::uint64_t program, std::uint64_t key, const DispatchVariant& old, const DispatchVariant& fresh, const ShaderRecompiler::ResourceCapture* capture, EntryCounters& counters);
    void reportDispatchCache(EntryCounters& counters);
    void lookupDispatch(std::uint64_t address, const Submission& submission, std::uint64_t key, const DispatchKeyParts& parts, const std::shared_ptr<const ShaderSnapshot>& registeredShader, bool noDispatchCache, bool traceCache, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, DispatchPhaseTiming& phaseTiming, std::array<double, DriverPhaseCount>& phaseMs, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit, bool& cached, bool& validated, std::shared_ptr<DispatchEntry>& missedEntry, bool& missedDiffering);
    void insertDispatch(std::uint64_t address, std::uint64_t key, const DispatchKeyParts& parts, bool noDispatchCache, bool profile, const std::shared_ptr<const ShaderSnapshot>& registeredShader, std::uint64_t forgetAtCapture, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::shared_ptr<ShaderMemory>& shaderMemory, const std::vector<ShaderRecompiler::MemoryRegion>& captured, const std::shared_ptr<const ShaderRecompiler::ResourceCapture>& capture, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, const std::shared_ptr<DispatchEntry>& missedEntry, bool missedDiffering, std::shared_ptr<DispatchVariant>& attachVariant, DispatchPhaseTiming& phaseTiming);
    void dispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments = 0);
    void dispatchNow(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments);
    void storeDispatchVariant(std::uint64_t key, std::shared_ptr<DispatchVariant> fresh, const std::shared_ptr<DispatchEntry>& missedEntry, std::shared_ptr<DispatchVariant>& attachVariant);
    static bool dispatchRelocationEnabled();
    static bool sameResourceBindings(const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b);
    static std::uint64_t userDataCallWords(std::span<const std::uint32_t> code, std::size_t userWords);
    static bool dispatchMissDiagnosisEnabled();
    static std::uint64_t dispatchStructureKey(const DispatchKeyParts& parts, const DispatchShape& shape);
    void learnDispatchDonor(const DispatchKeyParts& parts, const std::shared_ptr<DispatchVariant>& variant);
    void noteDispatchMiss(DispatchMiss reason);
    void diagnoseDispatchIdentity(const DispatchKeyParts& parts);
    void diagnoseDispatchWords(const DispatchKeyParts& parts, const DispatchShape& shape, std::uint64_t structure);
    void diagnoseDispatchContent(const DispatchVariant& relocated);
    static std::shared_ptr<DispatchVariant> relocatedDispatchVariant(const DispatchDonor& donor, std::span<const std::uint32_t> userData, std::uint64_t& moves);
    static void patchDispatchShaderData(DispatchVariant& variant);
    bool relocateDispatch(std::uint64_t address, const Submission& submission, std::uint64_t key, const DispatchKeyParts& parts, const std::shared_ptr<const ShaderSnapshot>& registeredShader, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::shared_ptr<DispatchEntry>& missedEntry, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit);
    void reportDispatchRelocation();
    void verifyDataHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const std::uint32_t> liveWords, const ShaderRecompiler::RecompileResult& patched);
    static bool drawEntries();
    static bool drawDataHits();
    static bool verifyDrawDataHits();
    static bool drawStageDataHits();
    static bool bindingPlan();
    static bool verifyBindingPlan();
    void planDrawDecode(const QueueState& queue, const Submission& submission, std::uint64_t deviceSerial, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches, bool profile);
    void notePlannedStage(bool verified, bool mismatch, SnapshotPart part, bool templateHit, ShaderRecompiler::SnapshotWordCounts words);
    static bool incrementalBindings();
    static bool verifyIncrementalBindings();
    static ShaderMemory::IncrementalState& incrementalBinding(const std::shared_ptr<const ShaderRecompiler::SourceHandle>& handle, std::span<const std::uint32_t> userData);
    void noteIncrementalStage(const ShaderRecompiler::SrtTapeInstance& instance, bool verified, bool stale);
    static bool incrementalPlans();
    static bool verifyIncrementalPlans();
    static ShaderMemory::PlanInstance& resourcePlanInstance(const std::shared_ptr<const ShaderRecompiler::SourceHandle>& handle, std::uint32_t pushOffset, std::span<const std::uint32_t> userData);
    void notePlanInstance(const ShaderMemory::PlanCapture& capture, bool verified, bool stale);
    static std::uint64_t drawDecodeKey(const QueueState& queue, const ShaderRegistry& registry, std::uint64_t deviceSerial);
    void reportBindingPlan();
    static bool verifyDrawEntries();
    static bool registerKeyEnabled();
    static bool verifyDrawRecipe();
    static std::size_t drawCacheEntries();
    void accountDrawVariant(const DispatchVariant& variant, bool added);
    void insertDrawEntry(std::uint64_t key, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::shared_ptr<const DrawDecode> decode, std::uint64_t shape = 0);
    bool admitDrawKey(std::uint64_t key, std::uint64_t shape);
    void eraseDrawEntry(std::uint64_t key, const std::shared_ptr<DrawEntry>& expected);
    std::shared_ptr<const DrawRecipe> findDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages);
    void attachDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages, std::shared_ptr<const DrawRecipe> recipe);
    static bool traceDrawCache();
    static void traceInsert(std::unordered_set<std::uint64_t>& set, std::uint64_t key);
    void maybeReportDrawCache(bool profile);
    void reportDrawCache(DrawEntryCounters& counters, bool profile);
    std::string profileDrawCacheLines(const DrawEntryCounters& counters);
    static std::uint64_t drawRegisterKey(const QueueState& queue, const ShaderRegistry& registry, std::uint64_t deviceSerial, std::uint64_t* shape = nullptr, const DrawRelocation* relocation = nullptr, DrawAliasKeys* aliases = nullptr);
    static std::uint64_t drawIdentityKey(const QueueState& queue, const ShaderRegistry& registry);
    static bool drawRelocationEnabled();
    static std::uint64_t relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable, std::vector<std::uint32_t>* raw = nullptr);
    static bool drawMissDiagnosisEnabled();
    static bool relocatedOnlyEntries();
    static DataWordPositionCounts tagDataLeaves(DispatchVariant& variant, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads);
    void noteDrawKeyMiss(DrawKeyMiss reason);
    void diagnoseStructureMiss(const DrawAliasKeys& aliases);
    void diagnoseStageMiss(const DrawAliasKeys& aliases, std::size_t stage, std::uint32_t userDataBase, std::uint64_t donorKey, bool refused);
    void reportDrawMisses();
    void diagnoseValidationMiss(const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions);
    void putStageDonor(std::uint64_t key, const std::shared_ptr<DispatchVariant>& variant, const std::vector<std::uint32_t>& userData, std::uint64_t* evicted, bool* added, bool count);
    static void tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, bool eligible, std::uint32_t hiddenWords, std::span<const std::pair<std::uint32_t, std::uint64_t>> baseSlots = {});
    static std::shared_ptr<DispatchVariant> followNestedBases(const DispatchVariant& variant, bool& refused);
    static void applyShaderDataPatches(DispatchVariant& variant);
    static std::shared_ptr<DispatchVariant> cloneVariant(const DispatchVariant& variant);
    static void tagBufferWords(DispatchVariant& variant, std::span<const ShaderRecompiler::BufferBaseWord> bufferBaseWords);
    static std::shared_ptr<DispatchVariant> patchBufferWords(const DispatchVariant& variant, std::span<const std::pair<std::uint32_t, std::uint32_t>> live);
    void learnPendingRelocation();
    void relocateDrawEntry(const QueueState& queue, const DrawAliasKeys& aliases, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches);
    void reportDrawRelocation();
    static bool sameVertexInfo(const ShaderRecompiler::ShaderVertexStageInfo& a, const ShaderRecompiler::ShaderVertexStageInfo& b);
    static bool sameDecode(const DrawDecode& a, const DrawDecode& b);
    std::shared_ptr<DrawDecode> decodeDraw(const QueueState& queue, const Submission& submission);
    void resolveDrawDecode(const QueueState& queue, const Submission& submission, std::shared_ptr<const DrawDecode>& decode, bool registerKey, std::uint64_t drawKey, bool profile);
    void lookupDraw(const Submission& submission, const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, bool useDrawEntries, bool registerKey, bool profile, std::uint64_t& drawKey, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool& drawHit, bool& verifyHit, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, const std::shared_ptr<DrawEntry>& dataEntry, DrawDataCandidates& dataCandidates, std::vector<StageDataHit>& stageData);
    std::shared_ptr<const ShaderRecompiler::RecompileResult> materializeDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, std::string& rejected);
    static void recordStageDataPositions(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture, const std::vector<Graphics::DecodeRead>& decodeReads);
    void cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::uint64_t drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming, const std::vector<bool>& reused, const std::shared_ptr<DrawEntry>& entry, std::uint64_t shapeKey);
    static bool drawPrecheck();
    std::optional<DrawVerdict> precheckDraw(const QueueState& queue, const Submission& submission, std::span<const std::uint32_t> packet, const Pm4::DrawParameters& drawParameters, std::string& rejected, bool& traceIndirect);
    static std::uint32_t drawUserWord(const DrawProgram& program, std::int32_t sgpr);
    DrawVerdict draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::string& rejected);
    DrawVerdict drawNow(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::string& rejected);
    struct DrawRecordResult;
    struct DrawRecordState {
        bool active = false;
        std::uint64_t overlapped = 0;
        bool settled = false;
        bool stale = false;
    };
    static DrawRecordState& drawRecordState();
    static bool recordThread();
    static std::uint32_t prepareAhead();
    static void runDrawRecords();
    bool deferDrawRecords(std::uint32_t queue) const;
    static bool recordDispatches();
    bool deferDispatchRecords(std::uint32_t queue) const;
    void deferRecord(const std::function<void()>& prepare);
    void deferAhead(const std::function<void()>& prepare);
    static std::string recordIdentity(const Submission& submission, const char* what, std::uint64_t program);
    bool settleDrawRecords(std::span<const Graphics::GuestMemorySnapshot> reads, bool drain = false);
    bool settleAhead(std::span<const Graphics::GuestMemorySnapshot> reads, bool drain);
    void finishDrawRecords();
    void waitDrawRecords();
    void waitDrawRecordsAt(std::uint32_t opcode);
    bool overlapCommandProcessorRead(std::span<const std::uint32_t> packet);
    void postDrawRecord(const std::shared_ptr<VulkanDevice>& localDevice, std::uint32_t queue, std::string identity, std::function<void(DrawRecordResult&)> record);
    static void noteDrawRecordRecipe(DrawRecordResult& result, std::shared_ptr<const DrawRecipe> built, std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages);
    void reportDrawRecords();
    struct DrawInputs;
    struct DispatchInputs;
    struct FrozenDraw;
    struct PreparedFrozenDraw;
    struct FrozenSink {
        bool inOrder = false;
        FrozenFallback refused = FrozenFallback::None;
        std::shared_ptr<DrawInputs> inputs;
        std::shared_ptr<DispatchInputs> dispatchInputs;
        std::vector<PreparedRead> reads;
        std::uint64_t program = 0;
        bool Refuse(FrozenFallback reason) {
            if (inOrder) return false;
            refused = reason;
            return true;
        }
    };
    using FrozenPipeline = OrderedPrepare<std::shared_ptr<FrozenDraw>, std::shared_ptr<PreparedFrozenDraw>>;
    static std::uint32_t prepareWorkers();
    static bool parallelResolve();
    static std::uint32_t frozenBound();
    static FrozenPipeline& frozenDraws();
    static FrozenSink*& frozenSink();
    static bool& frozenProducer();
    static void startDrawRecords();
    static void noteRecordHistory(std::uint64_t ticket, const std::shared_ptr<DrawRecordResult>& result);
    static void recordDrawInputs(const DrawInputs& inputs, DrawRecordResult& result, const std::shared_ptr<VulkanDevice>& localDevice);
    void noteDrawInputWriters(const DrawInputs& inputs, std::uint32_t queue);
    FrozenFallback frozenFallback(std::span<const std::uint32_t> packet);
    DrawVerdict freezeDraw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission);
    static bool prepareDispatches();
    static bool verifyPrepareDispatches();
    static FrozenFallback dispatchHleFallback(const Registers& shader, std::span<const std::uint32_t> packet, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, std::uint64_t indirectArguments);
    FrozenFallback dispatchFrozenFallback(const QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments);
    void freezeDispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments, std::chrono::steady_clock::time_point begin);
    static void recordDispatchInputs(DispatchInputs& inputs, const std::shared_ptr<VulkanDevice>& localDevice);
    void noteDispatchInputWriters(const DispatchInputs& inputs, std::uint32_t queue);
    static bool staleDispatchInputs(const DispatchInputs& adopted, const DispatchInputs& fresh, const std::function<bool(std::uint64_t, std::size_t)>& touchedEarlier);
    std::shared_ptr<PreparedFrozenDraw> prepareFrozen(const FrozenDraw& frozen, bool inOrder);
    void emitFrozen(const std::shared_ptr<FrozenDraw>& frozen, std::optional<std::shared_ptr<PreparedFrozenDraw>>& prepared, std::exception_ptr error);
    void drainFrozen(bool records);
    void mergeFrozenRecords();
    void addDriverPhases(DispatchClass which, const std::array<double, DriverPhaseCount>& ms, bool hit, bool validated);
    static PendingDispatchPhases& pendingDispatchPhases();
    static std::chrono::steady_clock::time_point& packetStartedAt();
    static PendingDrawPhases& pendingDrawPhases();
    void addDrawPhases(const std::array<double, DrawDriverPhaseCount>& ms, bool drawn, std::uint64_t captures);
    void noteLabelStore(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue);
    std::optional<StoredMatch> storedSince(std::span<const std::uint32_t> packet, std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp);
    static void traceLabel(std::span<const std::uint32_t> packet, std::uint32_t queue);
    std::string describeWait(std::span<const std::uint32_t> packet);
    static std::string describeObservedChain(std::uint64_t target);
    static std::array<WriteRecord, 16384>& writeHistory();
    static std::size_t& writeCursor();
    static void dumpPackets(std::span<const std::uint32_t> commands, const std::uint32_t* guest = nullptr);
    static void validate(const Submission& submission, const std::uint32_t* guest = nullptr);
    static void reportSkip(const char* kind, const std::string& what);
    static bool matchesFillKernel(std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute);
    static bool fillClearEnabled();
    static bool fillClearExactOnly();
    static void fillClearCount(const Graphics::StorageTexture::FillCoverage& coverage, std::size_t bytes, std::span<const std::uint32_t, 4> pattern, std::size_t discarded, bool cleared, const char* refusal);
    void storeBufferFill(std::uint32_t queueId, std::uint64_t base, std::size_t bytes, const std::array<std::uint32_t, 4>& pattern, const std::shared_ptr<VulkanDevice>& localDevice, bool memoryValueFill, bool noteMetadata);
    bool fillBuffer(QueueState& queue, std::uint32_t queueId, std::span<const std::uint32_t> packet, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, const std::shared_ptr<VulkanDevice>& localDevice);
    static bool matchesCopyKernel(std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute);
    static void countCopy(int path, std::size_t bytes, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point locked, const VulkanDevice::CopyOutcome* outcome = nullptr);
    void noteCopyWriter(std::uint64_t program, std::uint64_t begin, std::uint64_t end, std::uint32_t queue, std::span<const std::byte> value = {}, std::uint64_t generation = 0);
    static bool copyKnownValues();
    static bool knownValueVerify();
    bool copyBuffer(QueueState& queue, std::uint32_t queueId, std::span<const std::uint32_t> packet, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, const std::shared_ptr<VulkanDevice>& localDevice, std::uint64_t programAddress);
    void traceCopyRefused(const VulkanDevice::CopyOutcome& outcome, std::uint64_t source, std::uint64_t destination, std::size_t bytes, const std::shared_ptr<VulkanDevice>& localDevice);
    static bool traceCopy();
    void traceCopyPending(const char* what, std::uint64_t address, std::size_t bytes, std::uint64_t source, std::uint64_t destination, std::size_t copyBytes, const std::shared_ptr<VulkanDevice>& localDevice);
    static void countIndirect(int path, double readMs);
    void dispatchIndirect(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission);
    static bool& sampledRead();
    static bool writeEvidenceEnabled();
    static bool writerKeyedEvidence();
    static bool foreignWriters();
    static bool observationGuard();
    static bool siteSampling();
    static std::uint64_t hookWaits();
    static ShaderMemory::HookWaitCounter hookWaitCounter();
    static bool validateSkipEnabled();
    static bool validateLegacy();
    static bool gateRetry();
    static bool validateSkipVerify();
    static std::uint32_t writeEvidenceAfter();
    static std::uint32_t writeEvidenceSampleEvery();
    static std::uint64_t writeEvidenceMaxBytes();
    static bool traceCapSync();
    void observeDword(std::uint64_t address, bool unchanged);
    void observeRange(std::uint64_t address, std::span<const std::byte> before);
    static void observePendingWrite(std::uint64_t address, bool unchanged);
    static bool knownValueCurrent(const WrittenBuffer& writer);
    ShaderMemory::PendingWrite classifyPendingWrite(std::uint64_t address, std::size_t bytes, std::uint64_t ValidateCounters::*& reason, const PendingView& pending, std::span<std::byte> known = {});
    static ShaderMemory::PendingWrite queryPendingWrite(std::uint64_t address, std::size_t bytes, std::span<std::byte> known);
    template<typename TVisit>
    static void forEachWrittenBuffer(const ShaderRecompiler::RecompileResult& compiled, TVisit&& visit);
    void noteWrittenBuffers(std::uint64_t program, std::uint32_t queue, const ShaderRecompiler::RecompileResult& compiled);
    static void appendWrittenRanges(const ShaderRecompiler::RecompileResult& compiled, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges);
    void noteForeignWriter(std::uint64_t begin, std::uint64_t end, std::uint32_t queue);
    void noteDrawWriters(std::span<const Graphics::CompiledShader> stages, std::uint32_t queue);
    std::optional<WrittenBuffer> newestWriterLocked(std::uint64_t begin, std::uint64_t end) const;
    std::optional<WrittenBuffer> newestWriter(std::uint64_t begin, std::uint64_t end);
    std::string describeWriters(std::uint64_t begin, std::uint64_t end);
    static std::string describeSelf(const ShaderRecompiler::RecompileResult& compiled, std::uint64_t begin, std::uint64_t end);
    static bool traceBudget();
    void traceCapture(const char* what, std::uint64_t program, std::uint32_t queue, std::span<const ShaderRecompiler::MemoryRegion> regions, double waitedMs);
    void reportValidation(ValidateCounters& counters);
    bool captureStable(std::span<const ShaderRecompiler::MemoryRegion> captured);
    bool validateCaptured(std::uint64_t program, std::uint32_t queue, std::span<const ShaderRecompiler::MemoryRegion> captured, const ShaderRecompiler::RecompileResult& compiled, bool inPlace, const PendingView& view, bool* unmapped = nullptr, std::optional<SampledReadScope>* sampling = nullptr, const DataMask* data = nullptr);
    static void appendEntryRegions(const DispatchVariant& variant, std::vector<ShaderRecompiler::MemoryRegion>& regions, const std::vector<std::uint32_t>* words = nullptr);
    static std::shared_ptr<const ShaderRecompiler::RecompileResult> patchDataWords(const DispatchVariant& variant, const std::vector<std::uint32_t>& words);
    bool syncPendingRuns(std::uint64_t program, std::uint32_t queue, const ShaderRecompiler::RecompileResult& compiled, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& synced, PendingView& pending, std::optional<SampledReadScope>& sampling);
    EntryOutcome validateVariant(std::uint64_t program, std::uint32_t queue, const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& imagesFlushed, std::uint64_t& runsSynced, std::optional<SampledReadScope>& sampling, std::vector<std::pair<std::uint32_t, std::uint32_t>>* live = nullptr);
    static int waitTimeoutMs();
    static bool pollReapAll();
    static bool pollTryEach();
    static bool traceLateLabels();
    void waitMemory(std::span<const std::uint32_t> packet, std::uint32_t queue, const PacketHistory& context, std::uint64_t received, bool heldAtSubmit, bool requireMemory = false);
    struct PendingWait {
        std::array<std::uint32_t, 8> words;
        std::size_t count;
        std::uint64_t received;
        WaitObservation observation;
    };
    static std::vector<PendingWait>& pendingWaits();
    static void notePendingWait(std::span<const std::uint32_t> packet, const WaitObservation& observation);
    static std::uint64_t& waitFloor();
    static WaitFloorMode waitFloorMode();
    static bool verifyWaitObservations();
    static LabelTimelineMode labelTimelineMode();
    static std::chrono::microseconds batchAgeBound();
    void runBatchAge(std::chrono::microseconds bound) noexcept;
    static bool completionWatcher();
    void runCompletionWatcher() noexcept;
    void dischargeWait(WaitObservation& observation, std::span<const std::uint32_t> packet);
    void noteWaitChain(const WaitObservation& observation, std::span<const std::uint32_t> packet);
    void noteRelease(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue);
    void importRelease(const WaitObservation& observation);
    static WaitObservation& lastObservation();
    static void reportWaitChain(std::uint32_t queue);
    void landPendingWaits(std::uint32_t queue);
    struct ResolvedDispatch {
        std::uint64_t address = 0, key = 0, indirectArguments = 0;
        std::array<std::uint32_t, 5> packet{};
        std::shared_ptr<const ShaderSnapshot> registeredShader;
        std::shared_ptr<const ShaderRecompiler::RecompileResult> compiledResult;
        std::shared_ptr<DispatchVariant> keepVariant, attachVariant;
        std::shared_ptr<ShaderMemory> shaderMemory;
        std::vector<ShaderRecompiler::MemoryRegion> captured;
        std::vector<std::uint32_t> liveWords;
        std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
        bool dataHit = false, cached = false, validated = false;
        std::chrono::steady_clock::time_point resolvedAt{};
    };
    struct GroupCaptureStats {
        std::uint64_t runs = 0, resolved = 0, resolvedHits = 0, adopted = 0, addressMismatches = 0, keyMismatches = 0, failures = 0, stale = 0, labelOverlaps = 0, writtenBefore = 0, deviceRejects = 0;
        std::uint64_t pipelineHints = 0;
        double resolveMs = 0, resolveMaxMs = 0, ageMs = 0, ageMaxMs = 0;
        std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
    };
    static std::map<std::size_t, ResolvedDispatch>& resolvedAhead();
    static GroupCaptureStats& groupCaptureStats();
    static std::size_t& currentPacketOffset();
    static bool& resolvingAhead();
    static bool& gateOpened();
    void resolveGroupAhead(const Submission& submission, const QueueState& live, std::size_t from);
    static void clearResolvedAhead();
    void reportGroupCapture(std::uint32_t queue);
    static PollStats& pollStats();
    static WaitOutcomes& waitOutcomes();
    static Graphics::Recorder::LateStatistics& lateCountsSeen();
    static EpochBumps& epochBumps();
    static void bumpEpoch(std::uint64_t EpochBumps::*counter);
    static bool packetEpoch();
    static bool labelTryEachPacket();
    static std::chrono::microseconds labelFlushDeadline();
    static bool labelFlushDue();
    static bool completionsPending();
    static bool reapEachPacket();
    static std::uint64_t batchCap();
    void recordDeferredLabels(VulkanDevice* localDevice, std::uint32_t queue);
    bool recordLabelsForPacket(VulkanDevice* localDevice, std::uint32_t queue);
    void recordQueuedLabelsBeforeRead(std::uint32_t queue);
    bool recordQueuedLabelsAfterCapture(std::uint32_t queue, std::span<const ShaderRecompiler::MemoryRegion> regions);
    void noteQueuedLabels(std::uint32_t queue);
    void recordQueuedLabelsByTry(std::uint32_t queue, std::atomic<std::uint64_t>& counter);
    static void recordQueuedLabelsFromHook();
    void flushBetweenPackets(std::uint32_t queue, std::uint32_t header, bool labelPacket);
    bool preparePacketMemory(const Submission& submission, QueueState& queue, std::span<const std::uint32_t> packet, std::uint32_t header, std::uint32_t opcode, bool& wroteOnGpu, bool& endOfPipeInterrupt, bool& interruptDeferred, bool& drawPacket, bool& sampleDump);
    void dumpSampleCounters(std::uint64_t address);
    template <typename TWork>
    static void timed(double WorkerProfile::*bucket, TWork&& work);
    FrameTiming* frameTiming();
    FrameTiming* includeTimingSubmission(const Submission& submission, bool firstSegment);
    void execute(const Submission& submission);
    void markCompleted(std::uint64_t serial);
    static const std::atomic<std::uint64_t>*& workerQueued();
    void reapCompletionLabels();
    void run(std::uint32_t id) noexcept;

    std::mutex mutex;
    std::mutex shutdownMutex;
    std::condition_variable changed;

    std::map<std::uint32_t, QueueWorker> workers;
    std::uint64_t frameSerial = 0;
    std::shared_ptr<FrameTiming> pendingFrameTiming;

    std::atomic<std::uint64_t> flipsCounted{0};
    std::atomic<std::uint64_t> flipSerial{0};
    std::atomic<std::uint64_t> flipBatchesUnsignaled{0};

    alignas(64) std::atomic<int> packetsInFlight{0};
    std::atomic<std::uint64_t> packetsDone{0};

    std::atomic<std::uint64_t> eventSerial{0};
    QueueScheduler queueScheduler{QueueScheduler::ConfiguredMode(), QueueScheduler::ConfiguredVerify()};
    std::thread batchAgeThread;
    std::mutex batchAgeMutex;
    std::condition_variable batchAgeChanged;
    std::atomic<std::uint64_t> batchAgeSubmits{0};
    std::atomic<std::uint64_t> batchAgeChecks{0};
    std::thread completionThread;
    std::atomic<std::uint64_t> completionWatcherWaits{0};
    std::atomic<std::uint64_t> completionWatcherReaps{0};
    alignas(64) std::shared_ptr<ShaderRegistry> shaders = std::make_shared<ShaderRegistry>();

    std::unordered_map<std::uint64_t, std::shared_ptr<DispatchEntry>> dispatchCache;

    std::list<std::uint64_t> dispatchOrder;
    std::mutex dispatchCacheMutex;
    std::uint64_t dispatchCacheHits = 0;
    std::uint64_t dispatchCacheEvictions = 0;

    std::uint64_t dispatchCacheVariants = 0;
    std::uint64_t dispatchCacheVariantBytes = 0;
    Graphics::CacheSizing dispatchSizing{dispatchCacheLimits()};

    EntryCounters entryCounters;

    static constexpr std::size_t MaxDispatchDonors = 16384;
    RecentTable<DispatchShape> dispatchShapes{MaxDispatchDonors};
    RecentTable<DispatchDonor> dispatchDonors{MaxDispatchDonors};
    RecentTable<bool> evictedDispatchDonors{MaxDispatchDonors * 4};
    std::unordered_map<std::uint64_t, DispatchKeyLast> lastDispatchKeys;
    DispatchRelocationCounters dispatchRelocationCounters;
    DispatchCacheTotals dispatchCacheTotals;

    std::atomic<std::uint64_t> dataPendingMisses{0};

    using ValueSet = std::pair<std::vector<std::pair<std::uint64_t, std::uint64_t>>, std::vector<std::uint32_t>>;
    std::unordered_map<std::uint64_t, std::deque<ValueSet>> priorValueSets;

    std::unordered_map<std::uint64_t, std::shared_ptr<DrawEntry>> drawCache;
    std::unordered_map<std::uint64_t, std::uint64_t> drawShapes;
    std::unordered_set<std::uint64_t> traceShapes, traceKeysSeen, traceKeysEvicted;
    std::list<std::uint64_t> drawOrder;
    std::unordered_map<std::uint64_t, std::shared_ptr<const DrawRelocation>> drawRelocations;
    static constexpr std::size_t MaxDrawAliases = 16384;
    RecentTable<DrawStageDonor> drawStageDonors{MaxDrawAliases};
    RecentTable<DrawDecodeDonor> drawDecodes{MaxDrawAliases};
    DrawRelocationPending drawRelocationPending;
    std::unordered_map<std::uint64_t, DrawRelocationTrial> drawRelocationTrials;
    DrawRelocationCounters drawRelocationCounters;
    DrawMissDiagnosis drawMissDiagnosis;
    bool drawExactKeySeen = false;
    std::vector<std::uint64_t> drawStageDonorKeys;
    std::mutex drawCacheMutex;
    std::shared_ptr<DrawRecordResult> drawRecordResult;
    PrepareAheadWindow<DrawRecordResult> drawRecordWindow;
    std::mutex frozenPostedMutex;
    std::vector<std::shared_ptr<DrawRecordResult>> frozenPosted;
    std::uint64_t drawCacheHits = 0, drawCacheEvictions = 0, drawCacheVariants = 0, drawCacheVariantBytes = 0;
    std::optional<DrawKeySightings> drawKeySightings;
    std::optional<DrawKeySightings> drawShapeSightings;
    Graphics::CacheSizing drawSizing{drawCacheLimits()};

    DrawEntryCounters drawEntryCounters;
    RecentTable<std::shared_ptr<const DrawDecode>> plannedDecodes{MaxDrawAliases};
    BindingPlanCounters bindingPlanCounters;
    std::mutex bindingPlanMutex;

    std::mutex driverPhasesMutex;
    std::array<DriverPhaseTotals, DispatchClassCount> driverPhaseTotals{};
    std::chrono::steady_clock::time_point driverPhasesReport = std::chrono::steady_clock::now();

    std::mutex drawPhasesMutex;
    DrawPhaseTotals drawPhaseTotals;

    std::map<std::uint32_t, QueueState> queues;
    std::map<std::uint32_t, std::shared_ptr<IVideoOutput>> outputs;
    struct EopFlip {
        std::uint32_t handle;
        std::shared_ptr<IFlipRequest> request;
    };
    std::map<std::uint32_t, EopFlip> eopFlips;
    std::uint32_t nextEopToken = EopFlipTokenBase;
    DevicePointer device;

    std::mutex labelStoresMutex;
    std::unordered_map<std::uint64_t, std::array<LabelStore, LabelStoreHistory>> labelStores;
    std::mutex releasesMutex;
    std::array<Coherence::Release, 4096> releases;

    std::vector<std::shared_ptr<VulkanDevice>> replacedDevices;
    std::stop_token shutdownToken = LibcShutdownToken_nid_postfix();
    DeviceUseGate deviceReplacement;
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    std::set<std::uint64_t> completedOutOfOrder;
    std::exception_ptr failure;

    std::atomic<bool> failed{false};
    std::atomic<bool> stopping{false};
    bool stopped = false;
    bool resetGraphics = false;

    std::uint32_t idleWaiters = 0;
    std::uint64_t queue0Executing = 0;
    std::atomic<std::uint32_t> orderHolders{0};
    std::atomic<std::uint32_t> runningWorkers{0};
    std::atomic<std::uint64_t> idleEpoch{0};
    std::atomic<std::uint64_t> queue0Awaited{0};
    std::atomic<std::uint32_t> flipsAhead{0};
    std::atomic<std::uint32_t> flipHolders{0};
    std::atomic<bool> queue0AwaitsTitle{false};

    std::atomic<std::uint64_t> evidenceReads{0};
    std::atomic<std::uint64_t> evidenceValidations{0};

    std::mutex writtenBuffersMutex;
    std::deque<WrittenBuffer> writtenBuffers;
    std::uint64_t writtenBufferSerial = 0;
    std::unordered_map<std::uint64_t, DwordEvidence> dwordEvidence;
    std::atomic<std::uint64_t> observedUnchanged{0};
    std::atomic<std::uint64_t> observedChanged{0};

    std::atomic<std::uint64_t> observationsNoWait{0};
    std::atomic<std::uint64_t> observationsNotReached{0};

    std::mutex validateMutex;
    ValidateCounters validateCounters;

};

}

#endif

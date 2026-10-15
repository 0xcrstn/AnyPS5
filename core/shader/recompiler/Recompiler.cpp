#include "Recompiler.hpp"
#include "Materialize.hpp"
#include "ShaderManifest.hpp"
#include "StencilReplay.hpp"
#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include "CacheKey.hpp"
#include "CompileTrace.hpp"
#include "SpirvIntern.hpp"
#include "CompiledVariant.hpp"
#include "VertexInputSpecialization.hpp"
#include "FragmentOutputSpecialization.hpp"
#include "SpirvBackend/SpirvSpecialization.hpp"
#include "RdnaDecoder/RdnaDescriptorFormat.hpp"
#include "ShaderDiskCache.hpp"
#include <list>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include "ControlFlow/include/ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/include/ControlFlow/Structurizer.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "IntermediateRepresentation/include/IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/include/Optimization/BindingAllocator.hpp"
#include "Optimization/include/Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "Optimization/include/Optimization/ConstantFolder.hpp"
#include "Optimization/include/Optimization/DeadCodeEliminator.hpp"
#include "Optimization/include/Optimization/DenormalFlushEliminator.hpp"
#include "Optimization/include/Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/include/Optimization/HostInterpolationChecker.hpp"
#include "Optimization/include/Optimization/MaskedSelectEliminator.hpp"
#include "Optimization/include/Optimization/ReadLaneEliminator.hpp"
#include "Optimization/include/Optimization/RedundantValueEliminator.hpp"
#include "Optimization/include/Optimization/RequestMemoryView.hpp"
#include "Optimization/include/Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/include/Optimization/ResourceTracker.hpp"
#include "Optimization/include/Optimization/ShaderInfoCollector.hpp"
#include "Optimization/include/Optimization/SrtWalker.hpp"
#include "Optimization/include/Optimization/SsaBuilder.hpp"
#include "Optimization/include/Optimization/TessellationLowering.hpp"
#include "SpirvBackend/include/SpirvBackend/SpirvEmitter.hpp"
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "SpirvBackend/SpirvOptimizer.hpp"
#endif
#include "SpirvBackend/SpirvModuleShape.hpp"
#include "SpirvBackend/SpirvMemory/SpirvInputOutput.hpp"
#include "Translation/include/Translation/InstructionTranslator.hpp"
#include "Translation/include/Translation/ShaderInputInfoBuilder.hpp"
#include <exception>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <ControlFlow/RequestSerializer.hpp>

namespace ShaderRecompiler {

using namespace Materialization;

namespace {

ShaderStageKind toShaderStageKind(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Compute:
        return ShaderStageKind::Compute;
    case ShaderStage::Vertex:
        return ShaderStageKind::Vertex;
    case ShaderStage::TessellationControl:
        return ShaderStageKind::TessellationControl;
    case ShaderStage::TessellationEvaluation:
        return ShaderStageKind::TessellationEvaluation;
    case ShaderStage::Fragment:
        return ShaderStageKind::Pixel;
    case ShaderStage::Local:
        return ShaderStageKind::Local;
    case ShaderStage::Mesh:
        return ShaderStageKind::Mesh;
    case ShaderStage::Geometry:
        break;
    }
    throw std::runtime_error("ShaderRecompiler::Recompile: unsupported shader stage");
}

}

namespace {

// The host subgroup width wave64 programs are laid out for. Debug aid: APS5_SINGLE_LANE=<hex code
// addresses, comma separated, or "all"> keeps the listed programs at one guest lane per invocation.
std::uint32_t HostSubgroupSize(const RecompileRequest& request) {
    const bool vertexStage = request.shader.stage == ShaderStage::Vertex || request.shader.stage == ShaderStage::Local;
    if (vertexStage && !request.target.vertexSubgroups) return 1u;
    static const std::string list = [] { const char* text = std::getenv("APS5_SINGLE_LANE"); return text ? std::string(text) : std::string(); }();
    if (!list.empty()) {
        if (list == "all") return 64u;
        char address[32];
        std::snprintf(address, sizeof(address), "%llx", static_cast<unsigned long long>(request.shader.codeAddress));
        if (list.find(address) != std::string::npos) return 64u;
    }
    return request.target.subgroupSize;
}

RdnaProgram DecodeStageProgram(ShaderStageKind stage, std::span<const std::uint32_t> code) {
    if (stage != ShaderStageKind::Local) return RdnaInstructionDecoder{}.Decode(code);
    auto program = DecodeRdnaFrontProgram(code);
    static constexpr std::array<std::uint32_t, 1> EndProgram{0xbf810000u};
    program.instructions.back() = DecodeRdnaInstruction(program.instructions.back().programCounter, EndProgram, 0u);
    return program;
}

ShaderStageInputInfo RequestInputInfo(const RecompileRequest& request) {
    const auto* mesh = request.graphics && request.graphics->mesh ? &*request.graphics->mesh : nullptr;
    std::optional<TessellationConfiguration> tessellationCopy;
    if (request.graphics && request.graphics->tessellation) {
        tessellationCopy = *request.graphics->tessellation;
        tessellationCopy->instancePatches = TessellationInstancePatches(request);
        const auto& capabilities = request.target.supportedCapabilities;
        tessellationCopy->drawParameters = std::find(capabilities.begin(), capabilities.end(), static_cast<std::uint32_t>(spv::CapabilityDrawParameters)) != capabilities.end();
    }
    const auto* tessellation = tessellationCopy ? &*tessellationCopy : nullptr;
    return BuildShaderStageInputInfo(toShaderStageKind(request.shader.stage), request.context, HostSubgroupSize(request), mesh, tessellation);
}

std::uint32_t DeviceMemoryLdsBytes(const RecompileRequest& request) {
    if (!request.context.compute.has_value()) return 0u;
    const auto bytes = static_cast<std::uint64_t>(request.context.compute->ldsSizeDwords) * 4u;
    return bytes + 4u > request.target.maxWorkgroupSharedMemoryBytes ? static_cast<std::uint32_t>(bytes) : 0u;
}

std::atomic<std::uint64_t> frontEndBuilds{0};

std::uint64_t estimateProgramBytes(const IrProgram& program);

void TraceRequest(ShaderCompileTrace::Event& event, const RecompileRequest& request) {
    if (!event.Active()) return;
    std::vector<std::uint64_t> interfaceKey;
    RecompileCacheKey::BuildInterface(request, interfaceKey);
    const std::array<std::uint32_t, 4> layout{request.layout.descriptorSet, request.layout.firstBinding, request.layout.pushConstantOffsetBytes, request.layout.pushConstantSizeBytes};
    event.Hex("address", request.shader.codeAddress)
        .Number("stage", static_cast<std::uint64_t>(request.shader.stage))
        .Number("code_dwords", request.shader.code.size())
        .Hex("code_hash", ShaderCompileTrace::HashOf(std::span<const std::uint32_t>(request.shader.code.data(), request.shader.code.size())))
        .Hex("context_hash", RecompileCacheKey::ContextHash(request))
        .Hex("interface_key", ShaderCompileTrace::HashOf(std::span<const std::uint64_t>(interfaceKey)))
        .Hex("layout_key", ShaderCompileTrace::HashOf(std::span<const std::uint32_t>(layout)))
        .Number("callees", request.context.linkedCallFallbacks.size());
}

}

struct PreparedControlFlow {
    PreparedControlFlow(const RecompileRequest& request, const SwappcInfo& swappcInfo)
        : stage(request.shader.stage), fallbacks(swappcInfo.linkedCallFallbacks.begin(), swappcInfo.linkedCallFallbacks.end()), swappc(WithFallbacks(swappcInfo, fallbacks)), code(request.shader.code.begin(), request.shader.code.end()),
          decoded(DecodeStageProgram(toShaderStageKind(request.shader.stage), code)), cfg(GraphBuilder{}.Build(decoded, &swappc)) {
        Structurizer{}.Structurize(cfg);
    }

    PreparedControlFlow(const PreparedControlFlow&) = delete;
    PreparedControlFlow& operator=(const PreparedControlFlow&) = delete;

    bool Matches(const RecompileRequest& request, const SwappcInfo& swappcInfo) const {
        return stage == request.shader.stage && swappc.fetchCallAllowed == swappcInfo.fetchCallAllowed &&
               swappc.userDataBaseRegister == swappcInfo.userDataBaseRegister && swappc.userDataCount == swappcInfo.userDataCount &&
               std::ranges::equal(fallbacks, swappcInfo.linkedCallFallbacks) && std::ranges::equal(code, request.shader.code);
    }

    static SwappcInfo WithFallbacks(SwappcInfo info, const std::vector<std::uint32_t>& fallbacks) {
        info.linkedCallFallbacks = fallbacks;
        return info;
    }

    ShaderStage stage;
    std::vector<std::uint32_t> fallbacks;
    SwappcInfo swappc;
    std::vector<std::uint32_t> code;
    RdnaProgram decoded;
    ControlFlowGraph cfg;
};

std::shared_ptr<const PreparedControlFlow> ShaderPreparationContext::AcquireFrontend(const RecompileRequest& request) {
    const auto inputInfo = RequestInputInfo(request);
    const SwappcInfo swappc{inputInfo.vertex != nullptr, request.context.userDataBaseRegister, static_cast<std::uint32_t>(request.context.userData.size()), 0u, request.context.linkedCallFallbacks};
    static const bool reuse = std::getenv("APS5_NO_PERF_FRONTEND_PAIR") == nullptr;
    if (reuse && frontend != nullptr && frontend->Matches(request, swappc)) {
        return frontend;
    }
    auto prepared = std::make_shared<const PreparedControlFlow>(request, swappc);
    if (reuse) {
        frontend = prepared;
    }
    return prepared;
}

std::uint64_t FrontEndBuilds() {
    return frontEndBuilds.load(std::memory_order_relaxed);
}

std::uint32_t ShaderPreparationContext::ResourcePrograms() const {
    return resourcePrograms;
}

IrProgram PrepareResourceProgram(const RecompileRequest& request) {
    return PrepareResourceProgram(request, nullptr);
}

IrProgram PrepareResourceProgram(const RecompileRequest& request, ShaderPreparationContext* preparation) {
    frontEndBuilds.fetch_add(1, std::memory_order_relaxed);
    ShaderPreparationContext local;
    auto& context = preparation != nullptr ? *preparation : local;
    ++context.resourcePrograms;
    context.interfaceSource.reset();
    context.interfaceProgram.reset();
    ShaderCompileTrace::Event traced("frontend");
    TraceRequest(traced, request);
    const auto frontend = [&] {
        ShaderCompileTrace::Event decodeEvent("frontend.decode_cfg");
        const auto previous = context.frontend;
        auto acquired = context.AcquireFrontend(request);
        decodeEvent.Flag("reused", acquired == previous).Number("instructions", acquired->decoded.instructions.size());
        return acquired;
    }();
    const auto stageKind = toShaderStageKind(request.shader.stage);
    const auto inputInfo = RequestInputInfo(request);
    const auto& decoded = frontend->decoded;
    const auto& cfg = frontend->cfg;

    TranslateOptions translateOptions {};
    translateOptions.stage = stageKind;
    translateOptions.shaderHash = request.shader.codeAddress;
    translateOptions.waveSize = request.context.waveSize;
    translateOptions.userDataBaseRegister = request.context.userDataBaseRegister;
    translateOptions.userDataCount = static_cast<std::uint32_t>(request.context.userData.size());
    translateOptions.scratchDwords = request.context.compute.has_value() ? request.context.compute->scratchDwords : 0u;
    translateOptions.sharedMemoryBytes = DeviceMemoryLdsBytes(request);
    translateOptions.fragmentShaderBarycentricEnabled = request.target.fragmentShaderBarycentricEnabled;
    const auto supports = [&](spv::Capability capability) { return std::ranges::find(request.target.supportedCapabilities, static_cast<std::uint32_t>(capability)) != request.target.supportedCapabilities.end(); };
    translateOptions.nativeFloat16 = supports(spv::CapabilityFloat16) && supports(spv::CapabilityRoundingModeRTE) && supports(spv::CapabilityDenormPreserve);
    translateOptions.floatMode = request.context.floatMode;
    translateOptions.inputInfo = inputInfo;

    constexpr InstructionTranslator translator;

    EmbeddedFetchPlan embeddedFetch;
    if ((stageKind == ShaderStageKind::Vertex || stageKind == ShaderStageKind::Local) && inputInfo.vertex != nullptr && inputInfo.vertex->fetchEmbedded) {
        embeddedFetch = EmbeddedVertexFetchAnalyzer{}.Analyze(decoded, inputInfo.vertex->fetchAttribReg, inputInfo.vertex->fetchBufferReg, request.context.userDataBaseRegister, static_cast<std::uint32_t>(request.context.userData.size()), request.context.waveSize);
        translateOptions.embeddedFetch = &embeddedFetch;
    }
    auto program = [&] {
        ShaderCompileTrace::Event translateEvent("frontend.translate");
        auto translated = translator.Translate(decoded, cfg, translateOptions);
        translateEvent.Number("ir_bytes", estimateProgramBytes(translated));
        return translated;
    }();
    // Debug aid: APS5_DUMP_IR=<hex code address> (or "all") prints the program after each front-end pass.
    const auto dumpIr = [&](const char* pass) {
        static const std::string list = [] { const char* text = std::getenv("APS5_DUMP_IR"); return text ? std::string(text) : std::string(); }();
        if (list.empty()) return;
        char address[32];
        std::snprintf(address, sizeof(address), "%llx", static_cast<unsigned long long>(request.shader.codeAddress));
        if (list != "all" && list.find(address) == std::string::npos) return;
        std::fprintf(stderr, "==== IR 0x%s after %s\n%s\n", address, pass, ProgramToString(program).c_str());
    };

    constexpr SsaBuilder ssaBuilder;
    constexpr ConstantFolder constantFolder;
    constexpr DeadCodeEliminator deadCodeEliminator;
    constexpr ReadLaneEliminator readLaneEliminator;
    constexpr MaskedSelectEliminator maskedSelectEliminator;
    const auto simplify = [&] {
        ShaderCompileTrace::Event simplifyEvent("frontend.simplify");
        dumpIr("translate");
        ssaBuilder.Rewrite(program);
        dumpIr("ssa");

        constantFolder.Fold(program);
        ResolveControlFlowIdentities(program);
        deadCodeEliminator.RemoveIdentities(program);
        deadCodeEliminator.Eliminate(program);
        dumpIr("fold");

        const auto readLaneStats = readLaneEliminator.Eliminate(program, translateOptions.waveSize);
        if (readLaneStats.rewrittenReads != 0u) {
            constantFolder.Fold(program);
            ResolveControlFlowIdentities(program);
            deadCodeEliminator.RemoveIdentities(program);
            deadCodeEliminator.Eliminate(program);
        }

        if (maskedSelectEliminator.Eliminate(program).removedSelects != 0u) {
            const bool liveAncillary = std::ranges::any_of(program.Blocks(), [](const auto& block) {
                return std::ranges::any_of(block->Instructions(), [](const IrValue* value) {
                    return value->Opcode() == IrOpcode::GetBuiltin && value->HasUses() && value->Argument(0)->Resolve()->ImmediateU32() == static_cast<std::uint32_t>(StageInputKind::PackedAncillary);
                });
            });
            if (liveAncillary) {
                constantFolder.Fold(program);
                ResolveControlFlowIdentities(program);
                deadCodeEliminator.RemoveIdentities(program);
            }
            deadCodeEliminator.Eliminate(program);
        }
        if (constantFolder.ComposePackedAncillary(program)) {
            deadCodeEliminator.Eliminate(program);
        }
    };
    simplify();
    if (stageKind == ShaderStageKind::Pixel && !translateOptions.fragmentShaderBarycentricEnabled && !HostInterpolationChecker{}.Lower(program, *inputInfo.pixel)) {
        translateOptions.fragmentShaderBarycentricEnabled = true;
        program = translator.Translate(decoded, cfg, translateOptions);
        program.Metadata().barycentricEmulation = true;
        simplify();
    }

    ShaderCompileTrace::Event lowerEvent("frontend.lower_resources");
    constexpr DenormalFlushEliminator denormalFlushEliminator;
    if (denormalFlushEliminator.Eliminate(program).removedFlushes != 0u) {
        deadCodeEliminator.Eliminate(program);
    }

    if (inputInfo.vertex != nullptr) {
        constexpr TessellationLowering tessellationLowering;
        tessellationLowering.Lower(program, inputInfo.vertex->tess);
    }

    constexpr SrtWalker srtWalker;
    srtWalker.BuildPlan(program);
    deadCodeEliminator.Eliminate(program);
    dumpIr("srt");

    constexpr ResourceTracker resourceTracker;
    resourceTracker.Track(program);
    deadCodeEliminator.Eliminate(program);
    dumpIr("resources");
    program.Resources().srgbDecodeFormats = request.target.srgbDecodeFormats;
    if (traced.Active()) traced.Number("ir_bytes", estimateProgramBytes(program)).Flag("barycentric_retranslated", program.Metadata().barycentricEmulation);

    return program;
}

// A materialized result of one variant over one snapshot (Recompile(request, capture)): the
// shared immutable object every later capture that reproduces the snapshot receives, so Populate
// and the per-request copy run once per distinct snapshot.
struct ResultMemoEntry {
    std::uint64_t variantId;
    std::uint64_t hash;
    std::shared_ptr<const RecompileResult> result;
    std::uint64_t bytes = 0;
};

struct EmissionFailure {
    std::uint64_t codeAddress;
    BindingLayout layout;
    std::exception_ptr failure;
};

struct MaterializedTemplate;

struct SourceEntry : std::enable_shared_from_this<SourceEntry> {
    std::mutex mutex;
    // The code the entry was built for: the key carries only a hash of it, so a candidate entry is
    // accepted only when its code matches word for word. Owned here because the request's span
    // points into a registration the driver may replace while the entry lives on.
    std::vector<std::uint32_t> code;
    std::vector<std::uint32_t> linkedCallFallbacks;
    std::shared_ptr<const IrResourcePlan> plan;
    // A plan build that threw (an unsupported resource chain or control flow) is remembered and
    // rethrown: the front end ran every pass before failing, ~13 ms per dispatch of a shader the
    // title issues every frame (0x1048947300 at the intro video). APS5_NO_FAILURE_MEMO=1 rebuilds.
    std::exception_ptr planFailure;
    std::unique_ptr<IrProgram> program;
    std::vector<std::shared_ptr<const CompiledVariant>> variants;
    std::vector<EmissionFailure> emissionFailures;
    // The result memo, most recently used first, at most ResultMemoEntries (under mutex).
    std::list<ResultMemoEntry> memo;
    std::unordered_map<std::uint64_t, std::list<ResultMemoEntry>::iterator> memoIndex;
    std::vector<std::byte> planKey;
    std::weak_ptr<const IrResourcePlan> spilledPlan;
    std::uint64_t planCodeAddress = 0;
    std::uint64_t planBytes = 0;
    bool planStored = false;
    bool planSpilled = false;
    bool planUsed = false;
    bool planSpillRequested = false;
    std::vector<std::shared_ptr<const MaterializedTemplate>> templates;
};

namespace {

bool SpillPreparedSpirv() {
    static const bool spill = std::getenv("APS5_NO_SPIRV_SPILL") == nullptr;
    return spill;
}

std::uint64_t countPlan(const IrResourcePlan& plan) {
    std::uint64_t values = 0;
    const auto bytes = EstimatePlanBytes(plan, &values);
    auto& counters = retainedCounters();
    counters.plans.fetch_add(1, std::memory_order_relaxed);
    counters.planValues.fetch_add(values, std::memory_order_relaxed);
    counters.planBytes.fetch_add(bytes, std::memory_order_relaxed);
    return bytes;
}

bool SpillResourcePlans() {
    static const bool spill = std::getenv("APS5_NO_PLAN_SPILL") == nullptr;
    return spill;
}

void spillPlanLocked(SourceEntry& source) {
    if (source.plan == nullptr || source.planUsed || !source.planStored || !source.planSpillRequested) return;
    source.spilledPlan = source.plan;
    source.plan.reset();
    source.planSpilled = true;
    retainedCounters().spilledPlans.fetch_add(1, std::memory_order_relaxed);
    retainedCounters().spilledPlanBytes.fetch_add(source.planBytes, std::memory_order_relaxed);
}

std::shared_ptr<const IrResourcePlan> usePlan(SourceEntry& source) {
    std::lock_guard lock(source.mutex);
    source.planUsed = true;
    if (source.plan != nullptr || !source.planSpilled) return source.plan;
    source.plan = source.spilledPlan.lock();
    if (source.plan == nullptr) {
        IrResourcePlan loaded;
        if (!ShaderDiskCache::ReloadPlan(source.planKey, loaded)) throw std::runtime_error("ShaderRecompiler: a resource plan kept in the shader disk cache could not be read back from it");
        loaded.shaderHash = source.planCodeAddress;
        source.plan = std::make_shared<const IrResourcePlan>(std::move(loaded));
        retainedCounters().planReloads.fetch_add(1, std::memory_order_relaxed);
    }
    source.planSpilled = false;
    retainedCounters().spilledPlans.fetch_sub(1, std::memory_order_relaxed);
    retainedCounters().spilledPlanBytes.fetch_sub(source.planBytes, std::memory_order_relaxed);
    return source.plan;
}

std::uint64_t estimateValueBytes(const IrValue& value) {
    return sizeof(IrValue) + (value.ArgumentCount() + value.UseCount() + value.PhiBlockCount()) * sizeof(void*) + value.OperandUses().size() * sizeof(IrUse);
}

std::uint64_t estimateProgramBytes(const IrProgram& program) {
    std::uint64_t bytes = sizeof(IrProgram);
    for (const auto& block : program.Blocks()) {
        if (block == nullptr) continue;
        bytes += sizeof(IrBlock) + (block->Predecessors().size() + block->Successors().size()) * sizeof(void*);
        for (const auto* value : block->Instructions()) bytes += estimateValueBytes(*value) + 3u * sizeof(void*);
    }
    return bytes;
}

template<typename TElement>
std::uint64_t vectorBytes(const std::vector<TElement>& values) {
    return values.size() * sizeof(TElement);
}

template<typename TElement, std::size_t Count>
std::uint64_t vectorBytes(const std::array<TElement, Count>&) {
    return 0;
}

std::uint64_t estimateVariantMetadataBytes(const CompiledVariant& variant) {
    const auto& info = variant.info.info;
    std::uint64_t bytes = sizeof(CompiledVariant) + vectorBytes(info.buffers) + vectorBytes(info.images) + vectorBytes(info.runtimeImageModes) + vectorBytes(info.samplers) + vectorBytes(info.sampledPairs) + vectorBytes(info.inputs) + vectorBytes(info.outputs);
    for (const auto& modes : info.runtimeImageModes) bytes += vectorBytes(modes);
    const auto& artifact = variant.artifact;
    bytes += vectorBytes(artifact.runtimeImageResources) + vectorBytes(artifact.vertexInputs) + vectorBytes(artifact.vertexInputPatches) + vectorBytes(artifact.parameterExports) + vectorBytes(artifact.fragmentParameters);
    for (const auto& patch : artifact.vertexInputPatches) bytes += vectorBytes(patch.values);
    return bytes;
}

class InFlightIr {
public:
    explicit InFlightIr(std::uint64_t bytes) : bytes(bytes) {
        auto& counters = retainedCounters();
        counters.irPrograms.fetch_add(1, std::memory_order_relaxed);
        const auto total = counters.irBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        auto peak = counters.irPeakBytes.load(std::memory_order_relaxed);
        while (total > peak && !counters.irPeakBytes.compare_exchange_weak(peak, total, std::memory_order_relaxed)) {}
    }
    InFlightIr(const InFlightIr&) = delete;
    InFlightIr& operator=(const InFlightIr&) = delete;
    ~InFlightIr() {
        Release();
    }

    void Release() {
        if (released) return;
        released = true;
        retainedCounters().irPrograms.fetch_sub(1, std::memory_order_relaxed);
        retainedCounters().irBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }

private:
    std::uint64_t bytes;
    bool released = false;
};

void countProgram(bool retained) {
    if (retained) retainedCounters().programs.fetch_add(1, std::memory_order_relaxed);
    else retainedCounters().programs.fetch_sub(1, std::memory_order_relaxed);
}

bool VerifyShaderCache() {
    static const bool verify = std::getenv("APS5_VERIFY_SHADER_CACHE") != nullptr;
    return verify;
}

struct ResourceProgram {
    explicit ResourceProgram(const RecompileRequest& request, ShaderPreparationContext* preparation = nullptr, bool reclaim = true) : program(std::make_unique<IrProgram>(PrepareResourceProgram(request, preparation))) {
        const auto clearRegisterState = [](const auto& storage) {
            for (const auto& block : storage) {
                block->ssaScalarValues.fill(nullptr);
                block->ssaVectorValues.fill(nullptr);
                block->ssaThreadBitScalarValues.fill(nullptr);
                block->ssaScalarMaskTags.fill(nullptr);
            }
        };
        clearRegisterState(program->Blocks());
        clearRegisterState(program->Resources().blockStorage);
        if (reclaim) program->ReclaimUnusedValues();
        const InFlightIr inFlight(estimateProgramBytes(*program));
        plan = std::make_shared<const IrResourcePlan>(ResourceMaterializer{}.ExtractPlan(*program));
    }

    std::unique_ptr<IrProgram> program;
    std::shared_ptr<const IrResourcePlan> plan;
};

std::shared_ptr<const IrResourcePlan> makeResourcePlan(const RecompileRequest& request) {
    return ResourceProgram(request, nullptr, false).plan;
}

struct SourceKeyHash {
    std::size_t operator()(const std::vector<std::uint64_t>& key) const {
        std::size_t hash = 0;
        for (const auto value : key) {
            hash ^= static_cast<std::size_t>(value) + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) + (hash >> 2u);
            if constexpr (sizeof(std::size_t) < sizeof(value)) hash ^= static_cast<std::size_t>(value >> 32u);
        }
        return hash;
    }
};

bool FailureMemo() {
    static const bool memo = std::getenv("APS5_NO_FAILURE_MEMO") == nullptr;
    return memo;
}

bool diskPlans() {
    static const bool enabled = [] {
        const char* dumpIr = std::getenv("APS5_DUMP_IR");
        return std::getenv("APS5_NO_PERF_DISK_PLANS") == nullptr && (dumpIr == nullptr || *dumpIr == '\0');
    }();
    return enabled && ShaderDiskCache::Enabled() && !DebugProbeActive();
}

std::shared_ptr<SourceEntry> getSource(const RecompileRequest& request, ShaderPreparationContext* preparation = nullptr, bool reclaim = true) {
    static PrimedSharedMutex mutex;
    // Entries whose code hashes alike share a bucket; the code comparison picks the right one.
    static std::unordered_map<std::vector<std::uint64_t>, std::vector<std::shared_ptr<SourceEntry>>, SourceKeyHash> sources;
    struct SourceKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, SourceKeyStorage>();
    RecompileCacheKey::Build(request, key);
    key.push_back(HostSubgroupSize(request));
    const auto find = [&]() -> std::shared_ptr<SourceEntry> {
        const auto found = sources.find(key);
        if (found == sources.end()) return nullptr;
        for (const auto& entry : found->second) {
            if (std::equal(entry->code.begin(), entry->code.end(), request.shader.code.begin(), request.shader.code.end())) return entry;
        }
        return nullptr;
    };
    std::shared_ptr<SourceEntry> source;
    {
        std::shared_lock lock(mutex);
        source = find();
    }
    if (source == nullptr) {
        std::unique_lock lock(mutex);
        source = find();
        if (source == nullptr) {
            source = std::make_shared<SourceEntry>();
            retainedCounters().sources.fetch_add(1, std::memory_order_relaxed);
            source->code.assign(request.shader.code.begin(), request.shader.code.end());
            source->linkedCallFallbacks.assign(request.context.linkedCallFallbacks.begin(), request.context.linkedCallFallbacks.end());
            auto& bucket = sources[key];
            if (!bucket.empty()) {
                // A second entry under one key is a code hash collision (or the unhashed key with
                // identical code, which cannot happen); each is reported under the profile switch.
                static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
                static std::uint64_t collisions = 0;
                ++collisions;
                if (profile) std::fprintf(stderr, "[recompile] source key collision %llu: %zu entries share a key (%zu code words)\n", static_cast<unsigned long long>(collisions), bucket.size() + 1, request.shader.code.size());
            }
            bucket.push_back(source);
        }
    }
    {
        std::lock_guard lock(source->mutex);
        if (source->plan == nullptr && !source->planSpilled) {
            if (FailureMemo() && source->planFailure) std::rethrow_exception(source->planFailure);
            const bool disk = diskPlans();
            std::vector<std::byte> planKey;
            if (disk) {
                ShaderDiskCache::BuildPlanKey(request, HostSubgroupSize(request), planKey);
                IrResourcePlan loaded;
                if (ShaderDiskCache::LoadPlan(planKey, loaded)) {
                    loaded.shaderHash = request.shader.codeAddress;
                    if (VerifyShaderCache()) {
                        ShaderCompileTrace::Event verifyEvent("verify.cached_plan");
                        TraceRequest(verifyEvent, request);
                        const ResourceProgram fresh(request, preparation, reclaim);
                        if (!ShaderDiskCache::SamePlan(*fresh.plan, loaded)) throw std::runtime_error("APS5_VERIFY_SHADER_CACHE: the cached resource plan of shader 0x" + std::to_string(request.shader.codeAddress) + " differs from a fresh one (plan source version " + std::to_string(ShaderDiskCache::PlanSourceVersion()) + ")");
                    }
                    source->plan = std::make_shared<const IrResourcePlan>(std::move(loaded));
                    source->planBytes = countPlan(*source->plan);
                    source->planStored = true;
                } else if (std::string refused; FailureMemo() && ShaderDiskCache::LoadPlanFailure(planKey, refused)) {
                    source->planFailure = std::make_exception_ptr(std::runtime_error(refused));
                    std::rethrow_exception(source->planFailure);
                }
            }
            if (source->plan == nullptr) {
                try {
                    ResourceProgram resource(request, preparation, reclaim);
                    source->plan = std::move(resource.plan);
                    source->program = std::move(resource.program);
                    source->planBytes = countPlan(*source->plan);
                    if (source->program != nullptr) countProgram(true);
                } catch (const std::runtime_error& error) {
                    if (FailureMemo()) source->planFailure = std::current_exception();
                    if (disk && FailureMemo() && typeid(error) == typeid(std::runtime_error)) ShaderDiskCache::StorePlanFailure(planKey, error.what());
                    throw;
                } catch (...) {
                    if (FailureMemo()) source->planFailure = std::current_exception();
                    throw;
                }
                if (disk) {
                    ShaderDiskCache::StorePlan(planKey, source->plan, [weak = std::weak_ptr<SourceEntry>(source)] {
                        const auto stored = weak.lock();
                        if (stored == nullptr) return;
                        std::lock_guard storedLock(stored->mutex);
                        stored->planStored = true;
                        spillPlanLocked(*stored);
                    });
                }
            }
            if (disk) {
                source->planKey = std::move(planKey);
                source->planCodeAddress = request.shader.codeAddress;
            }
        }
    }
    return source;
}

template<typename TStep>
auto recordEmissionFailure(std::exception_ptr* emissionFailure, TStep&& step) -> decltype(step()) {
    try {
        return step();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        if (emissionFailure != nullptr) *emissionFailure = std::current_exception();
        throw;
    }
}

void prepareInterface(const RecompileRequest& request, IrProgram& program) {
    const auto inputInfo = RequestInputInfo(request);
    constexpr DeadCodeEliminator deadCodeEliminator;
    constexpr ResourceMaterializer resourceMaterializer;
    const bool nativeSampleOffsets = request.target.nonConstantImageOffsets && std::find(request.target.supportedCapabilities.begin(), request.target.supportedCapabilities.end(), spv::CapabilityImageGatherExtended) != request.target.supportedCapabilities.end();
    resourceMaterializer.ApplyStaticInterface(program, nativeSampleOffsets);

    deadCodeEliminator.RemoveIdentities(program);
    constexpr RedundantValueEliminator redundantValueEliminator;
    static_cast<void>(redundantValueEliminator.Eliminate(program));
    deadCodeEliminator.Eliminate(program);

    constexpr ShaderInfoCollector shaderInfoCollector;
    shaderInfoCollector.Collect(program, inputInfo);
}

bool VerifyShaderEmission() {
    static const bool verify = std::getenv("APS5_VERIFY_SHADER_EMISSION") != nullptr;
    return verify;
}

void TraceShape(ShaderCompileTrace::Event& event, std::span<const std::uint32_t> words) {
    const auto shape = DescribeSpirvShape(words);
    event.Number("functions", shape.functions).Number("entry_words", shape.entryFunctionWords).Number("helper_words", shape.helperFunctionWords).Number("largest_helper_words", shape.largestHelperWords).Number("switches", shape.switches).Number("switch_targets", shape.switchTargets).Number("largest_switch_targets", shape.largestSwitchTargets).Number("calls", shape.functionCalls);
}

void VerifyEmission(const RecompileRequest& request, const IrProgram& program, const ShaderStageInputInfo& inputInfo, const CompiledBindingLayout& bindings, SpirvTargetOptions options, std::span<const std::uint32_t> produced) {
    ShaderCompileTrace::Event event("verify.emission");
    TraceRequest(event, request);
    constexpr SpirvEmitter spirvEmitter;
    const auto again = spirvEmitter.Emit(program, inputInfo, bindings, options);
    if (!std::ranges::equal(again, produced)) throw std::runtime_error("APS5_VERIFY_SHADER_EMISSION: emitting shader 0x" + std::to_string(request.shader.codeAddress) + " again from the same program produced different SPIR-V");
    options.sharedFloatFunctions = true;
    const auto shared = spirvEmitter.Emit(program, inputInfo, bindings, options);
    options.sharedFloatFunctions = false;
    const auto inPlace = spirvEmitter.Emit(program, inputInfo, bindings, options);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    static_cast<void>(ValidateAndOptimizeSpirv(shared, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets, false));
    static_cast<void>(ValidateAndOptimizeSpirv(inPlace, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets, false));
#endif
    if (event.Active()) {
        event.Number("shared_words", shared.size()).Number("in_place_words", inPlace.size());
        const auto sharedShape = DescribeSpirvShape(shared);
        const auto inPlaceShape = DescribeSpirvShape(inPlace);
        event.Number("shared_functions", sharedShape.functions).Number("in_place_functions", inPlaceShape.functions).Number("shared_helper_words", sharedShape.helperFunctionWords).Number("in_place_helper_words", inPlaceShape.helperFunctionWords);
    }
    if (const auto difference = CompareSpirvInterfaces(inPlace, shared)) throw std::runtime_error("APS5_VERIFY_SHADER_EMISSION: shared float functions change the interface of shader 0x" + std::to_string(request.shader.codeAddress) + ": " + *difference);
    options.sharedFloatFunctions.reset();
    options.outlineImageModes = true;
    const auto outlined = spirvEmitter.Emit(program, inputInfo, bindings, options);
    options.outlineImageModes = false;
    const auto inlineModes = spirvEmitter.Emit(program, inputInfo, bindings, options);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    static_cast<void>(ValidateAndOptimizeSpirv(outlined, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets, false));
    static_cast<void>(ValidateAndOptimizeSpirv(inlineModes, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets, false));
#endif
    if (event.Active()) event.Number("outlined_image_words", outlined.size()).Number("inline_image_words", inlineModes.size()).Number("outlined_image_functions", DescribeSpirvShape(outlined).functions);
    if (const auto difference = CompareSpirvInterfaces(inlineModes, outlined)) throw std::runtime_error("APS5_VERIFY_SHADER_EMISSION: outlined image modes change the interface of shader 0x" + std::to_string(request.shader.codeAddress) + ": " + *difference);
    options.outlineImageModes.reset();
    options.sharedBufferHelpers = true;
    const auto sharedBuffers = spirvEmitter.Emit(program, inputInfo, bindings, options);
    options.sharedBufferHelpers = false;
    const auto resourceBuffers = spirvEmitter.Emit(program, inputInfo, bindings, options);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    static_cast<void>(ValidateAndOptimizeSpirv(sharedBuffers, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets, false));
    static_cast<void>(ValidateAndOptimizeSpirv(resourceBuffers, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets, false));
#endif
    if (event.Active()) event.Number("shared_buffer_words", sharedBuffers.size()).Number("resource_buffer_words", resourceBuffers.size()).Number("shared_buffer_functions", DescribeSpirvShape(sharedBuffers).functions);
    if (const auto difference = CompareSpirvInterfaces(resourceBuffers, sharedBuffers)) throw std::runtime_error("APS5_VERIFY_SHADER_EMISSION: shared formatted buffer helpers change the interface of shader 0x" + std::to_string(request.shader.codeAddress) + ": " + *difference);
}

CompiledVariant emitVariant(const RecompileRequest& request, IrProgram& program, bool consume = false) {
    InFlightIr inFlight(estimateProgramBytes(program));
    PrepareRewriteVerification(program);
    const auto inputInfo = RequestInputInfo(request);
    constexpr BindingAllocator bindingAllocator;
    bool registration = request.target.registrationModules && (request.shader.stage == ShaderStage::Vertex || request.shader.stage == ShaderStage::Fragment);
    program.Metadata().bindingLayoutComplete = false;
    const bool stencilReplay = request.shader.stage == ShaderStage::Fragment && std::ranges::any_of(program.Info().outputs, [](const StageOutput& output) { return output.kind == StageOutputKind::StencilRef; }) && std::ranges::find(request.target.supportedCapabilities, static_cast<std::uint32_t>(spv::CapabilityStencilExportEXT)) == request.target.supportedCapabilities.end();
    auto bindings = bindingAllocator.Allocate(program, request.layout, registration, stencilReplay);

    SpirvTargetOptions targetOptions {};
    targetOptions.vulkanVersion = request.target.vulkanVersion;
    targetOptions.spirvVersion = request.target.spirvVersion;
    targetOptions.subgroupSize = HostSubgroupSize(request) == 1u ? 1u : request.target.subgroupSize;
    targetOptions.bdaAbiVersion = request.target.bdaAbiVersion;
    targetOptions.supportedCapabilities = request.target.supportedCapabilities;
    targetOptions.supportedExtensions = request.target.supportedExtensions;
    targetOptions.nonConstantImageOffsets = request.target.nonConstantImageOffsets;
    targetOptions.narrowSubgroupClock = request.target.narrowSubgroupClock;
    targetOptions.float64DenormPreserve = request.target.float64DenormPreserve;
    targetOptions.registrationModules = registration;
    targetOptions.fragmentDescriptorSet = request.target.registrationModules;
    targetOptions.float16DenormPreserve = request.target.float16DenormPreserve;

    constexpr SpirvEmitter spirvEmitter;
    CompiledShaderArtifact result;
    result.variantId = nextVariantId();
    const auto emit = [&](const char* phase) {
        ShaderCompileTrace::Event emitEvent(phase);
        TraceRequest(emitEvent, request);
        auto words = spirvEmitter.Emit(program, inputInfo, bindings, targetOptions);
        if (emitEvent.Active()) {
            emitEvent.Flag("registration", targetOptions.registrationModules).Number("ir_bytes", estimateProgramBytes(program)).Number("spirv_words", words.size()).Hex("spirv_hash", ShaderCompileTrace::HashOf(std::span<const std::uint32_t>(words.data(), words.size())));
            TraceShape(emitEvent, words);
        }
        return words;
    };
    result.spirv = emit("emit.spirv");
    if (registration && result.spirv.size() > request.target.registrationWordLimit) {
        registration = false;
        program.Metadata().bindingLayoutComplete = false;
        bindings = bindingAllocator.Allocate(program, request.layout, false, stencilReplay);
        targetOptions.registrationModules = false;
        result.spirv = emit("emit.spirv_unregistered");
    }
    if (VerifyShaderEmission()) VerifyEmission(request, program, inputInfo, bindings, targetOptions, result.spirv.Words());
    result.registrationModule = registration;
    result.stencilReplaySafe = (request.shader.stage == ShaderStage::Vertex || stencilReplay) && StencilReplaySafe(program);
    if (stencilReplay) {
        result.stencilReferences = StencilReferenceValues(program);
        result.stencilReferenceDword = bindings.layout.stencilReferenceDword;
    }

    result.bdaAbiVersion = program.Info().usesDma || program.Info().usesFaultBuffer ? request.target.bdaAbiVersion : 0u;
    result.memoryOffsetDword = bindings.layout.memoryOffsetDword;
    result.shaderDataDwords = bindings.layout.ShaderDataDwords();
    result.imageMetadataDword = bindings.layout.ImageMetadataDword();
    result.runtimeImageCount = bindings.layout.runtimeImageCount;
    result.hostSubgroupSize = HostSubgroupSize(request);
    result.vertexOffsetSgpr = program.Info().vertexOffsetSgpr;
    result.instanceOffsetSgpr = program.Info().instanceOffsetSgpr;
    result.vertexOffsetShared = program.Info().vertexOffsetShared;
    result.instanceOffsetShared = program.Info().instanceOffsetShared;
    result.vertexOffsetConflict = program.Info().vertexOffsetConflict;
    result.instanceOffsetConflict = program.Info().instanceOffsetConflict;
    for (const auto& output : program.Info().outputs) {
        if (output.kind == StageOutputKind::Parameter) result.parameterExports.push_back(output.location);
    }
    if (request.shader.stage == ShaderStage::Fragment) {
        result.fragmentParameters = DescribeFragmentParameters(program, inputInfo);
        const auto& inputs = program.Info().inputs;
        const auto reads = [&](StageInputKind kind) { return std::any_of(inputs.begin(), inputs.end(), [&](const auto& input) { return input.kind == kind; }); };
        if (program.Metadata().barycentricEmulation) result.barycentricEmulation = {true, reads(StageInputKind::BaryCoordSmooth), reads(StageInputKind::BaryCoordNoPerspective)};
    }
    if (request.shader.stage == ShaderStage::Vertex || request.shader.stage == ShaderStage::Local) {
        if (inputInfo.vertex == nullptr) throw std::runtime_error("vertex input metadata is missing");
        for (const auto& input : program.Info().inputs) {
            if (input.kind != StageInputKind::Parameter) continue;
            if (input.location >= static_cast<std::uint32_t>(inputInfo.vertex->resourcesNum)) throw std::runtime_error("vertex attribute location exceeds resource count");
            result.vertexInputs.push_back({input.location, input.componentCount, inputInfo.vertex->resourcesDst[input.location].fetchIndex});
            if (inputInfo.vertex->fetchEmbedded) {
                for (const auto& block : program.Blocks()) {
                    for (const auto* instruction : block->Instructions()) {
                        if (instruction->Opcode() == IrOpcode::GetAttribute && instruction->Argument(0)->Resolve()->ImmediateU32() == input.location) result.vertexInputs.back().outputMask |= 1u << instruction->Argument(1)->Resolve()->ImmediateU32();
                    }
                }
            }
        }
    }

    auto compiledInfo = consume ? std::move(program).TakeCompiledInfo() : program.CompiledInfo();
    if (consume) {
        const IrProgram released = std::move(program);
        inFlight.Release();
    }
    const bool embedded = !result.vertexInputs.empty() && inputInfo.vertex != nullptr && inputInfo.vertex->fetchEmbedded;
    if (embedded) PrepareVertexInputSpecialization(result);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    {
        ShaderCompileTrace::Event optimizeEvent("emit.validate_optimize");
        const auto wordsIn = result.spirv.size();
        result.spirv = InternSpirv(ValidateAndOptimizeSpirv(result.spirv, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets, !embedded));
        if (optimizeEvent.Active()) optimizeEvent.Hex("address", request.shader.codeAddress).Flag("optimized", !embedded).Number("spirv_words_in", wordsIn).Number("spirv_words", result.spirv.size()).Hex("spirv_hash", ShaderCompileTrace::HashOf(result.spirv.Words()));
    }
    if (embedded) {
        std::array<std::uint32_t, ShaderVertexStageInfo::MaxResources> classes{};
        for (std::uint32_t kind = 1u; kind < 3u; ++kind) {
            classes.fill(kind);
            const auto specialized = SpecializeVertexInputTypes(result, classes);
            static_cast<void>(ValidateAndOptimizeSpirv(specialized, request.target.vulkanVersion, request.target.spirvVersion, request.target.nonConstantImageOffsets, false));
        }
    }
#endif

    return {request.layout, std::move(compiledInfo), std::move(static_cast<CompiledBindingLayout&>(bindings)), std::move(result)};
}

CompiledVariant compileVariant(const RecompileRequest& request, IrProgram program, std::exception_ptr* emissionFailure = nullptr) {
    return recordEmissionFailure(emissionFailure, [&] {
        {
            ShaderCompileTrace::Event interfaceEvent("emit.interface");
            TraceRequest(interfaceEvent, request);
            prepareInterface(request, program);
        }
        auto variant = emitVariant(request, program, true);
        return variant;
    });
}

bool sameLayout(const BindingLayout& left, const BindingLayout& right) {
    return left.descriptorSet == right.descriptorSet && left.firstBinding == right.firstBinding && left.pushConstantOffsetBytes == right.pushConstantOffsetBytes && left.pushConstantSizeBytes == right.pushConstantSizeBytes;
}

}

struct PreparedInterface {
    static CompiledVariant Compile(SourceEntry& source, const RecompileRequest& request, ShaderPreparationContext& preparation, std::exception_ptr* emissionFailure) {
        if (preparation.interfaceSource.get() != &source || preparation.interfaceProgram->Resources().shaderHash != request.shader.codeAddress) {
            preparation.interfaceSource.reset();
            preparation.interfaceProgram.reset();
            const bool retained = source.program != nullptr;
            auto program = source.program != nullptr ? std::move(source.program) : std::make_unique<IrProgram>(PrepareResourceProgram(request, &preparation));
            if (retained) countProgram(false);
            recordEmissionFailure(emissionFailure, [&] { prepareInterface(request, *program); });
            preparation.interfaceSource = source.shared_from_this();
            preparation.interfaceProgram = std::move(program);
        }
        auto& program = *preparation.interfaceProgram;
        return recordEmissionFailure(emissionFailure, [&] {
            auto variant = emitVariant(request, program);
            variant.info = program.CompiledInfo();
            return variant;
        });
    }
};

namespace {

std::shared_ptr<const CompiledVariant> findOrCompileVariant(SourceEntry& source, const RecompileRequest& request, bool& cacheHit, ShaderPreparationContext* preparation = nullptr) {
    for (const auto& candidate : source.variants) {
        if (sameLayout(candidate->layout, request.layout)) {
            cacheHit = true;
            return candidate;
        }
    }
    for (const auto& failure : source.emissionFailures) {
        if (failure.codeAddress == request.shader.codeAddress && sameLayout(failure.layout, request.layout)) std::rethrow_exception(failure.failure);
    }
    cacheHit = false;
    const bool disk = ShaderDiskCache::Enabled() && !DebugProbeActive();
    std::vector<std::byte> diskKey;
    std::shared_ptr<const CompiledVariant> variant;
    const bool spill = disk && SpillPreparedSpirv();
    if (disk) {
        ShaderDiskCache::BuildKey(request, HostSubgroupSize(request), diskKey);
        CompiledVariant loaded;
        if (ShaderDiskCache::Load(diskKey, loaded)) {
            loaded.layout = request.layout;
            loaded.artifact.variantId = nextVariantId();
            if (spill) loaded.artifact.spirv = loaded.artifact.spirv.Spill(diskKey, false);
            variant = std::make_shared<const CompiledVariant>(std::move(loaded));
            NoteShaderManifest(request, *variant, "disk");
        }
    }
    if (variant == nullptr) {
        static const bool layoutPair = std::getenv("APS5_NO_PERF_LAYOUT_PAIR") == nullptr;
        std::exception_ptr emissionFailure;
        try {
            if (layoutPair && preparation != nullptr) {
                variant = std::make_shared<const CompiledVariant>(PreparedInterface::Compile(source, request, *preparation, &emissionFailure));
            } else {
                const bool retained = source.program != nullptr;
                auto program = source.program != nullptr ? std::move(*source.program) : PrepareResourceProgram(request, preparation);
                source.program.reset();
                if (retained) countProgram(false);
                variant = std::make_shared<const CompiledVariant>(compileVariant(request, std::move(program), &emissionFailure));
            }
        } catch (...) {
            if (emissionFailure != nullptr && FailureMemo()) source.emissionFailures.push_back({request.shader.codeAddress, request.layout, emissionFailure});
            throw;
        }
        NoteShaderManifest(request, *variant, "compiled");
        if (spill) {
            auto retained = std::make_shared<CompiledVariant>(*variant);
            retained->artifact.spirv = variant->artifact.spirv.Spill(diskKey, true);
            const auto spilled = retained->artifact.spirv;
            ShaderDiskCache::Store(std::move(diskKey), variant, [spilled] { spilled.Unpin(); });
            variant = std::move(retained);
        } else if (disk) {
            if (auto persisted = ShaderDiskCache::Persist(diskKey, *variant)) variant = std::move(persisted);
            else ShaderDiskCache::Store(std::move(diskKey), variant);
        }
    }
    if (source.program != nullptr) {
        source.program.reset();
        countProgram(false);
    }
    source.variants.push_back(variant);
    retainedCounters().variants.fetch_add(1, std::memory_order_relaxed);
    retainedCounters().variantMetadataBytes.fetch_add(estimateVariantMetadataBytes(*variant), std::memory_order_relaxed);
    const auto spirvBytes = variant->artifact.spirv.WordCount() * sizeof(std::uint32_t);
    if (variant->artifact.spirv.Spilled()) {
        retainedCounters().spilledVariants.fetch_add(1, std::memory_order_relaxed);
        retainedCounters().spilledSpirvBytes.fetch_add(spirvBytes, std::memory_order_relaxed);
    } else {
        retainedCounters().variantSpirvBytes.fetch_add(spirvBytes, std::memory_order_relaxed);
    }
    return variant;
}

RecompileResult materializeVariant(SourceEntry& source, const RecompileRequest& request, const ResourceSnapshot& snapshot) {
    std::shared_ptr<const CompiledVariant> variant;
    bool cacheHit = false;
    {
        std::lock_guard lock(source.mutex);
        variant = findOrCompileVariant(source, request, cacheHit);
    }
    auto result = materializeResult(*variant, request, snapshot);
    result.cacheHit = cacheHit;
    return result;
}

RecompileResult RecompileImpl(const RecompileRequest& request) {
    static_cast<void>(RequestInputInfo(request));
    RequestMemoryView memory(request.context.memory);
    const auto runtime = memory.MakeRuntime(request.context.userData, request.shader.codeAddress);
    ResourceSnapshot snapshot;
    constexpr ResourceMaterializer materializer;
    if (!request.useCache) {
        auto program = PrepareResourceProgram(request);
        const auto plan = materializer.ExtractPlan(program);
        materializer.Materialize(plan, runtime, snapshot);
        const auto variant = compileVariant(request, std::move(program));
        return materializeResult(variant, request, snapshot);
    }
    const auto source = getSource(request);
    materializer.Materialize(*usePlan(*source), runtime, snapshot);
    return materializeVariant(*source, request, snapshot);
}

// APS5_NO_RESULT_MEMO=1: every Recompile(request, capture) materializes its own result as before.
bool ResultMemo() {
    static const bool resultMemo = std::getenv("APS5_NO_RESULT_MEMO") == nullptr;
    return resultMemo;
}

std::size_t ResultMemoEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_RESULT_MEMO_ENTRIES");
        if (text == nullptr || *text == '\0') return std::size_t{256};
        char* end = nullptr;
        const auto value = std::strtoull(text, &end, 10);
        if (end == text || *end != '\0' || value == 0) throw std::runtime_error("APS5_RESULT_MEMO_ENTRIES is not a positive whole number: " + std::string(text));
        return static_cast<std::size_t>(value);
    }();
    return entries;
}

struct ResultMemoCounters {
    std::atomic<std::uint64_t> hits{0}, misses{0}, evictions{0}, populateNanoseconds{0}, memoResults{0}, memoBytes{0};
    std::atomic<std::int64_t> lastReport{0};
};

std::uint64_t estimateResultBytes(const RecompileResult& result) {
    std::uint64_t bytes = sizeof(RecompileResult) + result.specialization.size() * sizeof(PipelineSpecializationConstant) + result.pushConstants.size() + result.vertexAttributes.size() * sizeof(VertexAttribute);
    bytes += result.vertexInputPatches.size() * sizeof(VertexInputPatch) + result.runtimeImageResources.size() * sizeof(std::uint32_t) + result.vertexInputs.size() * sizeof(VertexInput) + result.parameterExports.size() * sizeof(std::uint32_t) + result.fragmentParameters.size() * sizeof(FragmentParameter);
    for (const auto& binding : result.bindings) {
        bytes += sizeof(DescriptorBinding) + (binding.guestDescriptor.size() + binding.imageSamplers.size()) * sizeof(std::uint32_t);
        for (const auto* flags : {&binding.samplerDepthCompare, &binding.imageWritten, &binding.imageDepthCompare, &binding.imageAtomic, &binding.imageAtomic64, &binding.bufferAtomic, &binding.bufferWritten, &binding.samplerUnnormalized, &binding.samplerIntegerBorder, &binding.imageUnnormalized, &binding.imageUnnormalizedBaseLevel, &binding.bufferRead}) {
            bytes += (flags->size() + 63u) / 64u * sizeof(std::uint64_t);
        }
    }
    return bytes;
}

ResultMemoCounters& resultMemoCounters() {
    static ResultMemoCounters counters;
    return counters;
}

void reportResultMemo() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    auto& counters = resultMemoCounters();
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counters.lastReport.load(std::memory_order_relaxed);
    if (last == 0) {
        counters.lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 10'000'000'000ll || !counters.lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    const auto hits = counters.hits.exchange(0, std::memory_order_relaxed);
    const auto misses = counters.misses.exchange(0, std::memory_order_relaxed);
    const auto evictions = counters.evictions.exchange(0, std::memory_order_relaxed);
    const auto populate = counters.populateNanoseconds.exchange(0, std::memory_order_relaxed);
    std::fprintf(stderr, "[recompile] result memo (10 s): %llu hits, %llu misses (%.1f%% hits), Populate %.1f us per miss / %.1f ms in total, %llu evictions\n", static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses), hits + misses != 0 ? 100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses) : 0.0, misses != 0 ? static_cast<double>(populate) / 1000.0 / static_cast<double>(misses) : 0.0, static_cast<double>(populate) / 1e6, static_cast<unsigned long long>(evictions));
}

// Everything materializeResult reads besides the variant: the snapshot (the descriptor words, the
// flattened SRT, the user data, the uniform fill) and, for the vertex family, the V# table the
// attributes are resolved from.
std::uint64_t snapshotHash(const RecompileRequest& request, const ResourceSnapshot& snapshot) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ull;
    };
    const auto mixWords = [&](std::span<const std::uint32_t> words) {
        mix(words.size());
        for (const auto word : words) mix(word);
    };
    const auto mixDescriptors = [&](const std::vector<DescriptorValue>& values) {
        mix(values.size());
        for (const auto& value : values) {
            mix(value.dwordCount);
            for (std::uint32_t i = 0; i < value.dwordCount && i < value.dwords.size(); ++i) mix(value.dwords[i]);
        }
    };
    mixDescriptors(snapshot.buffers);
    mixDescriptors(snapshot.images);
    mixDescriptors(snapshot.samplers);
    mixWords(snapshot.flattenedSrt);
    mixWords(snapshot.userData);
    mix(static_cast<std::uint64_t>(snapshot.uniformFill.kind));
    mix(snapshot.uniformFill.resource);
    for (const auto stride : snapshot.uniformFill.groupStride) mix(stride);
    mix(snapshot.uniformFill.words);
    mix(snapshot.uniformFill.value);
    const auto& tables = snapshot.tables;
    mix(tables.shader);
    mix(tables.tables.size());
    for (const auto& table : tables.tables) {
        mix(table.base);
        mix(table.size);
        mix(table.records);
        mix(table.keys);
        mix(table.fault);
        mix(table.outside ? 1u : 0u);
        mixWords(table.codes);
        mix(table.readRanges.size());
        for (const auto& [base, size] : table.readRanges) {
            mix(base);
            mix(size);
        }
    }
    mixDescriptors(tables.words);
    mix(tables.ranges.size());
    for (const auto& [base, size] : tables.ranges) {
        mix(base);
        mix(size);
    }
    for (const auto threads : partialThreads(request)) mix(threads);
    mix(request.layout.pushConstantOffsetBytes);
    if (request.context.pixel) {
        for (const auto mapping : request.context.pixel->targetExportMapping) mix(mapping);
        for (const auto packing : request.context.pixel->targetExportPacking) mix(static_cast<std::uint64_t>(packing));
        mix(request.context.pixel->dualSourceBlend);
        mix(static_cast<std::uint64_t>(request.context.pixel->rasterPrimitive));
        mix(request.context.pixel->layerExported);
    }
    if (request.context.vertex) {
        const auto& vertex = *request.context.vertex;
        const auto count = std::min<std::uint32_t>(vertex.resourcesNum, ShaderVertexStageInfo::MaxResources);
        mix(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            for (const auto field : vertex.resources[i].fields) mix(field);
            mix(vertex.resourcesDst[i].fetchIndex);
        }
    } else {
        mix(1ull << 32u);
    }
    return hash;
}

// The memo'd result of `source`'s variant for the snapshot (design13 R5): a hit returns the shared
// object, a miss materializes outside the source mutex and inserts (a concurrent miss's object is
// as good). `memoHit` reports the hit.
std::shared_ptr<const RecompileResult> materializePreparedMemoized(SourceEntry& source, const std::shared_ptr<const CompiledVariant>& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot, bool cacheHit, bool* memoHit) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto hash = snapshotHash(request, snapshot);
    std::uint64_t index = 0;
    auto& counters = resultMemoCounters();
    {
        std::lock_guard lock(source.mutex);
        index = (variant->artifact.variantId * 0x9e3779b97f4a7c15ull) ^ hash;
        const auto found = source.memoIndex.find(index);
        if (found != source.memoIndex.end() && found->second->variantId == variant->artifact.variantId && found->second->hash == hash) {
            source.memo.splice(source.memo.begin(), source.memo, found->second);
            counters.hits.fetch_add(1, std::memory_order_relaxed);
            if (memoHit != nullptr) *memoHit = true;
            reportResultMemo();
            return found->second->result;
        }
    }
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto result = std::make_shared<RecompileResult>(materializeResult(*variant, request, snapshot));
    result->cacheHit = cacheHit;
    if (profile) counters.populateNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
    counters.misses.fetch_add(1, std::memory_order_relaxed);
    std::shared_ptr<const RecompileResult> shared = std::move(result);
    {
        std::lock_guard lock(source.mutex);
        const auto found = source.memoIndex.find(index);
        if (found != source.memoIndex.end()) {
            if (found->second->variantId == variant->artifact.variantId && found->second->hash == hash) {
                source.memo.splice(source.memo.begin(), source.memo, found->second);
                shared = found->second->result;
            } else {
                counters.memoBytes.fetch_sub(found->second->bytes, std::memory_order_relaxed);
                source.memo.erase(found->second);
                source.memoIndex.erase(found);
                counters.memoResults.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        if (source.memoIndex.find(index) == source.memoIndex.end()) {
            const auto bytes = estimateResultBytes(*shared);
            counters.memoResults.fetch_add(1, std::memory_order_relaxed);
            counters.memoBytes.fetch_add(bytes, std::memory_order_relaxed);
            source.memo.push_front({variant->artifact.variantId, hash, shared, bytes});
            source.memoIndex.emplace(index, source.memo.begin());
            while (source.memo.size() > ResultMemoEntries()) {
                const auto& last = source.memo.back();
                source.memoIndex.erase((last.variantId * 0x9e3779b97f4a7c15ull) ^ last.hash);
                counters.memoBytes.fetch_sub(last.bytes, std::memory_order_relaxed);
                source.memo.pop_back();
                counters.evictions.fetch_add(1, std::memory_order_relaxed);
                counters.memoResults.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
    reportResultMemo();
    return shared;
}

std::shared_ptr<const RecompileResult> materializeMemoized(SourceEntry& source, const RecompileRequest& request, const ResourceSnapshot& snapshot, bool* memoHit) {
    std::shared_ptr<const CompiledVariant> variant;
    bool cacheHit = false;
    {
        std::lock_guard lock(source.mutex);
        variant = findOrCompileVariant(source, request, cacheHit);
    }
    return materializePreparedMemoized(source, variant, request, snapshot, cacheHit, memoHit);
}

// The capture already resolved the source entry (stage input validation included) and materialized
// the request over exactly the words the driver captured, so neither is repeated here.
std::shared_ptr<const RecompileResult> RecompileImpl(const RecompileRequest& request, const ResourceCapture& capture, bool* memoHit) {
    if (!request.useCache || capture.source == nullptr) {
        auto program = PrepareResourceProgram(request);
        const auto variant = compileVariant(request, std::move(program));
        return std::make_shared<const RecompileResult>(materializeResult(variant, request, capture.snapshot));
    }
    if (!ResultMemo()) return std::make_shared<const RecompileResult>(materializeVariant(*capture.source, request, capture.snapshot));
    return materializeMemoized(*capture.source, request, capture.snapshot, memoHit);
}

template <typename Impl>
auto recompileReporting(const RecompileRequest& request, Impl&& impl) -> decltype(impl()) {
    try {
        return impl();
    } catch (const std::exception& e) {
        constexpr auto requestSerializer = RequestSerializer{};
        const auto inputInfo = "\nRecompileRequest:\n" + requestSerializer.Serialize(request);
        throw std::runtime_error(std::string("ShaderRecompiler::Recompile: ") + e.what() + inputInfo);
    } catch (...) {
        throw std::runtime_error("ShaderRecompiler::Recompile: unknown exception");
    }
}

}

namespace {

const IrValue& storedValue(const IrValue& value) {
    return value;
}

const IrValue& storedValue(const std::unique_ptr<IrValue>& value) {
    return *value;
}

}

struct SpilledSpirv {
    std::mutex mutex;
    std::vector<std::byte> key;
    std::size_t wordCount = 0;
    std::shared_ptr<std::vector<std::uint32_t>> pinned;
    std::weak_ptr<std::vector<std::uint32_t>> cached;
    std::shared_ptr<const void> pinnedOwner;
    std::weak_ptr<const void> mappedOwner;
    std::span<const std::uint32_t> mappedView;
};

std::size_t SharedSpirv::WordCount() const {
    if (words != nullptr) return words->size();
    return spilled != nullptr ? spilled->wordCount : view.size();
}

std::span<const std::uint32_t> SharedSpirv::SpilledWords() {
    throw std::logic_error("SPIR-V kept in the shader disk cache is read without SharedSpirv::Resolved");
}

SharedSpirv SharedSpirv::Spill(std::vector<std::byte> key, bool pinned) const {
    if (Spilled() || (words == nullptr && owner == nullptr && view.empty())) return *this;
    auto source = std::make_shared<SpilledSpirv>();
    source->key = std::move(key);
    source->wordCount = WordCount();
    source->cached = words;
    source->mappedOwner = owner;
    source->mappedView = view;
    if (pinned) {
        source->pinned = words;
        source->pinnedOwner = owner;
    }
    SharedSpirv result;
    result.spilled = std::move(source);
    return result;
}

void SharedSpirv::Unpin() const {
    if (spilled == nullptr) return;
    std::lock_guard lock(spilled->mutex);
    spilled->pinned.reset();
    spilled->pinnedOwner.reset();
}

SharedSpirv SharedSpirv::Resolved() const {
    if (words != nullptr || spilled == nullptr) return *this;
    SharedSpirv result;
    std::lock_guard lock(spilled->mutex);
    result.words = spilled->pinned != nullptr ? spilled->pinned : spilled->cached.lock();
    if (result.words != nullptr) return result;
    result.owner = spilled->pinnedOwner != nullptr ? spilled->pinnedOwner : spilled->mappedOwner.lock();
    if (result.owner != nullptr) {
        result.view = spilled->mappedView;
        return result;
    }
    CompiledVariant loaded;
    if (!ShaderDiskCache::Reload(spilled->key, loaded) || loaded.artifact.spirv.WordCount() != spilled->wordCount) throw std::runtime_error("SPIR-V kept in the shader disk cache could not be read back from it");
    result = std::move(loaded.artifact.spirv);
    spilled->cached = result.words;
    spilled->mappedOwner = result.owner;
    spilled->mappedView = result.view;
    retainedCounters().spirvReloads.fetch_add(1, std::memory_order_relaxed);
    retainedCounters().spirvReloadBytes.fetch_add(result.WordCount() * sizeof(std::uint32_t), std::memory_order_relaxed);
    return result;
}

std::uint64_t EstimatePlanBytes(const IrResourcePlan& plan, std::uint64_t* values) {
    std::uint64_t count = 0;
    std::uint64_t bytes = sizeof(IrResourcePlan);
    for (const auto& stored : plan.valueStorage) {
        ++count;
        bytes += estimateValueBytes(storedValue(stored));
    }
    for (const auto& block : plan.blockStorage) {
        if (block != nullptr) bytes += sizeof(IrBlock) + (block->Predecessors().size() + block->Successors().size()) * sizeof(void*);
    }
    bytes += plan.memoryInfo.size() * sizeof(MemoryInfo) + plan.descriptorSources.size() * sizeof(DescriptorSource) + plan.controlFlow.size() * sizeof(ResourceBlock) + plan.srtReads.size() * sizeof(SrtRead);
    bytes += (plan.materializationSources.size() + plan.guardedSrtSlots.size()) * sizeof(std::uint32_t) + plan.cleanFlatSlots.size() + plan.pureFlatSlots.size();
    if (values != nullptr) *values = count;
    return bytes;
}

RetainedShaderMemory GetRetainedShaderMemory() {
    const auto& counters = retainedCounters();
    const auto load = [](const std::atomic<std::uint64_t>& value) { return value.load(std::memory_order_relaxed); };
    return {load(counters.sources), load(counters.plans), load(counters.planValues), load(counters.planBytes), load(counters.spilledPlans), load(counters.spilledPlanBytes), load(counters.planReloads), load(counters.programs), load(counters.variants), load(counters.variantSpirvBytes), load(counters.variantMetadataBytes), load(counters.spilledVariants), load(counters.spilledSpirvBytes), load(counters.spirvReloads), load(counters.spirvReloadBytes), load(resultMemoCounters().memoResults), load(resultMemoCounters().memoBytes), load(counters.bindingPlans), load(counters.specializedModules), load(counters.specializedSpirvBytes), load(counters.registrationModules), load(counters.irPrograms), load(counters.irBytes), load(counters.irPeakBytes)};
}

std::shared_ptr<const IrResourcePlan> GetResourcePlan(const RecompileRequest& request) {
    static_cast<void>(RequestInputInfo(request));
    if (request.useCache) return usePlan(*getSource(request));
    return makeResourcePlan(request);
}

namespace {

// The capture's materialization; with pure flat slots in the plan the walk's read addresses are
// traced into the capture (ResourceCapture::readTrace).
void noteBufferBaseWords(ResourceCapture& capture) {
    const auto& plan = *capture.plan;
    const auto& use = capture.userDataUse;
    for (std::uint32_t buffer = 0; buffer < plan.info.buffers.size() && buffer < plan.materializationSources.size(); ++buffer) {
        const auto source = plan.materializationSources[buffer];
        const void* lead = nullptr;
        bool single = true;
        for (const auto& [index, instruction] : use.sourceLeads) {
            if (index != source) continue;
            if (lead != nullptr && lead != instruction) single = false;
            lead = instruction;
        }
        if (lead == nullptr || !single || std::find(use.operandReads.begin(), use.operandReads.end(), lead) != use.operandReads.end()) continue;
        std::uint64_t address = 0;
        std::size_t found = 0;
        for (std::size_t read = 0; read < use.readInstructions.size() && read < use.reads.size(); ++read) {
            if (use.readInstructions[read] != lead) continue;
            if (found != 0 && use.reads[read].first != address) single = false;
            address = use.reads[read].first;
            ++found;
        }
        for (std::size_t read = 0; read < use.readInstructions.size() && read < use.reads.size(); ++read) {
            if (use.reads[read].first == address && use.readInstructions[read] != lead) single = false;
        }
        std::int64_t flatOffset = -1;
        for (const auto& read : plan.srtReads) {
            if (read.value == nullptr || read.value->Resolve() != lead) continue;
            if (flatOffset >= 0) single = false;
            flatOffset = read.flatOffset;
        }
        if (found != 0 && single) capture.bufferBaseWords.push_back({buffer, address, flatOffset});
    }
}

std::shared_ptr<const std::vector<std::pair<const void*, std::uint32_t>>> baseOnlyReads(const std::shared_ptr<const IrResourcePlan>& plan) {
    static std::mutex mutex;
    static std::unordered_map<const IrResourcePlan*, std::pair<std::weak_ptr<const IrResourcePlan>, std::shared_ptr<const std::vector<std::pair<const void*, std::uint32_t>>>>> memo;
    {
        std::lock_guard lock(mutex);
        if (const auto found = memo.find(plan.get()); found != memo.end() && found->second.first.lock() == plan) return found->second.second;
    }
    std::unordered_map<const IrValue*, std::uint32_t> slots;
    for (const auto& read : plan->srtReads) {
        if (read.value != nullptr) slots.emplace(read.value->Resolve(), read.flatOffset);
    }
    std::vector<std::pair<const void*, std::uint32_t>> reads;
    for (const auto* raw : Detail::ComputeBaseOnlyReads(*plan)) {
        const auto slot = slots.find(raw);
        reads.emplace_back(raw, slot != slots.end() ? slot->second : NoBaseSlot);
    }
    auto computed = std::make_shared<const std::vector<std::pair<const void*, std::uint32_t>>>(std::move(reads));
    std::lock_guard lock(mutex);
    if (memo.size() >= 65536) {
        for (auto it = memo.begin(); it != memo.end();) it = it->second.first.expired() ? memo.erase(it) : std::next(it);
    }
    memo[plan.get()] = {plan, computed};
    return computed;
}

void noteBaseSlots(ResourceCapture& capture) {
    if (capture.plan == nullptr) return;
    const auto reads = baseOnlyReads(capture.plan);
    const auto& use = capture.userDataUse;
    for (const auto& [inst, slot] : *reads) {
        std::optional<std::uint64_t> address;
        bool single = true;
        for (std::size_t read = 0; read < use.readInstructions.size() && read < use.reads.size(); ++read) {
            if (use.readInstructions[read] != inst) continue;
            if (address && *address != use.reads[read].first) single = false;
            address = use.reads[read].first;
        }
        if (address && single) capture.baseSlots.emplace_back(slot, *address);
    }
}

void materializeCapture(ResourceCapture& capture, const SrtRuntime& runtime) {
    const auto& plan = *capture.plan;
    SrtRuntime traced = runtime;
    traced.userDataUse = &capture.userDataUse;
    if (std::none_of(plan.pureFlatSlots.begin(), plan.pureFlatSlots.end(), [](std::uint8_t pure) { return pure != 0u; })) {
        ResourceMaterializer{}.Materialize(plan, traced, capture.snapshot);
        noteBufferBaseWords(capture);
        noteBaseSlots(capture);
        return;
    }
    traced.readTrace = &capture.readTrace;
    ResourceMaterializer{}.Materialize(plan, traced, capture.snapshot);
    noteBufferBaseWords(capture);
    noteBaseSlots(capture);
    auto& other = capture.readTrace.otherReads;
    std::sort(other.begin(), other.end());
    other.erase(std::unique(other.begin(), other.end()), other.end());
}

}

std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // Validates the stage inputs once per request, as GetResourcePlan and Recompile(request) do.
    static_cast<void>(RequestInputInfo(request));
    auto capture = std::make_shared<ResourceCapture>();
    if (request.useCache) {
        capture->source = getSource(request);
        capture->plan = usePlan(*capture->source);
    } else {
        capture->plan = makeResourcePlan(request);
    }
    if (profile) capture->sourceNanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    materializeCapture(*capture, runtime);
    return capture;
}

std::shared_ptr<const SourceHandle> ResolveSource(const RecompileRequest& request) {
    if (!request.useCache) return nullptr;
    static_cast<void>(RequestInputInfo(request));
    return std::make_shared<const SourceHandle>(SourceHandle{getSource(request)});
}

std::shared_ptr<const SourceHandle> PrepareShader(const RecompileRequest& request) {
    return PrepareShader(request, nullptr);
}

std::shared_ptr<const SourceHandle> PrepareShader(const RecompileRequest& request, ShaderPreparationContext* preparation) {
    return recompileReporting(request, [&]() -> std::shared_ptr<const SourceHandle> {
        static_cast<void>(RequestInputInfo(request));
        auto handle = std::make_shared<SourceHandle>();
        handle->source = getSource(request, preparation, false);
        RecompileCacheKey::BuildInterface(request, handle->staticKey);
        handle->staticKey.push_back(HostSubgroupSize(request));
        bool cacheHit = false;
        std::lock_guard lock(handle->source->mutex);
        handle->artifact = findOrCompileVariant(*handle->source, request, cacheHit, preparation);
        if (SpillResourcePlans()) {
            handle->source->planSpillRequested = true;
            spillPlanLocked(*handle->source);
        }
        return handle;
    });
}

bool MatchesPreparedShader(const RecompileRequest& request, const SourceHandle& handle) {
    struct PreparedKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, PreparedKeyStorage>();
    BuildPreparedShaderKey(request, key);
    return MatchesPreparedShader(request, handle, key);
}

void BuildPreparedShaderKey(const RecompileRequest& request, std::vector<std::uint64_t>& key) {
    RecompileCacheKey::BuildInterface(request, key);
    key.push_back(HostSubgroupSize(request));
}

bool MatchesPreparedShader(const RecompileRequest& request, const SourceHandle& handle, std::span<const std::uint64_t> key) {
    if (handle.source == nullptr || handle.artifact == nullptr) return false;
    const auto& layout = request.layout;
    const auto& prepared = handle.artifact->bindings.layout;
    if (layout.descriptorSet != handle.artifact->layout.descriptorSet || layout.firstBinding != handle.artifact->layout.firstBinding) return false;
    if (layout.pushConstantOffsetBytes % 4u != 0u || layout.pushConstantSizeBytes % 4u != 0u || layout.pushConstantOffsetBytes > NativePushConstantSize || layout.pushConstantSizeBytes > NativePushConstantSize - layout.pushConstantOffsetBytes) return false;
    const auto words = prepared.ShaderDataDwords();
    const bool usesPush = prepared.runtimeImageCount == 0u && words != 0u && words <= layout.pushConstantSizeBytes / 4u;
    if (prepared.UsesPushData() != usesPush) return false;
    if (!handle.artifact->artifact.stencilReferences.empty() && (prepared.stencilReferenceDword >= layout.pushConstantSizeBytes / 4u || prepared.stencilReferenceDword != (usesPush ? words : 0u))) return false;
    if (!std::ranges::equal(key, handle.staticKey) || handle.source->code.size() != request.shader.code.size()) return false;
    return handle.source->code.data() == request.shader.code.data() || std::ranges::equal(handle.source->code, request.shader.code);
}

std::span<const std::uint32_t> GetPreparedCode(const SourceHandle& handle) {
    if (handle.source == nullptr || handle.artifact == nullptr) throw std::runtime_error("ShaderRecompiler: prepared artifact is missing");
    return handle.source->code;
}

PreparedShaderInvocation::PreparedShaderInvocation(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle) : request(request), handle(handle) {
    this->request.context.linkedCallFallbacks = handle->source->linkedCallFallbacks;
}

std::optional<PreparedShaderInvocation> PreparedShaderInvocation::TryCreate(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle) {
    struct PreparedKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, PreparedKeyStorage>();
    BuildPreparedShaderKey(request, key);
    return TryCreate(request, handle, key);
}

std::optional<PreparedShaderInvocation> PreparedShaderInvocation::TryCreate(const RecompileRequest& request, const std::shared_ptr<const SourceHandle>& handle, std::span<const std::uint64_t> key) {
    if (handle == nullptr || !MatchesPreparedShader(request, *handle, key)) return std::nullopt;
    if (request.shader.stage != ShaderStage::Compute && request.shader.stage != ShaderStage::Fragment) static_cast<void>(RequestInputInfo(request));
    return PreparedShaderInvocation(request, handle);
}

std::shared_ptr<const ResourceCapture> PreparedShaderInvocation::Capture(const SrtRuntime& runtime) const {
    auto capture = std::make_shared<ResourceCapture>();
    capture->source = handle->source;
    capture->plan = usePlan(*handle->source);
    materializeCapture(*capture, runtime);
    return capture;
}

std::shared_ptr<const RecompileResult> PreparedShaderInvocation::Materialize(const ResourceCapture& capture) const {
    if (capture.source != handle->source || capture.plan != usePlan(*handle->source)) throw std::runtime_error("ShaderRecompiler: resource capture belongs to another prepared shader");
    if (request.useCache && ResultMemo()) return materializePreparedMemoized(*handle->source, handle->artifact, request, capture.snapshot, true, nullptr);
    auto result = std::make_shared<RecompileResult>(materializeResult(*handle->artifact, request, capture.snapshot));
    result->cacheHit = true;
    return result;
}

std::shared_ptr<const ResourceCapture> PreparedShaderInvocation::CaptureValues(const SrtRuntime& runtime) const {
    auto capture = std::make_shared<ResourceCapture>();
    capture->source = handle->source;
    capture->plan = usePlan(*handle->source);
    SrtRuntime values = runtime;
    values.userDataUse = nullptr;
    values.readTrace = nullptr;
    values.compiledWalk = true;
    ResourceMaterializer{}.Materialize(*capture->plan, values, capture->snapshot);
    return capture;
}

struct MaterializedStorage {
    RecompileResult result;
    std::uint64_t retainedBytes = 0;
};

struct MaterializedTemplate {
    std::vector<std::uint64_t> key;
    std::shared_ptr<const RecompileResult> skeleton;
    MaterializedParts parts;
    mutable std::atomic<MaterializedStorage*> idle{nullptr};

    ~MaterializedTemplate();
};

namespace {

void specializationKeyWords(const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot, std::vector<std::uint64_t>& key) {
    bindingPlanKeyWords(variant, snapshot, request.context.pixel ? std::span<const std::uint8_t>(request.context.pixel->targetExportMapping) : std::span<const std::uint8_t>{}, key);
    key.push_back(request.layout.pushConstantOffsetBytes);
    key.push_back(request.target.registrationModules);
    if (request.context.pixel) {
        const auto& pixel = *request.context.pixel;
        key.push_back(1);
        for (const auto packing : pixel.targetExportPacking) key.push_back(static_cast<std::uint64_t>(packing));
        key.push_back(pixel.dualSourceBlend);
        key.push_back(static_cast<std::uint64_t>(pixel.rasterPrimitive));
        key.push_back(pixel.layerExported);
    } else {
        key.push_back(0);
    }
    if (request.context.vertex) {
        const auto& vertex = *request.context.vertex;
        key.push_back(1 + vertex.resourcesNum);
        key.push_back(vertex.fetchEmbedded);
        for (const auto& input : variant.artifact.vertexInputs) key.push_back(input.location < vertex.resources.size() ? vertex.resources[input.location].fields[3] : ~std::uint64_t {0});
    } else {
        key.push_back(0);
    }
    for (const auto threads : partialThreads(request)) key.push_back(threads);
}

constexpr std::size_t MaxMaterializedTemplates = 32;
constexpr std::uint64_t MaxMaterializedStorageBytes = 256u * 1024u;
constexpr std::uint64_t MaterializedStorageBudget = 32u * 1024u * 1024u;

struct MaterializeValuesCounters {
    std::atomic<std::uint64_t> templateHits{0}, templateMisses{0};
    std::atomic<std::uint64_t> storageCreated{0}, storageReused{0}, storageRetainedBytes{0}, storageRefused{0};
};

MaterializeValuesCounters& materializeValuesCounters() {
    static auto* const counters = new MaterializeValuesCounters();
    return *counters;
}

template<typename TElement>
std::uint64_t retainedVectorBytes(const std::vector<TElement>& values) {
    return values.capacity() * sizeof(TElement);
}

std::uint64_t materializedStorageBytes(const RecompileResult& result) {
    std::uint64_t bytes = sizeof(MaterializedStorage) + retainedVectorBytes(result.specialization) + retainedVectorBytes(result.bindings) + retainedVectorBytes(result.pushConstants) + retainedVectorBytes(result.vertexAttributes);
    bytes += retainedVectorBytes(result.vertexInputPatches) + retainedVectorBytes(result.runtimeImageResources) + retainedVectorBytes(result.vertexInputs) + retainedVectorBytes(result.parameterExports) + retainedVectorBytes(result.fragmentParameters);
    bytes += retainedVectorBytes(result.shaderDataUserWords) + retainedVectorBytes(result.bufferBaseSlots);
    for (const auto& binding : result.bindings) {
        bytes += retainedVectorBytes(binding.guestDescriptor) + retainedVectorBytes(binding.imageSamplers) + retainedVectorBytes(binding.bufferReadRanges);
        for (const auto& range : binding.bufferReadRanges) bytes += retainedVectorBytes(range.ranges);
        for (const auto* flags : {&binding.samplerDepthCompare, &binding.imageWritten, &binding.imageDepthCompare, &binding.imageAtomic, &binding.imageAtomic64, &binding.bufferAtomic, &binding.bufferWritten, &binding.samplerUnnormalized, &binding.samplerIntegerBorder, &binding.imageUnnormalized, &binding.imageUnnormalizedBaseLevel, &binding.bufferRead}) bytes += (flags->capacity() + 63u) / 64u * sizeof(std::uint64_t);
    }
    return bytes;
}

std::unique_ptr<MaterializedStorage> acquireMaterializedStorage(const MaterializedTemplate& materialized) {
    auto& counters = materializeValuesCounters();
    if (auto* result = materialized.idle.exchange(nullptr, std::memory_order_acq_rel)) {
        counters.storageRetainedBytes.fetch_sub(result->retainedBytes, std::memory_order_relaxed);
        counters.storageReused.fetch_add(1, std::memory_order_relaxed);
        return std::unique_ptr<MaterializedStorage>(result);
    }
    counters.storageCreated.fetch_add(1, std::memory_order_relaxed);
    auto storage = std::make_unique<MaterializedStorage>();
    storage->result = *materialized.skeleton;
    return storage;
}

void releaseMaterializedStorage(const MaterializedTemplate& materialized, std::unique_ptr<MaterializedStorage> result) {
    auto& counters = materializeValuesCounters();
    const auto bytes = materializedStorageBytes(result->result);
    result->retainedBytes = bytes;
    if (bytes > MaxMaterializedStorageBytes || materialized.idle.load(std::memory_order_relaxed) != nullptr) {
        counters.storageRefused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto retained = counters.storageRetainedBytes.load(std::memory_order_relaxed);
    while (retained <= MaterializedStorageBudget - bytes) {
        if (!counters.storageRetainedBytes.compare_exchange_weak(retained, retained + bytes, std::memory_order_relaxed)) continue;
        MaterializedStorage* expected = nullptr;
        if (materialized.idle.compare_exchange_strong(expected, result.get(), std::memory_order_release, std::memory_order_relaxed)) {
            static_cast<void>(result.release());
            return;
        }
        counters.storageRetainedBytes.fetch_sub(bytes, std::memory_order_relaxed);
        break;
    }
    counters.storageRefused.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const RecompileResult> materializeTemplateValues(const std::shared_ptr<const MaterializedTemplate>& materialized, const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot) {
    static const bool reuse = std::getenv("APS5_NO_MATERIALIZE_STORAGE_REUSE") == nullptr;
    if (!reuse) {
        auto result = std::make_shared<RecompileResult>(*materialized->skeleton);
        fillMaterializedValues(*result, variant, request, snapshot, materialized->parts);
        result->cacheHit = true;
        return result;
    }
    auto result = acquireMaterializedStorage(*materialized);
    fillMaterializedValues(result->result, variant, request, snapshot, materialized->parts);
    result->result.cacheHit = true;
    auto* owned = result.release();
    return std::shared_ptr<const RecompileResult>(&owned->result, [materialized, owned](const RecompileResult*) {
        releaseMaterializedStorage(*materialized, std::unique_ptr<MaterializedStorage>(owned));
    });
}

}

MaterializedTemplate::~MaterializedTemplate() {
    if (auto* result = idle.load(std::memory_order_relaxed)) {
        materializeValuesCounters().storageRetainedBytes.fetch_sub(result->retainedBytes, std::memory_order_relaxed);
        delete result;
    }
}

std::uint64_t SpecializationKey(const SourceHandle& handle, const RecompileRequest& request, const ResourceSnapshot& snapshot) {
    if (handle.artifact == nullptr) throw std::runtime_error("ShaderRecompiler: prepared artifact is missing");
    struct SpecializationKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, SpecializationKeyStorage>();
    specializationKeyWords(*handle.artifact, request, snapshot, key);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : key) {
        hash ^= word;
        hash *= 0x100000001b3ull;
    }
    return hash == 0 ? 1 : hash;
}

SnapshotWordCounts CountSpecializationWords(const ResourceSnapshot& snapshot) {
    SnapshotWordCounts counts;
    for (const auto& descriptor : snapshot.buffers) {
        if (descriptor.dwordCount == 0) continue;
        counts.keyWords += 2;
        counts.valueWords += 2;
    }
    for (const auto& descriptor : snapshot.images) {
        if (descriptor.dwordCount == 0) continue;
        counts.keyWords += 7;
        counts.valueWords += 1;
    }
    for (const auto& descriptor : snapshot.samplers) counts.keyWords += descriptor.dwordCount;
    counts.valueWords += snapshot.flattenedSrt.size() + snapshot.userData.size();
    return counts;
}

MaterializeValuesCounts GetMaterializeValuesCounts() {
    const auto& counters = materializeValuesCounters();
    return {counters.templateHits.load(std::memory_order_relaxed), counters.templateMisses.load(std::memory_order_relaxed), counters.storageCreated.load(std::memory_order_relaxed), counters.storageReused.load(std::memory_order_relaxed), counters.storageRetainedBytes.load(std::memory_order_relaxed), counters.storageRefused.load(std::memory_order_relaxed)};
}

std::shared_ptr<const RecompileResult> PreparedShaderInvocation::MaterializeValues(const ResourceCapture& capture, bool* templateHit) const {
    if (capture.source != handle->source || capture.plan != usePlan(*handle->source)) throw std::runtime_error("ShaderRecompiler: resource capture belongs to another prepared shader");
    if (templateHit != nullptr) *templateHit = false;
    static const bool templates = std::getenv("APS5_NO_MATERIALIZE_TEMPLATES") == nullptr;
    const auto& variant = *handle->artifact;
    auto& source = *handle->source;
    if (!templates) {
        auto result = std::make_shared<RecompileResult>(materializeResult(variant, request, capture.snapshot));
        result->cacheHit = true;
        return result;
    }
    struct TemplateKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, TemplateKeyStorage>();
    specializationKeyWords(variant, request, capture.snapshot, key);
    std::shared_ptr<const MaterializedTemplate> found;
    {
        std::lock_guard lock(source.mutex);
        for (std::size_t index = 0; index < source.templates.size(); ++index) {
            if (!std::ranges::equal(source.templates[index]->key, key)) continue;
            found = source.templates[index];
            if (index != 0) std::rotate(source.templates.begin(), source.templates.begin() + static_cast<std::ptrdiff_t>(index), source.templates.begin() + static_cast<std::ptrdiff_t>(index) + 1);
            break;
        }
    }
    if (found != nullptr) {
        auto result = materializeTemplateValues(found, variant, request, capture.snapshot);
        materializeValuesCounters().templateHits.fetch_add(1, std::memory_order_relaxed);
        if (templateHit != nullptr) *templateHit = true;
        return result;
    }
    auto stored = std::make_shared<MaterializedTemplate>();
    stored->key = key;
    auto skeleton = materializeSkeleton(variant, request, capture.snapshot, false, stored->parts);
    stored->skeleton = std::make_shared<const RecompileResult>(std::move(skeleton));
    auto result = materializeTemplateValues(stored, variant, request, capture.snapshot);
    {
        std::lock_guard lock(source.mutex);
        source.templates.insert(source.templates.begin(), std::move(stored));
        if (source.templates.size() > MaxMaterializedTemplates) source.templates.pop_back();
    }
    materializeValuesCounters().templateMisses.fetch_add(1, std::memory_order_relaxed);
    return result;
}

const CompiledShaderArtifact& GetPreparedArtifact(const SourceHandle& handle) {
    if (handle.artifact == nullptr) throw std::runtime_error("ShaderRecompiler: prepared artifact is missing");
    return handle.artifact->artifact;
}

std::shared_ptr<const RecompileResult> RegistrationResult(const RecompileRequest& request, const SourceHandle& handle) {
    if (handle.artifact == nullptr || !MatchesPreparedShader(request, handle)) throw std::runtime_error("ShaderRecompiler: prepared artifact does not match the static ABI");
    if (!request.target.registrationModules || !handle.artifact->artifact.registrationModule) return nullptr;
    const auto& info = handle.artifact->info.info;
    if (std::ranges::any_of(info.buffers, [](const BufferResource& buffer) { return buffer.descriptorFormatted; }) || std::ranges::any_of(info.samplers, [](const SamplerResource& sampler) { return FoldsTexelOffsets(sampler); })) return nullptr;
    ResourceSnapshot snapshot;
    snapshot.buffers.assign(info.buffers.size(), {{}, 4u});
    snapshot.images.assign(info.images.size(), {{}, 8u});
    snapshot.samplers.assign(info.samplers.size(), {{}, 4u});
    snapshot.userData.assign(RuntimeAbi::UserDataCapacity, 0u);
    snapshot.flattenedSrt.assign(1u, 0u);
    return std::make_shared<RecompileResult>(materializeResult(*handle.artifact, request, snapshot, true));
}

std::shared_ptr<const RecompileResult> MaterializeShader(const RecompileRequest& request, const ResourceCapture& capture, const SourceHandle& handle) {
    if (!MatchesPreparedShader(request, handle)) throw std::runtime_error("ShaderRecompiler: prepared artifact does not match the static ABI");
    if (capture.source != handle.source || capture.plan != usePlan(*handle.source)) throw std::runtime_error("ShaderRecompiler: resource capture belongs to another prepared shader");
    if (request.useCache && ResultMemo()) return materializePreparedMemoized(*handle.source, handle.artifact, request, capture.snapshot, true, nullptr);
    auto result = std::make_shared<RecompileResult>(materializeResult(*handle.artifact, request, capture.snapshot));
    result->cacheHit = true;
    return result;
}

std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime, const SourceHandle& handle) {
    if (handle.source == nullptr) throw std::runtime_error("ShaderRecompiler: source handle is missing");
    if (handle.artifact != nullptr && !MatchesPreparedShader(request, handle)) throw std::runtime_error("ShaderRecompiler: prepared artifact does not match the static ABI");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // The whole vertex family (Vertex, Local, TC, TE, Mesh) validates V# fields the memo key does not cover.
    if (request.shader.stage != ShaderStage::Compute && request.shader.stage != ShaderStage::Fragment) static_cast<void>(RequestInputInfo(request));
    auto capture = std::make_shared<ResourceCapture>();
    capture->source = handle.source;
    capture->plan = usePlan(*handle.source);
    if (profile) capture->sourceNanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    materializeCapture(*capture, runtime);
    return capture;
}

RecompileResult Recompile(const RecompileRequest& request) {
    return recompileReporting(request, [&] { return RecompileImpl(request); });
}

std::shared_ptr<const RecompileResult> Recompile(const RecompileRequest& request, const ResourceCapture& capture, bool* memoHit) {
    if (memoHit != nullptr) *memoHit = false;
    return recompileReporting(request, [&] { return RecompileImpl(request, capture, memoHit); });
}

}

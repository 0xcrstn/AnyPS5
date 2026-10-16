#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_SHADERTRACEOBSERVER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_SHADERTRACEOBSERVER_HPP

#include "prx/libSceAgcDriver/Execution/include/DrawTrace.hpp"
#include "Recompiler.hpp"
#include <algorithm>
#include <array>
#include <optional>

namespace AgcDriver::DriverDetail {

class ShaderTraceObserver {
public:
    ShaderTraceObserver() {
        if (DrawTrace::Enabled()) scope.emplace(ShaderRecompiler::CompilerObservationContext{&Observe, this, DrawTrace::Current().job});
    }
    ShaderTraceObserver(const ShaderTraceObserver&) = delete;
    ShaderTraceObserver& operator=(const ShaderTraceObserver&) = delete;

    static void LinkProducer(const ShaderRecompiler::CompiledShaderArtifact& artifact) noexcept {
        try {
            if (!DrawTrace::Enabled()) return;
            if (artifact.tracePreparationJob != 0) {
                DrawTrace::BoundaryReference(artifact.tracePreparationJob, "job", "prepared_before_window_or_uninstrumented");
                DrawTrace::Link(artifact.tracePreparationJob, DrawTrace::Current().operation, "prepares_operation");
            } else DrawTrace::Note("preparation_producer", "unknown");
        } catch (...) { DrawTrace::ReportObservationLoss(); }
    }

private:
    static void Observe(void* state, ShaderRecompiler::CompilerObservationKind kind, std::string_view key) noexcept {
        try { static_cast<ShaderTraceObserver*>(state)->observe(kind, key); }
        catch (...) { DrawTrace::ReportObservationLoss(); }
    }

    void observe(ShaderRecompiler::CompilerObservationKind kind, std::string_view key) {
        using Kind = ShaderRecompiler::CompilerObservationKind;
        if (kind == Kind::ObservationLost) { DrawTrace::ReportObservationLoss(); return; }
        const bool plan = kind == Kind::PlanBegin || kind == Kind::PlanComplete || kind == Kind::PlanFailed;
        const bool begin = kind == Kind::PlanBegin || kind == Kind::CompileBegin;
        const bool end = kind == Kind::PlanComplete || kind == Kind::CompileComplete || kind == Kind::PlanFailed || kind == Kind::CompileFailed;
        if (begin) {
            if (omittedDepth != 0 || depth == work.size()) {
                if (omittedDepth == 0) DrawTrace::ReportObservationLoss();
                ++omittedDepth;
                return;
            }
            auto& selected = work[depth++];
            selected.plan = plan;
            selected.span.emplace(plan ? "Shader.ResourcePlanBuild" : "Shader.Compile");
        } else if (omittedDepth != 0) {
            if (end) --omittedDepth;
            return;
        }
        if (!key.empty()) {
            std::array<char, 65> text{};
            std::copy_n(key.begin(), std::min<std::size_t>(key.size(), text.size() - 1), text.begin());
            DrawTrace::Note("compiler_key", text.data());
            DrawTrace::Note("compiler_key_domain", plan || kind == Kind::PlanMemoryHit || kind == Kind::PlanDiskHit || kind == Kind::PlanFailureMemoHit ? "resource_plan" : "compiled_variant");
        } else DrawTrace::Note("compiler_key_availability", "unknown");
        if (end) {
            auto* selected = depth != 0 && work[depth - 1].plan == plan ? &work[depth - 1] : nullptr;
            if (selected != nullptr && selected->span) selected->span->Outcome(kind == Kind::PlanFailed || kind == Kind::CompileFailed ? DrawTrace::Status::Failed : DrawTrace::Status::Complete);
            DrawTrace::Note("compile_outcome", kind == Kind::PlanFailed || kind == Kind::CompileFailed ? "failed" : "complete");
            DrawTrace::RecordCounter(plan ? "resource_plan_builds" : "shader_compilations", 1, "attempts");
            if (selected != nullptr) { selected->span.reset(); --depth; }
            return;
        }
        const char* outcome = nullptr;
        switch (kind) {
        case Kind::PlanMemoryHit: outcome = "plan_memory_hit"; break;
        case Kind::PlanDiskHit: outcome = "plan_disk_hit"; break;
        case Kind::VariantMemoryHit: outcome = "variant_memory_hit"; break;
        case Kind::VariantDiskHit: outcome = "variant_disk_hit"; break;
        case Kind::MaterializationMemoryHit: outcome = "materialization_memory_hit"; break;
        case Kind::PlanFailureMemoHit: outcome = "plan_failure_memo_hit"; break;
        case Kind::VariantFailureMemoHit: outcome = "variant_failure_memo_hit"; break;
        default: break;
        }
        if (outcome != nullptr) DrawTrace::Note("cache_outcome", outcome);
    }

    struct Work {
        std::optional<DrawTrace::Span> span;
        bool plan = false;
    };
    std::array<Work, 8> work;
    std::size_t depth = 0;
    std::size_t omittedDepth = 0;
    std::optional<ShaderRecompiler::CompilerObservationScope> scope;
};

}

#endif

#include "Recompiler.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace ShaderRecompiler;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Observations {
    std::array<CompilerObservationKind, 128> kinds{};
    std::array<std::array<char, 65>, 128> keys{};
    std::size_t count = 0;
    bool overflow = false;

    static void Observe(void* state, CompilerObservationKind kind, std::string_view key) noexcept {
        auto& observations = *static_cast<Observations*>(state);
        if (observations.count == observations.kinds.size()) { observations.overflow = true; return; }
        observations.kinds[observations.count] = kind;
        std::copy_n(key.begin(), std::min<std::size_t>(key.size(), 64), observations.keys[observations.count].begin());
        ++observations.count;
    }

    std::size_t Count(CompilerObservationKind kind) const {
        return static_cast<std::size_t>(std::count(kinds.begin(), kinds.begin() + count, kind));
    }
};

RecompileRequest Request(std::span<const std::uint32_t> code) {
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x12345600u, code, 0, {}};
    request.context.waveSize = 32;
    request.context.compute = ShaderComputeStageInfo{{1, 1, 1}, 0, {false, false, false}, false, 1, {}};
    request.target.vulkanVersion = (1u << 22u) | (1u << 12u);
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 32;
    request.target.maxWorkgroupSize = {1024, 1024, 64};
    request.target.maxWorkgroupInvocations = 1024;
    request.target.maxWorkgroupSharedMemoryBytes = 32768;
    request.layout = {0, 0, 0, 128};
    return request;
}

void NestedObserverRestoresAfterException() {
    Observations outer, inner;
    {
        CompilerObservationScope scope({&Observations::Observe, &outer, 17});
        try {
            CompilerObservationScope nested({&Observations::Observe, &inner, 29});
            throw std::runtime_error("synthetic unwind");
        } catch (const std::runtime_error&) {}
        const auto restored = ExchangeCompilerObserver({});
        Check(restored.observer == &Observations::Observe && restored.state == &outer && restored.producerJob == 17, "nested observer did not restore its parent after unwind");
        ExchangeCompilerObserver(restored);
        bool isolated = false;
        std::thread thread([&] { const auto inherited = ExchangeCompilerObserver({}); isolated = inherited.observer == nullptr && inherited.producerJob == 0; });
        thread.join();
        Check(isolated, "compiler observation leaked across worker threads");
    }
    Check(ExchangeCompilerObserver({}).observer == nullptr, "compiler observation outlived its scope");
}

void CompilationAndCachedConsumptionAreDistinct() {
    const std::array<std::uint32_t, 1> code{0xbf810000u};
    auto request = Request(code);
    Observations initial;
    std::shared_ptr<const SourceHandle> first;
    {
        CompilerObservationScope scope({&Observations::Observe, &initial, 77});
        first = PrepareShader(request);
    }
    Check(!initial.overflow && initial.Count(CompilerObservationKind::CompileBegin) == 1 && initial.Count(CompilerObservationKind::CompileComplete) == 1, "actual compile did not have one balanced work interval");
    Check(initial.Count(CompilerObservationKind::PlanBegin) == 1 && initial.Count(CompilerObservationKind::PlanComplete) == 1, "actual resource plan did not have one balanced work interval");
    Check(GetPreparedArtifact(*first).tracePreparationJob == 77, "compiled artifact lost its producing preparation job");
    const auto& identity = GetPreparedArtifact(*first).traceSourceIdentity;
    Check(identity != nullptr, "detailed compiler key missing");
    const auto compile = std::find(initial.kinds.begin(), initial.kinds.begin() + initial.count, CompilerObservationKind::CompileBegin);
    Check(std::string_view(initial.keys[static_cast<std::size_t>(compile - initial.kinds.begin())].data()) == identity->recompileKeyIdentity, "compile scope key differs from the resulting artifact key");
    Observations repeated;
    std::shared_ptr<const SourceHandle> second;
    {
        CompilerObservationScope scope({&Observations::Observe, &repeated, 88});
        second = PrepareShader(request);
    }
    Check(repeated.Count(CompilerObservationKind::CompileBegin) == 0 && repeated.Count(CompilerObservationKind::VariantMemoryHit) == 1 && repeated.Count(CompilerObservationKind::PlanMemoryHit) == 1, "cached consumer was incorrectly charged another compilation");
    Check(GetPreparedArtifact(*second).tracePreparationJob == 77 && GetPreparedArtifact(*second).spirv.data() == GetPreparedArtifact(*first).spirv.data(), "cached consumption changed the original artifact or producer");
    const auto count = repeated.count;
    static_cast<void>(PrepareShader(request));
    Check(repeated.count == count, "unbound compilation invoked a stale observer");
}

void FailedPlanAndFailureMemoAreDistinct() {
    const std::array<std::uint32_t, 1> code{0xffffffffu};
    auto request = Request(code);
    const auto fail = [&] {
        try { static_cast<void>(PrepareShader(request)); }
        catch (const std::exception& error) { return std::string(error.what()); }
        throw std::runtime_error("invalid shader unexpectedly prepared");
    };
    Observations first, second;
    std::string firstFailure, secondFailure;
    {
        CompilerObservationScope scope({&Observations::Observe, &first, 91});
        firstFailure = fail();
    }
    {
        CompilerObservationScope scope({&Observations::Observe, &second, 92});
        secondFailure = fail();
    }
    Check(first.Count(CompilerObservationKind::PlanBegin) == 1 && first.Count(CompilerObservationKind::PlanFailed) == 1 && first.Count(CompilerObservationKind::CompileComplete) == 0, "failed preparation reported a successful compile");
    Check(second.Count(CompilerObservationKind::PlanFailureMemoHit) == 1 && second.Count(CompilerObservationKind::PlanBegin) == 0 && firstFailure == secondFailure, "memoized failure changed shader behavior or invented repeated work");
}

}

int main() {
    try {
#ifdef _WIN32
        Check(_putenv_s("ANYPS5_NO_SHADER_CACHE", "1") == 0 && _putenv_s("APS5_TRACE_DETAIL", "draw") == 0 && _putenv_s("APS5_NO_FAILURE_MEMO", "") == 0, "cannot configure synthetic compiler test");
#else
        Check(setenv("ANYPS5_NO_SHADER_CACHE", "1", 1) == 0 && setenv("APS5_TRACE_DETAIL", "draw", 1) == 0 && unsetenv("APS5_NO_FAILURE_MEMO") == 0, "cannot configure synthetic compiler test");
#endif
        NestedObserverRestoresAfterException();
        CompilationAndCachedConsumptionAreDistinct();
        FailedPlanAndFailureMemoAreDistinct();
        std::cout << "Compiler observation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

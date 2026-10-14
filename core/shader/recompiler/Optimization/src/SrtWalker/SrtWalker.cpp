#include "Optimization/SrtWalker.hpp"
#include "Optimization/SrtWalker/SrtDescriptorEvaluation.hpp"
#include "Optimization/SrtWalker/SrtEvaluator.hpp"
#include "Optimization/SrtWalker/SrtPlanBuilder.hpp"
#include "Optimization/SrtWalker/SrtRuntimeValidator.hpp"
#include "Optimization/SrtWalker/SrtTape.hpp"

#include <algorithm>
#include <array>
#include <atomic>

#include <stdexcept>

namespace ShaderRecompiler {

namespace {
std::atomic<std::uint64_t> compiledWalkRuns{0};
std::atomic<std::uint64_t> compiledWalkFallbacks{0};
std::atomic<std::uint64_t> compiledWalkFallbacksInaccessible{0};
std::atomic<std::uint64_t> compiledWalkUncompiled{0};
std::atomic<std::uint64_t> compiledWalkIndirectImages{0};
std::array<std::atomic<std::uint64_t>, SrtCompiledWalkRefusals> compiledWalkRefusals{};
}

SrtCompiledWalkCounts CompiledWalkCounts() {
    SrtCompiledWalkCounts counts{compiledWalkRuns.load(std::memory_order_relaxed), compiledWalkFallbacks.load(std::memory_order_relaxed), compiledWalkFallbacksInaccessible.load(std::memory_order_relaxed), compiledWalkUncompiled.load(std::memory_order_relaxed), compiledWalkIndirectImages.load(std::memory_order_relaxed), {}};
    for (std::size_t reason = 0; reason < SrtCompiledWalkRefusals; ++reason) counts.refusals[reason] = compiledWalkRefusals[reason].load(std::memory_order_relaxed);
    return counts;
}

void SrtWalker::BuildPlan(IrProgram& program) const {
    if (program.Resources().resourceTrackingComplete) {
        throw std::runtime_error("shader SRT planning failed: cannot rebuild SRT after resource tracking");
    }
    program.Resources().srtPlanComplete = false;
    Detail::PlanBuilder(program).Run();
    program.Resources().srtPlanComplete = true;
}

bool SrtWalker::ValidateRuntimeValue(const IrResourcePlan& program, const IrValue* value, RuntimeValueType type) const {
    return Detail::RuntimeValidator(program, type).Run(const_cast<IrValue*>(value));
}

void SrtWalker::EvaluateUniformValues(const IrResourcePlan& program, std::span<IrValue* const> values, const SrtRuntime& runtime, std::span<std::uint32_t> results) const {
    if (values.size() != results.size()) {
        throw std::runtime_error("SrtWalker::EvaluateUniformValues value and result counts differ");
    }
    auto clean = runtime;
    clean.readMemory = runtime.readSpecializationMemory != nullptr ? runtime.readSpecializationMemory : +[](void*, std::uint64_t, std::uint32_t*) { return false; };
    Detail::Evaluator evaluator(program, clean);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!evaluator.Evaluate(values[i], results[i])) {
            throw std::runtime_error("SrtWalker::EvaluateUniformValues failed to evaluate a uniform value");
        }
    }
}

void SrtWalker::EvaluateDescriptorSource(const IrResourcePlan& program, std::uint32_t source, const SrtRuntime& runtime, DescriptorValue& result) const {
    std::vector<DescriptorValue> results;
    EvaluateDescriptorSources(program, std::span {&source, 1}, runtime, results);
    result = results.front();
}

void SrtWalker::EvaluateDescriptorSources(const IrResourcePlan& program, std::span<const std::uint32_t> sources, const SrtRuntime& runtime, std::vector<DescriptorValue>& results) const {
    std::vector<std::uint32_t> ignored;
    std::vector<std::uint8_t> active;
    if (!Detail::EvaluateRuntimeSourcesImpl(program, sources, runtime, results, ignored, false, {}, active)) {
        throw std::runtime_error("SrtWalker::EvaluateDescriptorSources failed to evaluate descriptor sources: " + Detail::RuntimeSourceFailureReason());
    }
}

void SrtWalker::EvaluateRuntimeSources(const IrResourcePlan& program, std::span<const std::uint32_t> sources, const SrtRuntime& runtime, std::vector<DescriptorValue>& results, std::vector<std::uint32_t>& flat, std::span<const std::uint8_t> cleanFlatSlots, std::vector<std::uint8_t>& activeSources, std::vector<SrtReadPoison>* poison, std::uint32_t* nullRootReads) const {
    if (runtime.compiledWalk && Detail::CompiledWalkEnabled() && poison != nullptr && nullRootReads != nullptr && runtime.userDataUse == nullptr && runtime.readTrace == nullptr && sources.data() == program.materializationSources.data() && sources.size() == program.materializationSources.size() && cleanFlatSlots.data() == program.cleanFlatSlots.data() && cleanFlatSlots.size() == program.cleanFlatSlots.size()) {
        const auto tape = Detail::SrtTape::For(program);
        if (tape != nullptr && tape->Refusal() == SrtCompiledWalkRefusal::IndirectImage) compiledWalkIndirectImages.fetch_add(1, std::memory_order_relaxed);
        if (tape != nullptr && tape->Compiled()) {
            const auto outcome = tape->Run(runtime, results, flat, activeSources, *nullRootReads);
            if (outcome == Detail::SrtTape::Outcome::Served) {
                poison->clear();
                compiledWalkRuns.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            compiledWalkFallbacks.fetch_add(1, std::memory_order_relaxed);
            if (outcome == Detail::SrtTape::Outcome::Inaccessible) compiledWalkFallbacksInaccessible.fetch_add(1, std::memory_order_relaxed);
        } else {
            compiledWalkUncompiled.fetch_add(1, std::memory_order_relaxed);
            const auto reason = tape != nullptr ? static_cast<std::size_t>(tape->Refusal()) : static_cast<std::size_t>(SrtCompiledWalkRefusal::IncompletePlan);
            compiledWalkRefusals[reason < SrtCompiledWalkRefusals ? reason : 0].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!Detail::EvaluateRuntimeSourcesImpl(program, sources, runtime, results, flat, true, cleanFlatSlots, activeSources, poison, nullRootReads)) {
        throw std::runtime_error("SrtWalker::EvaluateRuntimeSources failed to evaluate runtime sources: " + Detail::RuntimeSourceFailureReason());
    }
}

void SrtWalker::Walk(const IrResourcePlan& program, const SrtRuntime& runtime, std::vector<std::uint32_t>& flat) const {
    std::vector<DescriptorValue> ignored;
    std::vector<std::uint8_t> active;
    EvaluateRuntimeSources(program, {}, runtime, ignored, flat, {}, active);
}

}

#include "ShaderDiskCache.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

namespace Cache = ShaderRecompiler::ShaderDiskCache;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Versions() {
    Require(!Cache::LayeredCache(), "the layered cache is on without APS5_SHADER_CACHE_V2");
    Require(Cache::PlanSourceVersion() == Cache::SourceVersion(), "plans use a separate source version without APS5_SHADER_CACHE_V2");
}

void PlanComparison() {
    ShaderRecompiler::IrResourcePlan reference, same, different;
    reference.userDataCount = 48;
    same.userDataCount = 48;
    different.userDataCount = 47;
    Require(Cache::SamePlan(reference, same), "identical plans compare different");
    Require(!Cache::SamePlan(reference, different), "an injected plan mismatch compares equal");
}

void PlanEntries() {
    ShaderRecompiler::IrResourcePlan plan;
    plan.userDataCount = 24;
    const std::vector<std::byte> key{std::byte{1}, std::byte{2}, std::byte{3}};
    const auto file = Cache::EncodePlanEntry(key, plan);
    Require(!file.empty(), "a plan entry was not encoded");
    ShaderRecompiler::IrResourcePlan decoded;
    Require(Cache::DecodePlanEntry(file, key, decoded) == Cache::LoadStatus::Loaded && Cache::SamePlan(plan, decoded), "a plan entry did not round-trip");
    const std::vector<std::byte> otherKey{std::byte{1}, std::byte{2}, std::byte{4}};
    ShaderRecompiler::IrResourcePlan ignored;
    Require(Cache::DecodePlanEntry(file, otherKey, ignored) != Cache::LoadStatus::Loaded, "a plan entry loaded under another key");
    auto corrupt = file;
    corrupt.back() ^= std::byte{0x5a};
    Require(Cache::DecodePlanEntry(corrupt, key, ignored) == Cache::LoadStatus::Rejected, "a corrupt plan entry was not rejected");
    auto truncated = file;
    truncated.resize(truncated.size() / 2u);
    Require(Cache::DecodePlanEntry(truncated, key, ignored) == Cache::LoadStatus::Rejected, "a truncated plan entry was not rejected");
}

}

int main() {
    try {
        Versions();
        PlanComparison();
        PlanEntries();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "shader cache layer test failed: %s\n", error.what());
        return 1;
    }
    std::puts("shader cache layer tests passed");
    return 0;
}

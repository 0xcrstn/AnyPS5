#include "ShaderDiskCache.hpp"
#include "SpirvIntern.hpp"
#include <algorithm>
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

void Interning() {
    const std::vector<std::uint32_t> words{0x07230203u, 0x00010300u, 0u, 9u, 0u, 7u, 8u};
    const auto before = ShaderRecompiler::SpirvInternTotals();
    const auto first = ShaderRecompiler::InternSpirv(words, true);
    auto second = ShaderRecompiler::InternSpirv(words, true);
    Require(first.Words().data() == second.Words().data(), "identical SPIR-V was not shared");
    auto changed = words;
    changed.back() = 9u;
    const auto other = ShaderRecompiler::InternSpirv(changed, true);
    Require(other.Words().data() != first.Words().data() && std::ranges::equal(other.Words(), changed), "different SPIR-V was shared");
    const auto plain = ShaderRecompiler::InternSpirv(words, false);
    Require(plain.Words().data() != first.Words().data() && std::ranges::equal(plain.Words(), words), "interning ran while disabled");
    second[6] = 1u;
    Require(first[6] == 8u && second[6] == 1u, "writing one interned module changed another");
    const auto after = ShaderRecompiler::SpirvInternTotals();
    Require(after.lookups - before.lookups == 3u && after.shared - before.shared == 1u && after.sharedBytes - before.sharedBytes == words.size() * sizeof(std::uint32_t), "intern counters are wrong");
}

}

int main() {
    try {
        Versions();
        PlanComparison();
        PlanEntries();
        Interning();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "shader cache layer test failed: %s\n", error.what());
        return 1;
    }
    std::puts("shader cache layer tests passed");
    return 0;
}

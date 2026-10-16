#include "Recompiler.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    const std::vector<std::uint32_t> words{0x07230203u, 0x00010000u, 0u, 2u, 0u, 0x00020011u, 1u};
    ShaderRecompiler::SharedSpirv first(words);
    ShaderRecompiler::SharedSpirv same(words);
    if (first.ContentHash() != same.ContentHash()) return 1;
    const auto original = first.ContentHash();
    auto copied = first;
    copied[2] = 17u;
    if (copied.ContentHash() == original || first.ContentHash() != original) return 2;
    const auto changed = copied.ContentHash();
    copied[2] = 23u;
    if (copied.ContentHash() == changed) return 3;
    const auto owner = std::make_shared<const std::vector<std::uint32_t>>(words);
    ShaderRecompiler::SharedSpirv mapped(owner, *owner);
    if (mapped.ContentHash() != original) return 4;
    ShaderRecompiler::SharedSpirv mappedAlias(owner, *owner);
    if (!(mappedAlias == mapped)) return 8;
    mapped[2] = 19u;
    if (mapped.ContentHash() == original || (*owner)[2] != 0u) return 5;
    ShaderRecompiler::SharedSpirv concurrent(words);
    std::atomic<bool> agreed{true};
    std::array<std::thread, 4> readers;
    for (auto& reader : readers) reader = std::thread([&] {
        for (std::size_t index = 0; index < 1000; ++index) {
            if (concurrent.ContentHash() != original) agreed.store(false);
        }
    });
    for (auto& reader : readers) reader.join();
    if (!agreed.load()) return 6;
    ShaderRecompiler::SharedSpirv empty;
    if (empty.ContentHash() != ShaderRecompiler::SpirvContentHash{}) return 7;
    ShaderRecompiler::RecompileResult result;
    auto invocationCopy = result;
    const auto identity = std::make_shared<const std::string>("input-identity");
    const auto otherIdentity = std::make_shared<const std::string>("other-input-identity");
    if (!result.inputProvenance->Assign(identity) || invocationCopy.inputProvenance->Load() != identity) return 9;
    if (invocationCopy.inputProvenance->Assign(otherIdentity) || result.inputProvenance->Load() != identity) return 10;
    if (result.inputProvenance->Clear(otherIdentity) || invocationCopy.inputProvenance->Load() != identity) return 11;
    if (!invocationCopy.inputProvenance->Clear(identity) || result.inputProvenance->Load() != nullptr) return 12;
    std::puts("pipeline content identity passed");
}

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTEPIPELINERECIPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTEPIPELINERECIPE_HPP

#include "Recompiler.hpp"
#include "RuntimeAbi.hpp"
#include "CompileTrace.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver {

struct ComputePipelineRecipe {
    std::uint64_t programAddress = 0;
    bool pushes = false;
    std::shared_ptr<ShaderRecompiler::RecompileResult> shader;
};

class ComputeRecipeWriter {
public:
    template<typename TValue>
    void Value(TValue value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(TValue));
    }

    std::vector<std::byte> out;
};

class ComputeRecipeReader {
public:
    explicit ComputeRecipeReader(std::span<const std::byte> bytes) : bytes(bytes) {}

    template<typename TValue>
    bool Value(TValue& value) {
        if (bytes.size() - cursor < sizeof(TValue)) return false;
        std::memcpy(&value, bytes.data() + cursor, sizeof(TValue));
        cursor += sizeof(TValue);
        return true;
    }

    std::size_t Remaining() const {
        return bytes.size() - cursor;
    }

private:
    std::span<const std::byte> bytes;
    std::size_t cursor = 0;
};

inline constexpr std::uint32_t ComputeRecipeMagic = 0x57525041u;
inline constexpr std::uint32_t ComputeRecipeVersion = 1u;

inline std::vector<std::byte> EncodeComputeRecipe(const ShaderRecompiler::RecompileResult& shader, bool pushes, std::uint64_t programAddress) {
    ComputeRecipeWriter writer;
    writer.Value(ComputeRecipeMagic);
    writer.Value(ComputeRecipeVersion);
    writer.Value(programAddress);
    writer.Value(static_cast<std::uint32_t>(pushes ? 1u : 0u));
    writer.Value(shader.hostSubgroupSize);
    writer.Value(shader.workgroupMemoryDwords);
    writer.Value(shader.runtimeAbiVersion);
    writer.Value(shader.bdaAbiVersion);
    writer.Value(static_cast<std::uint32_t>(shader.specialization.size()));
    for (const auto& constant : shader.specialization) {
        writer.Value(constant.id);
        writer.Value(constant.value);
    }
    writer.Value(static_cast<std::uint32_t>(shader.bindings.size()));
    for (const auto& binding : shader.bindings) {
        writer.Value(static_cast<std::uint32_t>(binding.kind));
        writer.Value(static_cast<std::uint32_t>(binding.role));
        writer.Value(binding.descriptorSet);
        writer.Value(binding.binding);
        writer.Value(binding.count);
    }
    const auto words = shader.spirv.Words();
    writer.Value(static_cast<std::uint64_t>(words.size()));
    for (const auto word : words) writer.Value(word);
    writer.Value(ShaderCompileTrace::HashBytes(writer.out));
    return std::move(writer.out);
}

inline std::optional<ComputePipelineRecipe> DecodeComputeRecipe(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(std::uint64_t)) return std::nullopt;
    const auto body = bytes.first(bytes.size() - sizeof(std::uint64_t));
    std::uint64_t stored = 0;
    std::memcpy(&stored, bytes.data() + body.size(), sizeof(stored));
    if (ShaderCompileTrace::HashBytes(body) != stored) return std::nullopt;
    ComputeRecipeReader reader(body);
    std::uint32_t magic = 0, version = 0, pushes = 0, specializations = 0, bindings = 0;
    ComputePipelineRecipe recipe;
    recipe.shader = std::make_shared<ShaderRecompiler::RecompileResult>();
    auto& shader = *recipe.shader;
    if (!reader.Value(magic) || magic != ComputeRecipeMagic || !reader.Value(version) || version != ComputeRecipeVersion) return std::nullopt;
    if (!reader.Value(recipe.programAddress) || !reader.Value(pushes) || pushes > 1u) return std::nullopt;
    recipe.pushes = pushes != 0u;
    if (!reader.Value(shader.hostSubgroupSize) || !reader.Value(shader.workgroupMemoryDwords) || !reader.Value(shader.runtimeAbiVersion) || !reader.Value(shader.bdaAbiVersion)) return std::nullopt;
    if (shader.runtimeAbiVersion != ShaderRecompiler::RuntimeAbi::Version) return std::nullopt;
    if (!reader.Value(specializations) || specializations > reader.Remaining() / 8u) return std::nullopt;
    shader.specialization.resize(specializations);
    for (auto& constant : shader.specialization) {
        if (!reader.Value(constant.id) || !reader.Value(constant.value)) return std::nullopt;
    }
    if (!reader.Value(bindings) || bindings > reader.Remaining() / 20u) return std::nullopt;
    shader.bindings.resize(bindings);
    for (auto& binding : shader.bindings) {
        std::uint32_t kind = 0, role = 0;
        if (!reader.Value(kind) || !reader.Value(role) || !reader.Value(binding.descriptorSet) || !reader.Value(binding.binding) || !reader.Value(binding.count)) return std::nullopt;
        binding.kind = static_cast<ShaderRecompiler::DescriptorKind>(kind);
        binding.role = static_cast<ShaderRecompiler::DescriptorRole>(role);
    }
    std::uint64_t count = 0;
    if (!reader.Value(count) || count < 5u || count != reader.Remaining() / sizeof(std::uint32_t) || reader.Remaining() % sizeof(std::uint32_t) != 0u) return std::nullopt;
    std::vector<std::uint32_t> words(static_cast<std::size_t>(count));
    for (auto& word : words) reader.Value(word);
    if (words.front() != 0x07230203u) return std::nullopt;
    shader.spirv = ShaderRecompiler::SharedSpirv(std::move(words));
    if (!recipe.pushes) shader.pushConstants.clear();
    else shader.pushConstants.assign(1u, std::byte{0});
    return recipe;
}

}

#endif

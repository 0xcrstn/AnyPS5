#include "TraceIdentity.hpp"
#include "Recompiler.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ShaderRecompiler {

void TraceIdentityBuilder::Bytes(std::span<const std::byte> bytes) {
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() / 8u - length) throw std::length_error("trace identity input exceeds SHA256 length limit");
    length += bytes.size();
    for (const auto value : bytes) {
        block[used++] = std::to_integer<std::uint8_t>(value);
        if (used == block.size()) { transform(); used = 0; }
    }
}

void TraceIdentityBuilder::Integer(std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::byte>(value >> (index * 8u));
    Bytes(bytes);
}

void TraceIdentityBuilder::Text(std::string_view value) {
    Integer(value.size());
    Bytes(std::as_bytes(std::span(value.data(), value.size())));
}

void TraceIdentityBuilder::Words(std::span<const std::uint32_t> words) {
    Integer(words.size());
    for (const auto word : words) {
        std::array<std::byte, 4> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::byte>(word >> (index * 8u));
        Bytes(bytes);
    }
}

void TraceIdentityBuilder::transform() {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16u; ++index) {
        for (std::size_t byte = 0; byte < 4u; ++byte) words[index] = (words[index] << 8u) | block[index * 4u + byte];
    }
    for (std::size_t index = 16u; index < words.size(); ++index) {
        const auto first = words[index - 15u], second = words[index - 2u];
        words[index] = words[index - 16u] + (std::rotr(first, 7) ^ std::rotr(first, 18) ^ (first >> 3u)) + words[index - 7u] + (std::rotr(second, 17) ^ std::rotr(second, 19) ^ (second >> 10u));
    }
    auto working = state;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto first = working[0], fifth = working[4];
        const auto left = working[7] + (std::rotr(fifth, 6) ^ std::rotr(fifth, 11) ^ std::rotr(fifth, 25)) + ((fifth & working[5]) ^ (~fifth & working[6])) + constants[index] + words[index];
        const auto right = (std::rotr(first, 2) ^ std::rotr(first, 13) ^ std::rotr(first, 22)) + ((first & working[1]) ^ (first & working[2]) ^ (working[1] & working[2]));
        for (std::size_t step = 7u; step > 0; --step) working[step] = working[step - 1u];
        working[4] += left;
        working[0] = left + right;
    }
    for (std::size_t index = 0; index < state.size(); ++index) state[index] += working[index];
}

std::string TraceIdentityBuilder::Finish() const {
    auto result = *this;
    const auto bits = length * 8u;
    result.block[result.used++] = 0x80u;
    if (result.used > 56u) {
        std::fill(result.block.begin() + result.used, result.block.end(), 0u);
        result.transform();
        result.used = 0;
    }
    std::fill(result.block.begin() + result.used, result.block.begin() + 56u, 0u);
    for (std::size_t index = 0; index < 8u; ++index) result.block[63u - index] = static_cast<std::uint8_t>(bits >> (index * 8u));
    result.transform();
    constexpr char digits[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(64u);
    for (const auto word : result.state) {
        for (int shift = 28; shift >= 0; shift -= 4) digest.push_back(digits[(word >> shift) & 15u]);
    }
    return digest;
}

bool ShaderTraceIdentitiesEnabled() {
    static const bool enabled = [] { const auto* value = std::getenv("APS5_TRACE_DETAIL"); return value != nullptr && std::string_view(value) == "draw"; }();
    return enabled;
}

std::shared_ptr<TraceDigestCache> MakeTraceDigestCache() {
    return ShaderTraceIdentitiesEnabled() ? std::make_shared<TraceDigestCache>() : nullptr;
}

std::shared_ptr<ShaderTraceMetadataCache> MakeShaderTraceMetadataCache() {
    return ShaderTraceIdentitiesEnabled() ? std::make_shared<ShaderTraceMetadataCache>() : nullptr;
}

std::shared_ptr<const ShaderTraceSourceIdentity> MakeShaderTraceSourceIdentity(const RecompileRequest& request, std::uint32_t hostSubgroupSize) {
    if (!ShaderTraceIdentitiesEnabled()) return {};
    auto result = std::make_shared<ShaderTraceSourceIdentity>();
    TraceIdentityBuilder guest;
    guest.Text("anyps5.guest-shader.v1");
    guest.Integer(static_cast<std::uint32_t>(request.shader.stage));
    guest.Words(request.shader.code);
    result->guestIdentity = guest.Finish();
    result->stage = static_cast<std::uint32_t>(request.shader.stage);
    result->codeWords = request.shader.code.size();
    result->compilerSourceVersion = ShaderDiskCache::SourceVersion();
    std::vector<std::byte> key;
    ShaderDiskCache::BuildKey(request, hostSubgroupSize, key);
    TraceIdentityBuilder compiler;
    compiler.Text("anyps5.recompile-key.v1");
    compiler.Bytes(key);
    result->recompileKeyIdentity = compiler.Finish();
    return result;
}

std::vector<std::uint64_t> ShaderTraceVariantFields(const RecompileResult& shader) {
    std::vector<std::uint64_t> fields{shader.runtimeAbiVersion, shader.bdaAbiVersion, shader.hostSubgroupSize,
                                    shader.workgroupMemoryDwords, shader.pushConstants.size(), shader.registrationModule};
    auto constants = shader.specialization;
    std::sort(constants.begin(), constants.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    fields.push_back(constants.size());
    for (std::size_t index = 0; index < constants.size(); ++index) {
        if (index != 0 && constants[index - 1u].id == constants[index].id) throw std::invalid_argument("duplicate draw trace specialization constant");
        fields.push_back(constants[index].id);
        fields.push_back(constants[index].value);
    }
    fields.push_back(shader.bindings.size());
    for (const auto& binding : shader.bindings) {
        fields.push_back(static_cast<std::uint32_t>(binding.kind));
        fields.push_back(static_cast<std::uint32_t>(binding.role));
        fields.push_back(binding.descriptorSet); fields.push_back(binding.binding); fields.push_back(binding.count);
    }
    return fields;
}

void FinalizeShaderTraceIdentity(RecompileResult& shader) {
    if (!ShaderTraceIdentitiesEnabled()) return;
    auto cache = shader.traceMetadata != nullptr && shader.traceMetadata.use_count() == 1 ? shader.traceMetadata : std::make_shared<ShaderTraceMetadataCache>();
    cache->module = shader.spirv.TraceCache();
    cache->source = shader.traceSourceIdentity;
    cache->fields = ShaderTraceVariantFields(shader);
    cache->stages.fill(0);
    cache->epoch = 0;
    cache->finalized = true;
    shader.traceMetadata = std::move(cache);
}

}

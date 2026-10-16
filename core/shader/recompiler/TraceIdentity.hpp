#ifndef CORE_SHADER_RECOMPILER_TRACEIDENTITY_HPP
#define CORE_SHADER_RECOMPILER_TRACEIDENTITY_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ShaderRecompiler {

class TraceIdentityBuilder {
public:
    void Bytes(std::span<const std::byte> bytes);
    void Integer(std::uint64_t value);
    void Text(std::string_view value);
    void Words(std::span<const std::uint32_t> words);
    [[nodiscard]] std::string Finish() const;

private:
    void transform();
    std::array<std::uint32_t, 8> state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> block{};
    std::size_t used = 0;
    std::uint64_t length = 0;
};

struct TraceDigestCache {
    std::once_flag ready;
    std::string digest;
    std::atomic<bool> hashed{false};
};

struct ShaderTraceSourceIdentity {
    std::string guestIdentity;
    std::string recompileKeyIdentity;
    std::uint32_t stage = 0;
    std::uint64_t codeWords = 0;
    std::uint64_t compilerSourceVersion = 0;
};

struct ShaderTraceMetadataCache {
    std::mutex mutex;
    std::shared_ptr<TraceDigestCache> module;
    std::shared_ptr<const ShaderTraceSourceIdentity> source;
    std::vector<std::uint64_t> fields;
    std::array<std::uint64_t, 8> stages{};
    std::uint64_t epoch = 0;
    bool finalized = false;
};

struct RecompileRequest;
struct RecompileResult;
[[nodiscard]] bool ShaderTraceIdentitiesEnabled();
[[nodiscard]] std::shared_ptr<TraceDigestCache> MakeTraceDigestCache();
[[nodiscard]] std::shared_ptr<ShaderTraceMetadataCache> MakeShaderTraceMetadataCache();
[[nodiscard]] std::shared_ptr<const ShaderTraceSourceIdentity> MakeShaderTraceSourceIdentity(const RecompileRequest& request, std::uint32_t hostSubgroupSize);
[[nodiscard]] std::vector<std::uint64_t> ShaderTraceVariantFields(const RecompileResult& shader);
void FinalizeShaderTraceIdentity(RecompileResult& shader);

}

#endif

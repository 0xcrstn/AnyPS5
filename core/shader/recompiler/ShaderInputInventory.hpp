#ifndef CORE_SHADER_RECOMPILER_SHADERINPUTINVENTORY_HPP
#define CORE_SHADER_RECOMPILER_SHADERINPUTINVENTORY_HPP

#include "ControlFlow/RequestSerializer.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ShaderRecompiler {

struct ShaderInputInventoryOptions {
    std::uint64_t maxEntryBytes = 16u * 1024u * 1024u;
    std::uint64_t maxEntries = 16384u;
    std::uint64_t maxTotalBytes = 512u * 1024u * 1024u;
};

enum class ShaderInputCaptureStatus {
    Stored,
    Queued,
    Duplicate,
    Disabled,
    TooLarge,
    Full,
    Invalid,
    IoError
};

struct ShaderInputRecord {
    std::string identity;
    std::filesystem::path path;
    std::uint64_t bytes = 0;
};

class ShaderInputInventory {
public:
    explicit ShaderInputInventory(std::filesystem::path directory, ShaderInputInventoryOptions options = {});
    ~ShaderInputInventory();
    ShaderInputInventory(const ShaderInputInventory&) = delete;
    ShaderInputInventory& operator=(const ShaderInputInventory&) = delete;
    [[nodiscard]] std::vector<ShaderInputRecord> List() const;
    [[nodiscard]] DeserializedRequest Load(const std::filesystem::path& path) const;
    [[nodiscard]] ShaderInputCaptureStatus Store(const RecompileRequest& request);
    [[nodiscard]] ShaderInputCaptureStatus StoreBinary(std::string_view bytes);
    [[nodiscard]] static std::filesystem::path DefaultDirectory();
    [[nodiscard]] static std::string Identity(std::string_view bytes);
private:
    struct State;
    std::unique_ptr<State> state;
};

struct ShaderInputCaptureCounters {
    std::uint64_t queued = 0;
    std::uint64_t stored = 0;
    std::uint64_t duplicate = 0;
    std::uint64_t tooLarge = 0;
    std::uint64_t full = 0;
    std::uint64_t invalid = 0;
    std::uint64_t ioErrors = 0;
    std::uint64_t pendingBytes = 0;
    std::uint64_t pendingEntries = 0;
    std::uint64_t cooldown = 0;
};

[[nodiscard]] bool ShouldCaptureShaderInput(const std::shared_ptr<const RecompileResult>& result) noexcept;
[[nodiscard]] ShaderInputCaptureStatus CaptureShaderInput(const RecompileRequest& request, const std::shared_ptr<const RecompileResult>& result, bool completeSnapshot = true) noexcept;
[[nodiscard]] std::string FindShaderInputIdentity(const std::shared_ptr<const RecompileResult>& result);
[[nodiscard]] std::string FindShaderInputIdentity(const RecompileResult& result);
[[nodiscard]] ShaderInputCaptureCounters ShaderInputCaptureTotals();
void FlushShaderInputs();

}

#endif

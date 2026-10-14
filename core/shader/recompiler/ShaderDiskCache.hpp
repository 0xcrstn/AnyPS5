#ifndef CORE_SHADER_RECOMPILER_SHADERDISKCACHE_HPP
#define CORE_SHADER_RECOMPILER_SHADERDISKCACHE_HPP

#include "CompiledVariant.hpp"
#include "IntermediateRepresentation/IrMetadata/ResourcePlan.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ShaderRecompiler::ShaderDiskCache {

inline constexpr std::uint32_t FormatVersion = 32;

enum class LoadStatus {
    Loaded,
    Absent,
    KeyMismatch,
    Rejected,
};

struct Counters {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t writes = 0;
    std::uint64_t loadFailures = 0;
    std::uint64_t writeFailures = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t planHits = 0;
    std::uint64_t planMisses = 0;
    std::uint64_t planWrites = 0;
    std::uint64_t planLoadFailures = 0;
    std::uint64_t reloads = 0;
    std::uint64_t reloadFailures = 0;
    std::uint64_t planFailureHits = 0;
    std::uint64_t planFailureWrites = 0;
    std::uint64_t planReloads = 0;
    std::uint64_t planReloadFailures = 0;
};

[[nodiscard]] std::uint64_t SourceVersion();

void BuildKey(const RecompileRequest& request, std::uint32_t hostSubgroupSize, std::vector<std::byte>& key);

[[nodiscard]] std::string EntryName(std::span<const std::byte> key);

void EncodeResult(const RecompileResult& result, std::vector<std::byte>& out);
[[nodiscard]] bool DecodeResult(std::span<const std::byte> bytes, RecompileResult& result);

[[nodiscard]] std::vector<std::byte> EncodeEntry(std::span<const std::byte> key, const CompiledVariant& variant);
[[nodiscard]] LoadStatus DecodeEntry(std::span<const std::byte> file, std::span<const std::byte> key, CompiledVariant& variant);

void BuildPlanKey(const RecompileRequest& request, std::uint32_t hostSubgroupSize, std::vector<std::byte>& key);
[[nodiscard]] bool EncodePlan(const IrResourcePlan& plan, std::vector<std::byte>& out);
[[nodiscard]] bool DecodePlan(std::span<const std::byte> bytes, IrResourcePlan& plan);
[[nodiscard]] std::vector<std::byte> EncodePlanEntry(std::span<const std::byte> key, const IrResourcePlan& plan);
[[nodiscard]] LoadStatus DecodePlanEntry(std::span<const std::byte> file, std::span<const std::byte> key, IrResourcePlan& plan);
void BuildPlanFailureKey(std::span<const std::byte> planKey, std::vector<std::byte>& key);
[[nodiscard]] std::vector<std::byte> EncodePlanFailureEntry(std::span<const std::byte> key, const std::string& message);
[[nodiscard]] LoadStatus DecodePlanFailureEntry(std::span<const std::byte> file, std::span<const std::byte> key, std::string& message);

[[nodiscard]] bool Enabled();
[[nodiscard]] std::filesystem::path EntryDirectory();

[[nodiscard]] bool Load(std::span<const std::byte> key, CompiledVariant& variant);
[[nodiscard]] bool Reload(std::span<const std::byte> key, CompiledVariant& variant);
void Store(std::vector<std::byte> key, std::shared_ptr<const CompiledVariant> variant, std::function<void()> stored = {});
[[nodiscard]] bool LoadPlan(std::span<const std::byte> key, IrResourcePlan& plan);
void StorePlan(std::vector<std::byte> key, std::shared_ptr<const IrResourcePlan> plan, std::function<void()> stored = {});
[[nodiscard]] bool ReloadPlan(std::span<const std::byte> key, IrResourcePlan& plan);
[[nodiscard]] bool LoadPlanFailure(std::span<const std::byte> planKey, std::string& message);
void StorePlanFailure(std::span<const std::byte> planKey, std::string message);
void Flush();
[[nodiscard]] Counters Totals();

struct UserDataCallee {
    std::uint32_t userDataRegister = 0;
    std::uint32_t linkRegister = 0;
    std::vector<std::uint32_t> code;
};

[[nodiscard]] std::vector<std::byte> EncodeUserDataCallee(const UserDataCallee& callee);
[[nodiscard]] std::optional<UserDataCallee> DecodeUserDataCallee(std::span<const std::byte> file);
[[nodiscard]] std::string UserDataCalleeName(const UserDataCallee& callee);
[[nodiscard]] std::filesystem::path UserDataCalleeDirectory();
void StoreUserDataCallee(const UserDataCallee& callee);
[[nodiscard]] std::vector<UserDataCallee> LoadUserDataCallees();

}

#endif

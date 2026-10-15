#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TOOLS_SHADERCACHEENTRY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TOOLS_SHADERCACHEENTRY_HPP

#include "CacheKey.hpp"
#include "Recompiler.hpp"
#include "ShaderDiskCache.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver::Tools {

inline constexpr std::uint64_t CacheEntryCodeAddress = 0x1000000000ull;

struct CacheEntryRequest {
    std::vector<std::uint32_t> code;
    std::vector<std::uint32_t> capabilities;
    std::vector<std::string> extensionText;
    std::vector<std::string_view> extensions;
    std::vector<std::uint32_t> userData;
    std::vector<std::uint32_t> linkedCallFallbacks;
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<std::uint64_t> key;
    ShaderRecompiler::RecompileRequest request{};
    std::uint32_t hostSubgroupSize = 0;
    std::size_t storedWords = 0;
    std::uint64_t storedHash = 0xcbf29ce484222325ull;
};

namespace CacheEntryDetail {

class Bytes {
public:
    explicit Bytes(std::span<const std::byte> bytes) : bytes(bytes) {}

    template<typename TValue>
    TValue Read() {
        if (bytes.size() - cursor < sizeof(TValue)) throw std::runtime_error("shader cache entry ends early");
        TValue value;
        std::memcpy(&value, bytes.data() + cursor, sizeof(TValue));
        cursor += sizeof(TValue);
        return value;
    }

    void Seek(std::size_t offset) {
        if (offset > bytes.size()) throw std::runtime_error("shader cache entry ends early");
        cursor = offset;
    }

private:
    std::span<const std::byte> bytes;
    std::size_t cursor = 0;
};

class Words {
public:
    explicit Words(std::span<const std::uint64_t> words) : words(words) {}

    std::uint64_t Next() {
        if (cursor >= words.size()) throw std::runtime_error("shader cache key ends early");
        return words[cursor++];
    }

    std::uint32_t Next32() {
        return static_cast<std::uint32_t>(Next());
    }

    bool NextBool() {
        return Next() != 0u;
    }

    [[nodiscard]] bool Done() const {
        return cursor == words.size();
    }

private:
    std::span<const std::uint64_t> words;
    std::size_t cursor = 0;
};

inline constexpr std::uint32_t FileMagic = 0x43535041u;
inline constexpr std::size_t HeaderBytes = 48;

}

inline std::unique_ptr<CacheEntryRequest> ReadCacheEntry(std::span<const std::byte> file, bool graphics) {
    using namespace CacheEntryDetail;
    if (file.size() < HeaderBytes) throw std::runtime_error("shader cache entry ends early");
    Bytes bytes(file);
    if (bytes.Read<std::uint32_t>() != FileMagic) throw std::runtime_error("not a shader cache entry");
    if (bytes.Read<std::uint32_t>() != ShaderRecompiler::ShaderDiskCache::FormatVersion) throw std::runtime_error("shader cache entry has another format version");
    bytes.Read<std::uint64_t>();
    const auto keyBytes = bytes.Read<std::uint64_t>();
    if (keyBytes > file.size() - HeaderBytes) throw std::runtime_error("shader cache entry ends early");
    bytes.Seek(HeaderBytes + 16u);
    const auto count = bytes.Read<std::uint64_t>();
    if (count == 0u || count > keyBytes / sizeof(std::uint64_t)) return nullptr;
    auto entry = std::make_unique<CacheEntryRequest>();
    entry->key.resize(count);
    for (auto& word : entry->key) word = bytes.Read<std::uint64_t>();
    entry->code.resize(bytes.Read<std::uint64_t>());
    for (auto& word : entry->code) word = bytes.Read<std::uint32_t>();
    entry->hostSubgroupSize = bytes.Read<std::uint32_t>();
    auto& request = entry->request;
    request.layout.descriptorSet = bytes.Read<std::uint32_t>();
    request.layout.firstBinding = bytes.Read<std::uint32_t>();
    request.layout.pushConstantOffsetBytes = bytes.Read<std::uint32_t>();
    request.layout.pushConstantSizeBytes = bytes.Read<std::uint32_t>();
    const auto keyEnd = HeaderBytes + keyBytes;
    const auto padding = (8u - keyEnd % 8u) % 8u;
    if (padding > file.size() - keyEnd) throw std::runtime_error("shader cache entry ends early");
    bytes.Seek(keyEnd + padding);
    bytes.Read<std::uint32_t>();
    entry->storedWords = bytes.Read<std::uint64_t>();
    for (std::size_t index = 0; index < entry->storedWords; ++index) entry->storedHash = (entry->storedHash ^ bytes.Read<std::uint32_t>()) * 0x100000001b3ull;

    Words key(entry->key);
    if (key.Next32() != ShaderRecompiler::RuntimeAbi::Version) throw std::runtime_error("shader cache entry has another runtime ABI version");
    const auto stage = static_cast<ShaderRecompiler::ShaderStage>(key.Next());
    if (stage != ShaderRecompiler::ShaderStage::Compute && (!graphics || (stage != ShaderRecompiler::ShaderStage::Fragment && stage != ShaderRecompiler::ShaderStage::Vertex))) return nullptr;
    if (key.Next() != entry->code.size()) throw std::runtime_error("shader cache key holds code words; it was written with APS5_NO_CODE_HASH_KEY");
    key.Next();
    request.context.waveSize = key.Next32();
    request.context.userDataBaseRegister = key.Next32();
    entry->userData.assign(key.Next(), 0u);
    if (stage == ShaderRecompiler::ShaderStage::Compute) {
        if (!key.NextBool()) throw std::runtime_error("compute shader cache key has no compute stage information");
        ShaderRecompiler::ShaderComputeStageInfo compute{};
        for (auto& value : compute.numThreads) value = key.Next32();
        compute.ldsSizeDwords = key.Next32();
        for (auto& value : compute.groupIdEnable) value = key.NextBool();
        compute.tgSizeEnable = key.NextBool();
        compute.threadIdComponentCount = key.Next32();
        if (key.NextBool()) compute.partialThreads = {1u, 1u, 1u};
        compute.scratchDwords = key.Next32();
        request.context.compute = compute;
        if (key.NextBool() || key.NextBool()) throw std::runtime_error("compute shader cache key has pixel or vertex stage information");
    } else if (stage == ShaderRecompiler::ShaderStage::Vertex) {
        if (key.NextBool() || key.NextBool() || !key.NextBool()) throw std::runtime_error("vertex shader cache key has no vertex stage information");
        ShaderRecompiler::ShaderVertexStageInfo info{};
        info.fetchAttribReg = key.Next32();
        info.fetchBufferReg = key.Next32();
        info.fetchEmbedded = key.NextBool();
        info.paClVsOutCntl = key.Next32();
        info.resourcesNum = key.Next32();
        if (info.resourcesNum > info.resources.size()) throw std::runtime_error("vertex shader cache key has too many vertex resources");
        for (std::uint32_t index = 0; index < info.resourcesNum; ++index) {
            if (!info.fetchEmbedded) {
                const auto numericClass = static_cast<ShaderRecompiler::IrTextureNumericClass>(key.Next());
                const auto format = numericClass == ShaderRecompiler::IrTextureNumericClass::Uint ? ShaderRecompiler::IrBufferFormat::Format32UInt : numericClass == ShaderRecompiler::IrTextureNumericClass::Sint ? ShaderRecompiler::IrBufferFormat::Format32SInt : ShaderRecompiler::IrBufferFormat::Format32Float;
                info.resources[index].fields[3] = static_cast<std::uint32_t>(format) << 12u;
            }
            auto& destination = info.resourcesDst[index];
            destination.registerStart = static_cast<std::int32_t>(key.Next());
            destination.registersNum = static_cast<std::int32_t>(key.Next());
            destination.attrId = static_cast<std::int32_t>(key.Next());
            destination.fetchIndex = key.Next32();
        }
        request.context.vertex = info;
    } else {
        if (key.NextBool() || !key.NextBool()) throw std::runtime_error("pixel shader cache key has no pixel stage information");
        ShaderRecompiler::ShaderPixelStageInfo info{};
        info.interpolatorCount = key.Next32();
        if (info.interpolatorCount > info.interpolatorSettings.size()) throw std::runtime_error("pixel shader cache key has too many interpolators");
        for (std::uint32_t index = 0; index < info.interpolatorCount; ++index) info.interpolatorSettings[index] = key.Next32();
        info.wave32 = key.NextBool();
        info.inputAddr = key.Next32();
        for (bool* flag : {&info.hasPerspectiveCenterVgpr, &info.perspectiveCentroid, &info.posX, &info.posY, &info.posZ, &info.posW, &info.frontFace, &info.frontFaceAllBits, &info.ancillary, &info.sampleShading, &info.noPerspective, &info.linearCentroid, &info.pixelKillEnable, &info.depthExportEnable, &info.sampleMaskExportEnable, &info.earlyZ, &info.executeOnNoop}) *flag = key.NextBool();
        info.conservativeZExport = static_cast<ShaderRecompiler::ConservativeZExport>(key.Next());
        info.orderedPixelShader = key.NextBool();
        info.alphaToCoverage = key.NextBool();
        info.stencilRefExportEnable = key.NextBool();
        info.ditheredAlphaToCoverage = key.NextBool();
        info.quadPixelMask = static_cast<std::uint8_t>(key.Next());
        for (auto& mask : info.quadSampleMask) mask = static_cast<std::uint16_t>(key.Next());
        info.extraLdsDwords = key.Next32();
        for (auto& mode : info.targetOutputMode) mode = static_cast<std::uint8_t>(key.Next());
        for (std::uint8_t target = 0; target < info.targetExportMapping.size(); ++target) info.targetExportMapping[target] = target;
        request.context.pixel = info;
        if (key.NextBool()) throw std::runtime_error("pixel shader cache key has vertex stage information");
    }
    if (key.NextBool()) {
        ShaderRecompiler::ShaderFloatMode floatMode{};
        floatMode.floatMode = key.Next32();
        floatMode.dx10Clamp = key.NextBool();
        floatMode.ieeeMode = key.NextBool();
        floatMode.fp16Overflow = key.NextBool();
        request.context.floatMode = floatMode;
    }
    entry->linkedCallFallbacks.resize(key.Next());
    for (auto& programCounter : entry->linkedCallFallbacks) programCounter = key.Next32();
    auto& target = request.target;
    target.vulkanVersion = key.Next32();
    target.spirvVersion = key.Next32();
    target.subgroupSize = key.Next32();
    target.bdaAbiVersion = key.Next32();
    entry->capabilities.resize(key.Next());
    for (auto& capability : entry->capabilities) capability = key.Next32();
    entry->extensionText.resize(key.Next());
    for (auto& text : entry->extensionText) {
        text.resize(key.Next());
        for (auto& character : text) character = static_cast<char>(key.Next());
    }
    for (const auto& text : entry->extensionText) entry->extensions.emplace_back(text);
    target.supportedCapabilities = entry->capabilities;
    target.supportedExtensions = entry->extensions;
    target.fragmentShaderBarycentricEnabled = key.NextBool();
    for (auto& value : target.maxWorkgroupSize) value = key.Next32();
    target.maxWorkgroupInvocations = key.Next32();
    target.maxWorkgroupSharedMemoryBytes = key.Next32();
    if (key.NextBool()) {
        ShaderRecompiler::MeshTargetLimits mesh{};
        for (auto& value : mesh.maxWorkgroupSize) value = key.Next32();
        mesh.maxWorkgroupInvocations = key.Next32();
        mesh.maxSharedMemoryBytes = key.Next32();
        mesh.maxOutputVertices = key.Next32();
        mesh.maxOutputPrimitives = key.Next32();
        mesh.maxOutputComponents = key.Next32();
        mesh.maxOutputMemoryBytes = key.Next32();
        mesh.outputPerVertexGranularity = key.Next32();
        mesh.outputPerPrimitiveGranularity = key.Next32();
        target.mesh = mesh;
    }
    if (key.NextBool()) {
        ShaderRecompiler::TessellationTargetLimits tessellation{};
        tessellation.maxPatchSize = key.Next32();
        tessellation.maxControlPerVertexInputComponents = key.Next32();
        tessellation.maxControlPerVertexOutputComponents = key.Next32();
        tessellation.maxControlPerPatchOutputComponents = key.Next32();
        tessellation.maxControlTotalOutputComponents = key.Next32();
        tessellation.maxEvaluationInputComponents = key.Next32();
        tessellation.maxEvaluationOutputComponents = key.Next32();
        target.tessellation = tessellation;
    }
    target.nonConstantImageOffsets = key.NextBool();
    target.srgbDecodeFormats = key.Next32();
    target.narrowSubgroupClock = key.NextBool();
    target.float64DenormPreserve = key.NextBool();
    target.registrationModules = key.NextBool();
    target.registrationWordLimit = key.Next32();
    target.vertexSubgroups = key.NextBool();
    target.float16DenormPreserve = key.NextBool();
    target.robustBufferAccess2 = key.NextBool();
    key.Next();
    key.Next();
    key.Next();
    if (!key.Done()) throw std::runtime_error("shader cache key has more fields than this build writes");
    entry->memory.push_back({CacheEntryCodeAddress, std::as_bytes(std::span<const std::uint32_t>(entry->code))});
    request.shader = {stage, CacheEntryCodeAddress, entry->code, 0, {}};
    request.context.userData = entry->userData;
    request.context.linkedCallFallbacks = entry->linkedCallFallbacks;
    request.context.memory = entry->memory;
    std::vector<std::uint64_t> rebuilt;
    ShaderRecompiler::RecompileCacheKey::Build(request, rebuilt);
    if (rebuilt != entry->key) throw std::runtime_error("shader cache key does not match the request this build rebuilds from it");
    return entry;
}

inline std::unique_ptr<CacheEntryRequest> ReadComputeCacheEntry(std::span<const std::byte> file) {
    return ReadCacheEntry(file, false);
}

}

#endif

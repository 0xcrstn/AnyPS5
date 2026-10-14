#include "../tools/ShaderCacheEntry.hpp"
#include "BdaAbi.hpp"
#include "CompiledVariant.hpp"

#include <array>
#include <cstdio>
#include <exception>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ShaderRecompiler;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SpirvTarget Target() {
    static constexpr std::array<std::uint32_t, 3> capabilities{spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{"SPV_KHR_8bit_storage", "SPV_KHR_physical_storage_buffer"};
    SpirvTarget target{0x00401000u, 0x00010300u, 32, BdaAbi::Version, capabilities, extensions, false, {1024, 1024, 64}, 1024, 32768, {}, {}};
    target.srgbDecodeFormats = 3u;
    target.registrationWordLimit = 12345u;
    return target;
}

std::vector<std::byte> Entry(const RecompileRequest& request, std::size_t words) {
    std::vector<std::byte> key;
    ShaderDiskCache::BuildKey(request, 32u, key);
    CompiledVariant variant{};
    std::vector<std::uint32_t> spirv(words, 0u);
    spirv.front() = spv::MagicNumber;
    variant.artifact.spirv = std::move(spirv);
    return ShaderDiskCache::EncodeEntry(key, variant);
}

void RoundTrip(bool partial, bool floatMode) {
    const std::vector<std::uint32_t> code{0xbf800000u, 0xbf810000u};
    const std::vector<std::uint32_t> userData(6u, 0u);
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x40000, code, 0, {}};
    request.target = Target();
    request.layout = {1, 2, 16, 112};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 2;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{8, 4, 2}, 512, {true, false, true}, true, 3, partial ? std::array<std::uint32_t, 3>{100, 4, 2} : std::array<std::uint32_t, 3>{}, 16};
    if (floatMode) request.context.floatMode = ShaderFloatMode{0xc0u, true, false, true};
    const auto file = Entry(request, 7u);
    const auto entry = AgcDriver::Tools::ReadComputeCacheEntry(file);
    Require(entry != nullptr, "a compute shader cache entry was not read");
    const auto& read = entry->request;
    Require(entry->code == code && entry->storedWords == 7u && entry->hostSubgroupSize == 32u, "the code or the stored module size was not read back");
    Require(read.layout.descriptorSet == 1u && read.layout.firstBinding == 2u && read.layout.pushConstantOffsetBytes == 16u && read.layout.pushConstantSizeBytes == 112u, "the binding layout was not read back");
    Require(read.context.waveSize == 64u && read.context.userDataBaseRegister == 2u && read.context.userData.size() == userData.size(), "the wave size or user data shape was not read back");
    const auto& compute = *read.context.compute;
    Require(compute.numThreads == std::array<std::uint32_t, 3>{8, 4, 2} && compute.ldsSizeDwords == 512u && compute.groupIdEnable == std::array<bool, 3>{true, false, true} && compute.tgSizeEnable && compute.threadIdComponentCount == 3u && compute.scratchDwords == 16u, "the compute stage was not read back");
    Require(compute.PartialGroups() == partial, "the partial-group flag was not read back");
    Require(read.context.floatMode.has_value() == floatMode && (!floatMode || *read.context.floatMode == *request.context.floatMode), "the float mode was not read back");
    Require(read.target.srgbDecodeFormats == 3u && read.target.registrationWordLimit == 12345u && read.target.supportedExtensions.size() == 2u && read.target.supportedExtensions[1] == "SPV_KHR_physical_storage_buffer", "the target was not read back");
    std::vector<std::byte> rebuilt;
    ShaderDiskCache::BuildKey(read, entry->hostSubgroupSize, rebuilt);
    std::vector<std::byte> original;
    ShaderDiskCache::BuildKey(request, 32u, original);
    Require(rebuilt == original, "the request read back does not build the stored key");
}

void Rejections() {
    const std::vector<std::uint32_t> code{0xbf810000u};
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x40000, code, 0, {}};
    request.target = Target();
    request.layout = {0, 0, 0, 128};
    request.context.waveSize = 64;
    request.context.pixel = ShaderPixelStageInfo{};
    Require(AgcDriver::Tools::ReadComputeCacheEntry(Entry(request, 5u)) == nullptr, "a pixel shader entry was read as a compute shader");
    request.shader.stage = ShaderStage::Compute;
    request.context.pixel.reset();
    request.context.compute = ShaderComputeStageInfo{{64, 1, 1}, 0, {false, false, false}, false, 1, {}, 0};
    auto file = Entry(request, 5u);
    file.resize(64u);
    bool failed = false;
    try {
        static_cast<void>(AgcDriver::Tools::ReadComputeCacheEntry(file));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    Require(failed, "a truncated shader cache entry was accepted");
}

}

int main() {
    try {
        RoundTrip(false, false);
        RoundTrip(true, true);
        Rejections();
        std::printf("shader cache entry tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

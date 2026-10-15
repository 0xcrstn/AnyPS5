#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ShaderRecompiler;

alignas(256) std::array<std::uint32_t, 65536> Output{};
alignas(256) std::array<std::uint8_t, 32768> Texture{};
alignas(256) constexpr std::array<std::uint32_t, 20> Code{
    0x7e080218u, 0x343c0885u, 0x4a3c3d00u, 0x34063c84u, 0x2c3e3c87u, 0x363c3cffu, 0x0000007fu,
    0x7e140280u, 0x7e160280u, 0x7e180280u, 0x7e1a0280u, 0xf0001f08u, 0x00010a1eu, 0xf0001f08u, 0x00010e1eu,
    0xbf8c3f70u, 0x06141d0au, 0xe0781000u, 0x80000a03u, 0xbf810000u,
};

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void SetOutlining(bool enabled) {
#ifdef _WIN32
    _putenv_s("APS5_OUTLINE_IMAGE_MODES", enabled ? "1" : "");
#else
    if (enabled) setenv("APS5_OUTLINE_IMAGE_MODES", "1", 1);
    else unsetenv("APS5_OUTLINE_IMAGE_MODES");
#endif
}

std::vector<std::uint32_t> Compile() {
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(Output.data());
    const auto textureAddress = reinterpret_cast<std::uintptr_t>(Texture.data());
    const std::array<std::uint32_t, 4> buffer{
        static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>((outputAddress >> 32u) & 0xffffu),
        static_cast<std::uint32_t>(Output.size() * 4u), 0x31016facu,
    };
    const std::array<std::uint32_t, 8> image{
        static_cast<std::uint32_t>(textureAddress >> 8u),
        static_cast<std::uint32_t>((textureAddress >> 40u) & 0xffu) | (6u << 20u) | (3u << 30u),
        31u | (127u << 14u), 0x24cu | (9u << 28u), 0u, 0u, 0u, 0u,
    };
    const std::array<std::uint32_t, 4> sampler{0x92u, 0x00fff000u, 0u, 0u};
    std::array<std::uint32_t, 24> userData{};
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(image.begin(), image.end(), userData.begin() + 4);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 12);
    const std::array<MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const std::array<std::uint32_t, 15> capabilities{
        spv::CapabilityShader, spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses,
        spv::CapabilityGroupNonUniform, spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle,
        spv::CapabilityImageQuery, spv::CapabilityStorageImageExtendedFormats, spv::CapabilityStorageImageReadWithoutFormat,
        spv::CapabilityStorageImageWriteWithoutFormat, spv::CapabilitySampled1D, spv::CapabilityImage1D,
        spv::CapabilitySampledBuffer, spv::CapabilityImageBuffer, spv::CapabilityFloat16,
    };
    const std::array<std::string_view, 4> extensions{
        "SPV_EXT_descriptor_indexing", "SPV_KHR_float_controls", "SPV_KHR_physical_storage_buffer", "SPV_KHR_16bit_storage",
    };
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}};
    request.context = {32, 0, userData, ShaderComputeStageInfo{{32, 1, 1}, 0, {true, false, false}, false, 1}, std::nullopt, std::nullopt, memory};
    request.target = {0x00403000u, 0x00010600u, 32, BdaAbi::Version, capabilities, extensions, false, {1024, 1024, 64}, 1024, 32768, {}, {}};
    request.layout = {0, 0, 0, 128};
    request.useCache = false;
    const auto result = Recompile(request);
    const auto words = result.spirv.Words();
    Require(words.size() > 5u && words[0] == spv::MagicNumber, "the recompiler produced no SPIR-V module");
    return {words.begin(), words.end()};
}

struct Shape {
    std::map<std::string, std::uint32_t> named;
    std::map<std::uint32_t, std::size_t> calls;
    std::size_t functions = 0;
};

Shape Describe(const std::vector<std::uint32_t>& words) {
    Shape shape;
    std::map<std::uint32_t, std::string> names;
    std::set<std::uint32_t> functions;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> 16u;
        const auto opcode = words[offset] & 0xffffu;
        Require(count != 0u && offset + count <= words.size(), "the module has a malformed instruction");
        if (opcode == spv::OpName) names[words[offset + 1]] = reinterpret_cast<const char*>(&words[offset + 2]);
        if (opcode == spv::OpFunction) functions.insert(words[offset + 2]);
        if (opcode == spv::OpFunctionCall) ++shape.calls[words[offset + 3]];
        offset += count;
    }
    shape.functions = functions.size();
    for (const auto& [id, name] : names) {
        if (functions.contains(id) && name.starts_with("image_modes_")) shape.named[name] = id;
    }
    return shape;
}

}

int main() {
    try {
        SetOutlining(false);
        const auto inlineWords = Compile();
        const auto inlineShape = Describe(inlineWords);
        Require(inlineShape.named.empty(), "image modes were outlined while APS5_OUTLINE_IMAGE_MODES is off");
        SetOutlining(true);
        const auto outlinedWords = Compile();
        const auto outlinedShape = Describe(outlinedWords);
        Require(outlinedShape.named.size() == 1u, "two samples of one image resource did not share one outlined image-mode function (" + std::to_string(outlinedShape.named.size()) + " found)");
        const auto helper = outlinedShape.named.begin()->second;
        const auto calls = outlinedShape.calls.contains(helper) ? outlinedShape.calls.at(helper) : 0u;
        Require(calls == 2u, "the outlined image-mode function is called " + std::to_string(calls) + " times for two image accesses");
        Require(outlinedWords.size() < inlineWords.size(), "outlining did not shrink a module with two accesses to one image");
        Require(Compile() == outlinedWords, "outlined emission is not deterministic");
        SetOutlining(false);
        std::printf("image mode outlining passed: %zu words inline, %zu words outlined\n", inlineWords.size(), outlinedWords.size());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "image mode outlining test failed: %s\n", error.what());
        return 1;
    }
}

#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Layers = 4;
constexpr std::size_t LayerBytes = 65536;
constexpr std::size_t SurfaceBytes = LayerBytes * Layers;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::uint32_t Color32FloatInfo = (4u << 2u) | (7u << 8u);
constexpr std::uint32_t TiledAttrib3 = 0x4dc6c000u;
constexpr std::uint32_t Float32Export = 9;
constexpr std::uint32_t Initial = 0x5a5a5a5au;
constexpr std::uint32_t Stored = 0x3c3c3c3cu;
constexpr std::uint32_t Strip = 8;
constexpr float Drawn = 0.25f;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

alignas(256) std::array<std::array<std::uint32_t, 9>, Layers> LoadCodes{};
alignas(256) std::array<std::uint32_t, 16> PixelCode{};
alignas(256) std::array<std::uint32_t, Threads> Output{};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(SurfaceBytes, LayerBytes));
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, static_cast<DWORD>(SurfaceBytes), nullptr);
        if (block != nullptr && section != nullptr) GuestArena::GuestArenaMap_nid_postfix(block, SurfaceBytes, section, 0, PAGE_READWRITE);
#else
        void* mapped = mmap(nullptr, SurfaceBytes + LayerBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + LayerBytes - 1) & ~(static_cast<std::uintptr_t>(LayerBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + SurfaceBytes != begin + SurfaceBytes + LayerBytes) munmap(reinterpret_cast<void*>(aligned + SurfaceBytes), begin + LayerBytes - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, SurfaceBytes);
        }
#endif
        Require(block != nullptr, "continued pass targets: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, SurfaceBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, SurfaceBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, SurfaceBytes);
        if (section != nullptr) {
            GuestArena::GuestArenaForgetSection_nid_postfix(section);
            CloseHandle(section);
        }
#else
        munmap(block, SurfaceBytes);
        if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, SurfaceBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
#ifdef _WIN32
    HANDLE section = nullptr;
#endif
};

std::array<std::uint32_t, 8> SurfaceDescriptor(std::uint64_t address) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32Float << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2DArray << 28u),
        Layers - 1u,
        0u,
        0u,
        0u,
    };
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void LoadLayer(AgcDriver::VulkanDevice& device, std::uint64_t surface, std::uint32_t layer) {
    auto& code = LoadCodes[layer];
    code = {0x7e020280, 0x7e040280u | layer, 0xf0001128, 0x00010300, 0x34080082, 0xbf8c3f70, 0xe0701000, 0x80000304, 0xbf810000};
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto buffer = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    const auto texture = SurfaceDescriptor(surface);
    const std::array<std::uint32_t, 4> pointSampler{0u, 0xfffu << 12u, 1u << 26u, 0u};
    std::copy(pointSampler.begin(), pointSampler.end(), userData.begin() + 12);
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> span(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(span.data()), std::as_bytes(span)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(span.data()), span, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(span.data()));
    device.WaitIdle();
}

void RequireLayer(AgcDriver::VulkanDevice& device, std::uint64_t surface, std::uint32_t layer, const std::function<std::uint32_t(std::uint32_t)>& expected, const std::string& what) {
    LoadLayer(device, surface, layer);
    for (std::uint32_t x = 0; x < Threads; ++x) Require(Output[x] == expected(x), what + ": layer " + std::to_string(layer) + " texel (" + std::to_string(x) + ", 0) reads " + Hex(Output[x]) + ", expected " + Hex(expected(x)));
}

AgcDriver::Graphics::ColorTarget DecodeTarget(std::uint64_t address, std::uint32_t slice) {
    AgcDriver::Registers cx;
    cx[0x318] = static_cast<std::uint32_t>(address >> 8u);
    cx[0x31b] = slice | (slice << 13u);
    cx[0x31c] = Color32FloatInfo;
    cx[0x31d] = 0;
    cx[0x3b0] = ((Side - 1u) << 14u) | (Side - 1u);
    cx[0x3b8] = (TiledAttrib3 & ~(0x1fu << 14u)) | (TileR64KBX << 14u) | (Layers - 1u);
    cx[0x390] = static_cast<std::uint32_t>(address >> 40u);
    const auto color = AgcDriver::Graphics::DecodeColorBuffer(cx, 0);
    Require(color.arrayAddress == address && color.arraySlice == slice, "the color view did not keep its array and slice");
    return color;
}

void DrawStrip(AgcDriver::VulkanDevice& device, std::uint64_t surface, std::uint32_t slice, std::uint32_t strip) {
    const auto target = device.Target();
    constexpr std::uint32_t waveSize = 64;
    std::size_t at = 0;
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        PixelCode[at++] = 0x7e0002ffu | ((4u + channel) << 17u);
        PixelCode[at++] = std::bit_cast<std::uint32_t>(Drawn);
    }
    PixelCode[at++] = 0xf800180fu;
    PixelCode[at++] = 0x07060504u;
    PixelCode[at++] = 0xbf810000u;
    const std::span<const std::uint32_t> pixelCode(PixelCode.data(), at);
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto address = reinterpret_cast<std::uintptr_t>(Triangle.data());
    const std::array<std::uint32_t, 4> vertexBuffer{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(Triangle.size()), 0x01016facu};
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {waveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    const auto color = DecodeTarget(surface, slice);
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, waveSize, waveSize, std::nullopt, std::nullopt};
    state.colors = {color};
    state.color = color;
    state.hasColorTarget = true;
    state.renderExtent = {Side, Side};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Side), static_cast<float>(Side), -static_cast<float>(Side), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{static_cast<std::int32_t>(strip * Strip), 0}, {Strip, Side}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blends = {blend};
    state.blend = blend;
    state.blendConstants = {};

    const auto codeAddress = reinterpret_cast<std::uintptr_t>(pixelCode.data());
    const AgcDriver::Registers context{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, Float32Export}};
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{codeAddress, std::as_bytes(pixelCode)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, codeAddress, pixelCode, 0, {}},
        {waveSize, 0, {}, std::nullopt, AgcDriver::Graphics::DecodePixelStageInfo(context, AgcDriver::Graphics::ExportMappings(state)), std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
}

struct Counts {
    std::uint64_t lookups;
    std::uint64_t reuses;
};

Counts Now() {
    return {AgcDriver::Graphics::ResidentTargetLookups(), AgcDriver::Graphics::ContinuedPassTargetReuses()};
}

void Reset(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    device.WaitIdle();
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(texels), SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(AddressOf(texels), SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
}

std::uint32_t Value(bool drawn, std::uint32_t otherwise) {
    return drawn ? std::bit_cast<std::uint32_t>(Drawn) : otherwise;
}

void RequireCounts(const Counts& before, std::uint64_t lookups, std::uint64_t reuses, const std::string& what) {
    const auto after = Now();
    Require(after.lookups - before.lookups == lookups && after.reuses - before.reuses == reuses, what + ": " + std::to_string(after.lookups - before.lookups) + " target lookups and " + std::to_string(after.reuses - before.reuses) + " reuses, expected " + std::to_string(lookups) + " and " + std::to_string(reuses));
}

void Continued(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    const auto surface = AddressOf(texels);
    Reset(device, texels);
    const auto before = Now();
    DrawStrip(device, surface, 1, 0);
    DrawStrip(device, surface, 1, 1);
    DrawStrip(device, surface, 1, 2);
    RequireCounts(before, 1, 2, "three draws continuing one pass");
    RequireLayer(device, surface, 1, [](std::uint32_t x) { return Value(x < 3 * Strip, Initial); }, "continued draws");
    RequireLayer(device, surface, 0, [](std::uint32_t) { return Initial; }, "continued draws");
}

void SliceChange(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    const auto surface = AddressOf(texels);
    Reset(device, texels);
    const auto before = Now();
    DrawStrip(device, surface, 1, 0);
    DrawStrip(device, surface, 2, 1);
    RequireCounts(before, 2, 0, "a draw into another slice");
    RequireLayer(device, surface, 1, [](std::uint32_t x) { return Value(x < Strip, Initial); }, "slice change");
    RequireLayer(device, surface, 2, [](std::uint32_t x) { return Value(x >= Strip && x < 2 * Strip, Initial); }, "slice change");
}

void EndOfPass(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    const auto surface = AddressOf(texels);
    Reset(device, texels);
    const auto before = Now();
    DrawStrip(device, surface, 1, 0);
    device.WaitIdle();
    DrawStrip(device, surface, 1, 1);
    RequireCounts(before, 2, 0, "a draw after the pass ended");
    RequireLayer(device, surface, 1, [](std::uint32_t x) { return Value(x < 2 * Strip, Initial); }, "end of pass");
}

void CpuWrite(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    const auto surface = AddressOf(texels);
    Reset(device, texels);
    const auto before = Now();
    DrawStrip(device, surface, 1, 0);
    std::fill_n(reinterpret_cast<volatile std::uint32_t*>(texels + LayerBytes), LayerBytes / 4u, Stored);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    DrawStrip(device, surface, 1, 1);
    RequireCounts(before, 2, 0, "a draw after a CPU write to the target and an ordering point");
    RequireLayer(device, surface, 1, [](std::uint32_t x) { return Value(x >= Strip && x < 2 * Strip, Stored); }, "CPU write between draws");
}

void DriverStore(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    const auto surface = AddressOf(texels);
    Reset(device, texels);
    const auto before = Now();
    DrawStrip(device, surface, 1, 0);
    AgcDriver::GuestMemory::MarkWritten(surface + LayerBytes, 4);
    DrawStrip(device, surface, 1, 1);
    RequireCounts(before, 2, 0, "a draw after a store into the target in the same epoch");
}

}

int main() {
    try {
        GuestBlock block;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!AgcDriver::GuestMemory::Watched(AddressOf(block.Data()), SurfaceBytes)) {
            std::puts("skipped, the target memory is not write-watched");
            return VulkanTestSkipped;
        }
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Continued(*device, block.Data());
        SliceChange(*device, block.Data());
        EndOfPass(*device, block.Data());
        CpuWrite(*device, block.Data());
        DriverStore(*device, block.Data());
        Reset(*device, block.Data());
        std::puts("continued pass target tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

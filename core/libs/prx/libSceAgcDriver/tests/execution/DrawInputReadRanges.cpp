#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::ColorTargetLayout;
using AgcDriver::Graphics::ColorTileMode;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Color32FloatInfo = (4u << 2u) | (7u << 8u);
constexpr std::uint32_t TiledAttrib3 = 0x4dc6c000u;
constexpr std::uint32_t Float32Export = 9;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

using Color = std::array<float, 4>;

alignas(256) constexpr std::array<std::uint32_t, 10> ScalarCode{
    0xf4280100u, 0xfa000100u, 0xbf8cc07fu, 0x7e000204u, 0x7e020205u, 0x7e040206u, 0x7e060207u, 0xf800180fu, 0x03020100u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 6> FormattedCode{
    0xe00c0100u, 0x80000000u, 0xbf8c3f70u, 0xf800180fu, 0x03020100u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 11> ConstantCode{
    0x7e0002ffu, 0x3e800000u, 0x7e0202ffu, 0x00000000u, 0x7e0402ffu, 0x00000000u, 0x7e0602ffu, 0x3f800000u, 0xf800180fu, 0x03020100u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 11> OtherConstantCode{
    0x7e0002ffu, 0x3f400000u, 0x7e0202ffu, 0x00000000u, 0x7e0402ffu, 0x00000000u, 0x7e0602ffu, 0x3f800000u, 0xf800180fu, 0x03020100u, 0xbf810000u,
};

constexpr float OtherRed = 0.75f;

alignas(256) constexpr std::array<std::uint32_t, 8> ClampedVertexCode{
    0x260c0affu, 0x000003ffu, 0xe0382000u, 0x80000006u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u,
};

constexpr float ConstantRed = 0.25f;
constexpr std::uint32_t VertexRecords = 1024;
constexpr std::uint32_t FirstVertex = 100;

struct PixelProgram {
    std::span<const std::uint32_t> code;
    std::uint32_t word3;
    const char* name;
};

std::array<std::uint32_t, 4> TriangleDescriptor() {
    const auto address = reinterpret_cast<std::uintptr_t>(Triangle.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(Triangle.size()), 0x01016facu};
}

constexpr std::uint32_t ReadOffset = 0x100;
constexpr std::uint32_t InputBytes = 0x4000;

struct Block {
    explicit Block(std::size_t bytes) : bytes(bytes), watched(AgcDriver::GuestMemory::WriteWatched()) {
#ifdef _WIN32
        if (watched) {
            data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
            GuestArena::GuestArenaCommit_nid_postfix(data, bytes, PAGE_READWRITE, bytes);
        } else {
            data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
#else
        if (watched) {
            void* raw = mmap(nullptr, bytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Require(raw != MAP_FAILED, "cannot map the color target");
            const auto begin = reinterpret_cast<std::uintptr_t>(raw);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(raw, aligned - begin);
            if (aligned + bytes != begin + bytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + bytes), begin + BlockBytes - aligned);
            data = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
        } else {
            data = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, bytes));
        }
#endif
        Require(data != nullptr, "cannot allocate the color target");
        Require(!watched || AgcDriver::GuestMemory::Watched(Address(), bytes), "the color target is not write-watched");
        GuestAllocations::Mutation mutation;
        mutation.Add(data, bytes, true, true, true);
    }
    ~Block() {
        AgcDriver::Graphics::StorageTexture::FlushPending(Address(), bytes, nullptr, "test release");
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
#ifdef _WIN32
        if (watched) {
            GuestArena::GuestArenaReset_nid_postfix(data, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
        } else {
            VirtualFree(data, 0, MEM_RELEASE);
        }
#else
        if (watched) {
            munmap(data, bytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, bytes);
        } else {
            std::free(data);
        }
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t bytes;
    bool watched;
    std::uint8_t* data = nullptr;
};

AgcDriver::Graphics::ColorTarget DecodeTarget(const Block& block, std::uint32_t attrib3) {
    AgcDriver::Registers cx;
    cx[0x318] = static_cast<std::uint32_t>(block.Address() >> 8u);
    cx[0x31b] = 0;
    cx[0x31c] = Color32FloatInfo;
    cx[0x31d] = 0;
    cx[0x3b0] = ((Width - 1u) << 14u) | (Height - 1u);
    cx[0x3b8] = attrib3;
    cx[0x390] = static_cast<std::uint32_t>(block.Address() >> 40u);
    auto color = AgcDriver::Graphics::DecodeColorBuffer(cx, 0);
    Require(color.format == VK_FORMAT_R32_SFLOAT && color.elementBytes == 4 && color.bytes == block.bytes, "the COLOR_32 float target did not decode as an R32 surface");
    return color;
}

ShaderRecompiler::RecompileResult CompilePixel(AgcDriver::VulkanDevice& device, std::uint32_t vertexPush, std::span<const std::uint32_t> descriptor, const AgcDriver::Graphics::State& state, std::span<const std::uint32_t> pixelCode) {
    const auto target = device.Target();
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(pixelCode.data());
    const AgcDriver::Registers context{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, Float32Export}};
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{codeAddress, std::as_bytes(pixelCode)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, codeAddress, pixelCode, 0, {}},
        {64, 0, descriptor, std::nullopt, AgcDriver::Graphics::DecodePixelStageInfo(context, AgcDriver::Graphics::ExportMappings(state)), std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    return ShaderRecompiler::Recompile(fragment);
}

void Draw(AgcDriver::VulkanDevice& device, const AgcDriver::Graphics::ColorTarget& color, std::span<const std::uint32_t> descriptor, const PixelProgram& program, bool checkRanges, const std::array<std::uint32_t, 4>& vertexBuffer, std::uint32_t firstVertex, std::span<const std::uint32_t> vertexCode = VertexCode) {
    const auto target = device.Target();
    constexpr std::uint32_t waveSize = 64;
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(vertexCode.data()), std::as_bytes(vertexCode)}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0, {}},
        {waveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    if (checkRanges && firstVertex != 0) {
        const auto binding = std::find_if(vertexResult.bindings.begin(), vertexResult.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& candidate) { return candidate.role == ShaderRecompiler::DescriptorRole::GuestBuffers; });
        Require(binding != vertexResult.bindings.end() && binding->bufferReadRanges.size() == 1, "the vertex shader has no guest buffer binding with read ranges");
        const auto& ranges = binding->bufferReadRanges.front();
        const bool clamped = vertexCode.data() == ClampedVertexCode.data();
        const auto expectedSource = clamped ? ShaderRecompiler::BufferIndexSource::Expression : ShaderRecompiler::BufferIndexSource::VertexIndex;
        Require(ranges.bounded && ranges.ranges.size() == 1 && ranges.ranges.front().indexSource == expectedSource && ranges.ranges.front().begin == 0u && ranges.ranges.front().end == 16u, clamped ? "a buffer_load_dwordx4 at min(vertex id, 1023) was not analysed as an index expression of the vertex id" : "a buffer_load_dwordx4 at the vertex id was not analysed as reading 16 bytes of the vertex id's record");
    }
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, waveSize, waveSize, std::nullopt, std::nullopt};
    state.colors = {color};
    state.color = color;
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    state.blends = {blend};
    state.blend = blend;
    state.blendConstants = {};
    const auto pixelResult = CompilePixel(device, vertexPush, descriptor, state, program.code);
    if (checkRanges && firstVertex == 0) {
        const auto binding = std::find_if(pixelResult.bindings.begin(), pixelResult.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& candidate) { return candidate.role == ShaderRecompiler::DescriptorRole::GuestBuffers; });
        Require(binding != pixelResult.bindings.end() && binding->bufferReadRanges.size() == 1, "the pixel shader has no guest buffer binding with read ranges");
        const auto& ranges = binding->bufferReadRanges.front();
        Require(ranges.bounded && ranges.ranges.size() == 1 && ranges.ranges.front() == ShaderRecompiler::BufferReadRange{0u, 0u, ReadOffset, ReadOffset + 16u}, std::string(program.name) + " at offset 0x100 was not analysed as reading bytes 0x100 to 0x110");
    }
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false, firstVertex};
    device.Draw(state, draw, shaders);
}

std::vector<float> ReadBack(AgcDriver::VulkanDevice& device, const Block& block) {
    AgcDriver::Graphics::StorageTexture::FlushPending(block.Address(), block.bytes, nullptr, "test read-back");
    device.WaitIdle();
    std::vector<float> stored(block.bytes / 4u);
    AgcDriver::GuestMemory::Read(block.Address(), std::as_writable_bytes(std::span(stored)), 1);
    return stored;
}

void ExpectSurface(AgcDriver::VulkanDevice& device, const Block& block, const ColorTargetLayout& layout, float texel, const std::string& what) {
    const auto stored = ReadBack(device, block);
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            const auto index = layout.Offset(x, y) / 4u;
            if (stored[index] == texel) continue;
            char detail[160];
            std::snprintf(detail, sizeof(detail), ": pixel (%u, %u) holds %.9g, expected %.9g", x, y, static_cast<double>(stored[index]), static_cast<double>(texel));
            throw std::runtime_error(what + detail);
        }
    }
}

void ReadRangeTests(AgcDriver::VulkanDevice& device, Block& target, Block& input) {
    const ColorTargetLayout layout(Width, Height, ColorTileMode::RenderTarget, 4);
    Require(layout.Bytes() == target.bytes, "the test surface size disagrees with its layout");
    const auto color = DecodeTarget(target, TiledAttrib3);
    const std::array<Color, 3> cases{{
        {{1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.375f, 10.0f, 21.0f, 1.0f}},
        {{-6.5f, 3.0f, 10.0f, 0.0f}},
    }};
    const std::array<PixelProgram, 2> programs{{
        {ScalarCode, 0x31016facu, "s_buffer_load_dwordx4"},
        {FormattedCode, (0x31016facu & ~(0x7fu << 12u)) | (77u << 12u), "buffer_load_format_xyzw of a 32_32_32_32_FLOAT V#"},
    }};
    for (const auto& program : programs) {
        const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(input.Address()), static_cast<std::uint32_t>((input.Address() >> 32u) & 0xffffu), InputBytes, program.word3};
        bool first = true;
        for (const auto& expected : cases) {
            auto* floats = reinterpret_cast<float*>(input.data);
            for (std::size_t i = 0; i < InputBytes / 4u; ++i) floats[i] = 0.5f;
            std::memcpy(input.data + ReadOffset, expected.data(), sizeof(expected));
            Draw(device, color, descriptor, program, first, TriangleDescriptor(), 0u);
            first = false;
            std::memset(input.data, 0, InputBytes);
            char what[200];
            std::snprintf(what, sizeof(what), "a pixel shader exporting what %s loads at 0x100 of a 16 KiB buffer (%.9g, %.9g, %.9g, %.9g)", program.name, expected[0], expected[1], expected[2], expected[3]);
            ExpectSurface(device, target, layout, expected[0], what);
        }
    }
    const std::array<std::uint32_t, 4> vertexDescriptor{static_cast<std::uint32_t>(input.Address()), static_cast<std::uint32_t>((input.Address() >> 32u) & 0xffffu) | (16u << 16u), VertexRecords, 0x01016facu};
    const PixelProgram constant{ConstantCode, 0u, "a constant pixel shader"};
    const std::array<std::uint32_t, 4> unused{};
    auto* floats = reinterpret_cast<float*>(input.data);
    for (std::size_t i = 0; i < InputBytes / 4u; ++i) floats[i] = 0.0f;
    std::memcpy(input.data + FirstVertex * 16u, Triangle.data(), sizeof(Triangle));
    Draw(device, color, unused, constant, true, vertexDescriptor, FirstVertex);
    std::memset(input.data, 0, InputBytes);
    ExpectSurface(device, target, layout, ConstantRed, "a draw whose vertex shader loads its position at the vertex id from vertices 100 to 102 of a 1024-vertex buffer");
    const PixelProgram clampedConstant{OtherConstantCode, 0u, "a constant pixel shader after a clamped fetch"};
    std::memcpy(input.data + FirstVertex * 16u, Triangle.data(), sizeof(Triangle));
    Draw(device, color, unused, clampedConstant, true, vertexDescriptor, FirstVertex, ClampedVertexCode);
    std::memset(input.data, 0, InputBytes);
    ExpectSurface(device, target, layout, OtherRed, "a draw whose vertex shader loads its position at min(vertex id, 1023) from vertices 100 to 102 of a 1024-vertex buffer");
}

}

int main() {
    try {
        Block target(ColorTargetLayout(Width, Height, ColorTileMode::RenderTarget, 4).Bytes());
        Block input(BlockBytes);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        ReadRangeTests(*device, target, input);
        std::puts("draw input read range tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

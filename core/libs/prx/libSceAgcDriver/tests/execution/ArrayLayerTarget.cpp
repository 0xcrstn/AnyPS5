#include "prx/libSceAgcDriver/Execution/include/Coherence.hpp"
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
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using AgcDriver::Graphics::TextureDimension;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Layers = 4;
constexpr std::size_t LayerBytes = 65536;
constexpr std::size_t SurfaceBytes = LayerBytes * Layers;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::uint32_t Color32FloatInfo = (4u << 2u) | (7u << 8u);
constexpr std::uint32_t TiledAttrib3 = 0x4dc6c000u;
constexpr std::uint32_t Float32Export = 9;
constexpr std::uint32_t Initial = 0x5a5a5a5au;
constexpr std::uint32_t ScissorWidth = 16;
constexpr float Drawn = 0.25f;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

alignas(256) std::array<std::array<std::uint32_t, 9>, Layers> LoadCodes{};
alignas(256) constexpr std::array<std::uint32_t, 13> Sample2DCode{
    0x7e020d00, 0x060202f0, 0x100202ff, 0x3c000000, 0x7e0402ff, 0x3b800000, 0xf09c0108, 0x00610301,
    0x34080082, 0xbf8c3f70, 0xe0701000, 0x80000304, 0xbf810000,
};
alignas(256) std::array<std::uint32_t, 16> PixelCode{};
alignas(256) std::array<std::uint32_t, Threads> Output{};

void SampleSliceZero(AgcDriver::VulkanDevice& device, std::uint8_t* texels, std::uint32_t swizzleMode);

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
    explicit GuestBlock(std::size_t registeredBytes = SurfaceBytes, std::size_t registeredOffset = 0) : registeredOffset(registeredOffset) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(SurfaceBytes, LayerBytes));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, SurfaceBytes, PAGE_READWRITE, SurfaceBytes);
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
        Require(block != nullptr, "array layer target: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block + registeredOffset, registeredBytes, true, true, true);
    }

    ~GuestBlock() {
        if (registered) GuestAllocations::Mutation().Remove(block + registeredOffset);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, SurfaceBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, SurfaceBytes);
#else
        munmap(block, SurfaceBytes);
        if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, SurfaceBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }
    void Unregister() {
        GuestAllocations::Mutation().Remove(block + registeredOffset);
        registered = false;
    }

private:
    std::uint8_t* block = nullptr;
    std::size_t registeredOffset = 0;
    bool registered = true;
};

std::array<std::uint32_t, 8> SurfaceDescriptor(std::uint64_t address, std::uint32_t swizzleMode, std::uint32_t type, std::uint32_t lastLayer) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32Float << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (swizzleMode << 20u) | (type << 28u),
        lastLayer,
        0u,
        0u,
        0u,
    };
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void Dispatch(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Load(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::array<std::uint32_t, 8>& texture) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto buffer = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    const std::array<std::uint32_t, 4> pointSampler{0u, 0xfffu << 12u, 1u << 26u, 0u};
    std::copy(pointSampler.begin(), pointSampler.end(), userData.begin() + 12);
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    Dispatch(device, code, userData);
}

void LoadLayer(AgcDriver::VulkanDevice& device, std::uint64_t surface, std::uint32_t layer, std::uint32_t swizzleMode) {
    auto& code = LoadCodes[layer];
    code = {0x7e020280, 0x7e040280u | layer, 0xf0001128, 0x00010300, 0x34080082, 0xbf8c3f70, 0xe0701000, 0x80000304, 0xbf810000};
    Load(device, code, SurfaceDescriptor(surface, swizzleMode, Type2DArray, Layers - 1u));
}

void Sample2D(AgcDriver::VulkanDevice& device, std::uint64_t surface, std::uint32_t swizzleMode) {
    Load(device, Sample2DCode, SurfaceDescriptor(surface, swizzleMode, Type2D, 0u));
}

void RequireOutput(std::uint32_t inside, std::uint32_t outside, const std::string& what) {
    for (std::uint32_t x = 0; x < Threads; ++x) {
        const auto expected = x < ScissorWidth ? inside : outside;
        Require(Output[x] == expected, what + ": texel (" + std::to_string(x) + ", 0) reads " + Hex(Output[x]) + ", expected " + Hex(expected));
    }
}

AgcDriver::Graphics::ColorTarget DecodeTarget(std::uint64_t address, std::uint32_t slice, std::uint32_t swizzleMode, bool collapsed) {
    AgcDriver::Registers cx;
    cx[0x318] = static_cast<std::uint32_t>(address >> 8u);
    cx[0x31b] = slice | (slice << 13u);
    cx[0x31c] = Color32FloatInfo;
    cx[0x31d] = 0;
    cx[0x3b0] = ((Side - 1u) << 14u) | (Side - 1u);
    cx[0x3b8] = (TiledAttrib3 & ~(0x1fu << 14u)) | (swizzleMode << 14u) | (collapsed ? 0u : Layers - 1u);
    cx[0x390] = static_cast<std::uint32_t>(address >> 40u);
    const auto color = AgcDriver::Graphics::DecodeColorBuffer(cx, 0);
    Require(color.format == VK_FORMAT_R32_SFLOAT && color.bytes == LayerBytes && color.address == address + slice * LayerBytes, "the COLOR_32 float view did not decode as a 128x128 R32 slice of the array");
    if (collapsed) Require(color.arrayAddress == 0 && color.arrayLastSlice == 0, "a collapsed slice view unexpectedly describes a whole array");
    else Require(color.arrayAddress == address && color.arrayLastSlice == Layers - 1u && color.arraySlice == slice, "the color view did not keep its array and slice");
    return color;
}

void DrawSlice(AgcDriver::VulkanDevice& device, std::uint64_t surface, std::uint32_t slice, std::uint32_t swizzleMode, bool collapsed) {
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

    const auto color = DecodeTarget(surface, slice, swizzleMode, collapsed);
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
    state.scissor = {{0, 0}, {ScissorWidth, Side}};
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
    device.WaitIdle();
}

#ifdef _WIN32
class SharedMutationSurface {
public:
    SharedMutationSurface() {
        data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(2 * SurfaceBytes, LayerBytes));
        Require(data != nullptr, "guarded image mutation: cannot allocate shared views");
        try {
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, static_cast<DWORD>(SurfaceBytes), nullptr);
            Require(section != nullptr, "guarded image mutation: cannot create the original section");
            Require(DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) != 0, "guarded image mutation: cannot duplicate the original section");
            replacement = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, static_cast<DWORD>(SurfaceBytes), nullptr);
            Require(replacement != nullptr, "guarded image mutation: cannot create the replacement section");
            auto* initialized = static_cast<std::uint32_t*>(MapViewOfFile(replacement, FILE_MAP_WRITE, 0, 0, SurfaceBytes));
            Require(initialized != nullptr, "guarded image mutation: cannot initialize the replacement section");
            std::fill_n(initialized, SurfaceBytes / sizeof(std::uint32_t), Replacement);
            UnmapViewOfFile(initialized);
            GuestArena::GuestArenaMap_nid_postfix(data, SurfaceBytes, section, 0, PAGE_READWRITE);
            GuestArena::GuestArenaMap_nid_postfix(data + SurfaceBytes, SurfaceBytes, duplicate, 0, PAGE_READWRITE);
            GuestAllocations::Mutation().Add(data, SurfaceBytes, true, true, true);
            registered[0] = true;
            GuestAllocations::Mutation().Add(data + SurfaceBytes, SurfaceBytes, true, true, false);
            registered[1] = true;
        } catch (...) { release(); throw; }
    }

    ~SharedMutationSurface() { release(); }
    SharedMutationSurface(const SharedMutationSurface&) = delete;
    SharedMutationSurface& operator=(const SharedMutationSurface&) = delete;
    std::uint8_t* Data() const { return data; }

    void Mutate(bool alias, bool remap, const std::function<void()>& beforePhysical) {
        const auto index = alias ? 1u : 0u;
        auto* changed = data + index * SurfaceBytes;
        GuestAllocations::Mutation mutation;
        if (remap) {
            mutation.Protect(changed, SurfaceBytes, true, true, !alias, [&] {
                beforePhysical();
                GuestArena::GuestArenaReset_nid_postfix(changed, SurfaceBytes);
                GuestArena::GuestArenaMap_nid_postfix(changed, SurfaceBytes, replacement, 0, PAGE_READWRITE);
            });
        } else {
            mutation.Unmap(changed, SurfaceBytes, [&](const void* first, std::size_t bytes, const void*, bool) {
                Require(first == changed && bytes == SurfaceBytes, "guarded image mutation: the registry split an unexpected physical range");
                beforePhysical();
                GuestArena::GuestArenaReset_nid_postfix(changed, SurfaceBytes);
            });
            registered[index] = false;
        }
    }

    static constexpr std::uint32_t Replacement = 0x26262626u;

private:
    void release() noexcept {
        if (data != nullptr) {
            for (std::size_t index = 0; index < registered.size(); ++index) {
                if (registered[index]) {
                    try { GuestAllocations::Mutation().Remove(data + index * SurfaceBytes); } catch (...) {}
                    registered[index] = false;
                }
            }
            try {
                GuestArena::GuestArenaReset_nid_postfix(data, 2 * SurfaceBytes);
                GuestArena::GuestArenaRelease_nid_postfix(data, 2 * SurfaceBytes);
            } catch (...) {}
            data = nullptr;
        }
        for (auto* handle : {&replacement, &duplicate, &section}) {
            if (*handle == nullptr) continue;
            try { GuestArena::GuestArenaForgetSection_nid_postfix(*handle); } catch (...) {}
            CloseHandle(*handle);
            *handle = nullptr;
        }
    }
    std::uint8_t* data = nullptr;
    HANDLE section = nullptr;
    HANDLE duplicate = nullptr;
    HANDLE replacement = nullptr;
    std::array<bool, 2> registered{};
};

bool MutationPixels(const std::vector<std::uint32_t>& values) {
    constexpr std::uint32_t slice = 2;
    const auto first = values.begin() + slice * LayerBytes / sizeof(std::uint32_t);
    const auto last = first + LayerBytes / sizeof(std::uint32_t);
    const auto drawn = std::bit_cast<std::uint32_t>(Drawn);
    return std::count(first, last, drawn) == ScissorWidth * Side
        && std::all_of(first, last, [&](std::uint32_t value) { return value == Initial || value == drawn; })
        && std::all_of(values.begin(), first, [](std::uint32_t value) { return value == Initial; })
        && std::all_of(last, values.end(), [](std::uint32_t value) { return value == Initial; });
}

void GuardedImageMutation(AgcDriver::VulkanDevice& device, bool alias, bool remap) {
    SharedMutationSurface surface;
    auto* data = surface.Data();
    const auto address = AddressOf(data);
    Require(AgcDriver::Coherence::FaultOwnership(), "guarded image mutation: fault ownership is disabled");
    std::fill_n(reinterpret_cast<std::uint32_t*>(data), SurfaceBytes / sizeof(std::uint32_t), Initial);
    AgcDriver::GuestMemory::CollectWritesUncached(address, SurfaceBytes);
    AgcDriver::GuestMemory::CollectWritesUncached(address + SurfaceBytes, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        DrawSlice(device, address, 2u, TileR64KBX, false);
        const auto pending = StorageTexture::FindPending(address, SurfaceBytes);
        Require(pending != nullptr && pending->Descriptor().baseAddress == address && pending->Descriptor().dimension == TextureDimension::k2DArray, "guarded image mutation: the drawn array is not the pending GPU image");
        Require(GuestArena::GuestArenaMappingProtected_nid_postfix(address, SurfaceBytes) && GuestArena::GuestArenaMappingProtected_nid_postfix(address + SurfaceBytes, SurfaceBytes), "guarded image mutation: original and duplicate-handle alias are not guarded");
        std::vector<std::uint32_t> before(SurfaceBytes / sizeof(std::uint32_t));
        GuestArena::GuestArenaCopyDriverMemory_nid_postfix(before.data(), data, SurfaceBytes);
        Require(std::all_of(before.begin(), before.end(), [](std::uint32_t value) { return value == Initial; }), "guarded image mutation: GPU pixels were already published before the mutation");
        std::array<std::weak_ptr<const GuestAllocations::Range>, 2> ranges;
        {
            const auto live = GuestAllocations::GuestAllocationsAcquireAll_nid_postfix();
            for (const auto& range : live) {
                if (range->address == address) ranges[0] = range;
                if (range->address == address + SurfaceBytes) ranges[1] = range;
            }
        }
        Require(ranges[0].use_count() == 1 && ranges[1].use_count() == 1, "guarded image mutation: a remaining range lease would mask the unpinned mutation path");
    }
    std::vector<std::uint32_t> published(SurfaceBytes / sizeof(std::uint32_t));
    std::atomic<bool> completed{false};
    std::exception_ptr failure;
    bool physicalReached = false;
    std::thread mutation([&] {
        try {
            surface.Mutate(alias, remap, [&] {
                GuestArena::GuestArenaCopyDriverMemory_nid_postfix(published.data(), data + (alias ? 0 : SurfaceBytes), SurfaceBytes);
                Require(MutationPixels(published), "guarded image mutation: old GPU pixels were not published before physical replacement");
                Require(!GuestArena::GuestArenaMappingProtected_nid_postfix(address, SurfaceBytes) && !GuestArena::GuestArenaMappingProtected_nid_postfix(address + SurfaceBytes, SurfaceBytes), "guarded image mutation: the physical callback still owns a pending native guard");
                physicalReached = true;
            });
        } catch (...) { failure = std::current_exception(); }
        completed.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!completed.load(std::memory_order_acquire)) {
        std::fputs("guarded image mutation deadlocked before completing the native mapping change\n", stderr);
        std::_Exit(1);
    }
    mutation.join();
    if (failure) std::rethrow_exception(failure);
    Require(physicalReached, "guarded image mutation: no physical mapping change occurred");
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(StorageTexture::FindPending(address, SurfaceBytes) == nullptr, "guarded image mutation: the materialized image remained pending");
        AgcDriver::Graphics::ClearCachedTextures(device.Device());
    }
    GuestArena::GuestArenaCopyDriverMemory_nid_postfix(published.data(), data + (alias ? 0 : SurfaceBytes), SurfaceBytes);
    Require(MutationPixels(published), "guarded image mutation: surviving backing lost the published GPU pixels");
    if (remap) {
        GuestArena::GuestArenaCopyDriverMemory_nid_postfix(published.data(), data + (alias ? SurfaceBytes : 0), SurfaceBytes);
        Require(std::all_of(published.begin(), published.end(), [](std::uint32_t value) { return value == SharedMutationSurface::Replacement; }), "guarded image mutation: stale GPU pixels overwrote the replacement section");
    }
}
#endif

void Run(AgcDriver::VulkanDevice& device, std::uint8_t* texels, std::uint32_t swizzleMode, bool collapsed = false, std::uint32_t slice = 1) {
    const auto surface = AddressOf(texels);
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto drawn = std::bit_cast<std::uint32_t>(Drawn);
    DrawSlice(device, surface, slice, swizzleMode, collapsed);
    const auto pending = StorageTexture::FindPending(collapsed ? surface + slice * LayerBytes : surface, collapsed ? LayerBytes : SurfaceBytes);
    if (collapsed) Require(pending != nullptr && pending->Descriptor().dimension == TextureDimension::k2D && pending->Descriptor().baseAddress == surface + slice * LayerBytes, "a collapsed slice did not render into its shifted surface");
    else Require(pending != nullptr && pending->Descriptor().dimension == TextureDimension::k2DArray && pending->Descriptor().baseAddress == surface && pending->Descriptor().depthOrLastArray == Layers - 1u, "the draw into slice 1 did not render into the image of its array");
    LoadLayer(device, surface, slice, swizzleMode);
    RequireOutput(drawn, Initial, "slice " + std::to_string(slice) + " read through the array after the draw");
    for (std::uint32_t other = 0; other < Layers; ++other) {
        if (other == slice) continue;
        LoadLayer(device, surface, other, swizzleMode);
        RequireOutput(Initial, Initial, "slice " + std::to_string(other) + " after the draw into slice " + std::to_string(slice));
    }
    StorageTexture::FlushPending(surface, SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    LoadLayer(device, surface, slice, swizzleMode);
    RequireOutput(drawn, Initial, "slice " + std::to_string(slice) + " uploaded again from guest memory");
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    SampleSliceZero(device, texels, swizzleMode);
}

void SampleSliceZero(AgcDriver::VulkanDevice& device, std::uint8_t* texels, std::uint32_t swizzleMode) {
    const auto surface = AddressOf(texels);
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    DrawSlice(device, surface, 0, swizzleMode, false);
    const auto pending = StorageTexture::FindPending(surface, LayerBytes);
    Require(pending != nullptr && pending->Descriptor().dimension == TextureDimension::k2DArray, "the draw into slice 0 did not render into the image of its array");
    const auto plain = SurfaceDescriptor(surface, swizzleMode, Type2D, 0u);
    Require(AgcDriver::Graphics::Texture::CanCopyFrom(*pending, AgcDriver::Graphics::DecodeTextureResource(plain)), "a 2D texture over slice 0 cannot view the array image's first layer");
    Sample2D(device, surface, swizzleMode);
    RequireOutput(std::bit_cast<std::uint32_t>(Drawn), Initial, "slice 0 sampled as a 2D texture after the draw");
    StorageTexture::FlushPending(surface, SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
}

void UnregisteredUnusedLayer(AgcDriver::VulkanDevice& device, std::uint32_t slice) {
    GuestBlock block(LayerBytes, slice * LayerBytes);
    auto* texels = block.Data();
    const auto surface = AddressOf(texels);
    const auto active = surface + slice * LayerBytes;
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    DrawSlice(device, surface, slice, TileR64KBX, false);
    Require(StorageTexture::FindPending(surface, SurfaceBytes) != nullptr, "the registered slice was not pending before its CPU write-back");
    StorageTexture::FlushPending(active, LayerBytes, nullptr, "test");
    device.WaitIdle();
    const auto* first = reinterpret_cast<std::uint32_t*>(active);
    const auto* last = reinterpret_cast<std::uint32_t*>(active + LayerBytes);
    const auto unchanged = [](std::uint32_t value) { return value == Initial; };
    const auto drawn = std::count(first, last, std::bit_cast<std::uint32_t>(Drawn));
    const bool untouched = std::all_of(reinterpret_cast<const std::uint32_t*>(texels), first, unchanged) && std::all_of(last, reinterpret_cast<const std::uint32_t*>(texels + SurfaceBytes), unchanged);
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(drawn == ScissorWidth * Side, "CPU write-back discarded the registered slice because an unused array slice was unregistered");
    Require(untouched, "CPU write-back modified an unregistered unused slice");
}

void RetiredArrayLayer(AgcDriver::VulkanDevice& device) {
    GuestBlock block(LayerBytes);
    auto* texels = block.Data();
    const auto surface = AddressOf(texels);
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    DrawSlice(device, surface, 0u, TileR64KBX, false);
    Require(StorageTexture::FindPending(surface, SurfaceBytes) != nullptr, "the retired slice was not pending before it was unregistered");
    block.Unregister();
    StorageTexture::FlushPending(surface, LayerBytes, nullptr, "test");
    device.WaitIdle();
    const bool untouched = std::all_of(reinterpret_cast<const std::uint32_t*>(texels), reinterpret_cast<const std::uint32_t*>(texels + SurfaceBytes), [](std::uint32_t value) { return value == Initial; });
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(untouched, "CPU write-back stored results over a retired allocation");
}

class ExtraRegistration {
public:
    ExtraRegistration(void* address, std::size_t bytes) : address(address) { GuestAllocations::Mutation().Add(address, bytes, true, true, true); }
    ~ExtraRegistration() { if (address != nullptr) GuestAllocations::Mutation().Remove(address); }
    void Remove() {
        GuestAllocations::Mutation().Remove(address);
        address = nullptr;
    }

private:
    void* address;
};

void MixedRetiredLayers(AgcDriver::VulkanDevice& device, std::uint32_t survivor, std::uint32_t retired) {
    GuestBlock block(LayerBytes, survivor * LayerBytes);
    auto* texels = block.Data();
    ExtraRegistration registration(texels + retired * LayerBytes, LayerBytes);
    const auto surface = AddressOf(texels);
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    DrawSlice(device, surface, retired, TileR64KBX, false);
    DrawSlice(device, surface, survivor, TileR64KBX, false);
    Require(StorageTexture::FindPending(surface, SurfaceBytes) != nullptr, "mixed array slices were not pending before retirement");
    registration.Remove();
    StorageTexture::FlushPending(surface, SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    const auto* first = reinterpret_cast<const std::uint32_t*>(texels + survivor * LayerBytes);
    const auto* last = reinterpret_cast<const std::uint32_t*>(texels + (survivor + 1u) * LayerBytes);
    const auto drawn = std::bit_cast<std::uint32_t>(Drawn);
    const bool pixels = std::count(first, last, drawn) == ScissorWidth * Side;
    const bool padding = std::all_of(first, last, [&](std::uint32_t value) { return value == Initial || value == drawn; });
    const auto untouched = [](std::uint32_t value) { return value == Initial; };
    const bool otherSlices = std::all_of(reinterpret_cast<const std::uint32_t*>(texels), first, untouched) && std::all_of(last, reinterpret_cast<const std::uint32_t*>(texels + SurfaceBytes), untouched);
    const bool settled = StorageTexture::FindPending(surface, SurfaceBytes) == nullptr;
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(pixels && padding, "mixed retirement discarded or corrupted the still-registered array slice");
    Require(otherSlices, "mixed retirement wrote a retired or unused array slice");
    Require(settled, "mixed retirement left settled array slices pending");
}

void PartiallyRegisteredLayer(AgcDriver::VulkanDevice& device) {
    GuestBlock block(LayerBytes / 2u);
    auto* texels = block.Data();
    const auto surface = AddressOf(texels);
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    DrawSlice(device, surface, 0u, TileR64KBX, false);
    const auto pending = StorageTexture::FindPending(surface, SurfaceBytes);
    Require(pending != nullptr, "partially registered slice was not pending");
    bool refused = false;
    try {
        StorageTexture::FlushPending(surface, LayerBytes, nullptr, "test");
    } catch (const std::exception& error) {
        refused = std::string(error.what()).find("partially registered pending unit") != std::string::npos;
    }
    Require(refused, "partially registered pending slice publication was not refused");
    Require(StorageTexture::FindPending(surface, SurfaceBytes) == pending, "partial registration refusal lost the pending GPU image");
    Require(std::all_of(reinterpret_cast<const std::uint32_t*>(texels), reinterpret_cast<const std::uint32_t*>(texels + SurfaceBytes), [](std::uint32_t value) { return value == Initial; }), "partial registration refusal modified guest bytes");
    ExtraRegistration registration(texels + LayerBytes / 2u, LayerBytes / 2u);
    StorageTexture::FlushPending(surface, LayerBytes, nullptr, "test");
    device.WaitIdle();
    const auto* first = reinterpret_cast<const std::uint32_t*>(texels);
    const auto* last = reinterpret_cast<const std::uint32_t*>(texels + LayerBytes);
    const auto drawn = std::bit_cast<std::uint32_t>(Drawn);
    const bool survived = std::count(first, last, drawn) == ScissorWidth * Side && std::all_of(first, last, [&](std::uint32_t value) { return value == Initial || value == drawn; });
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(survived, "partial registration refusal failed to retain the GPU pixels for later publication");
}

}

int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const std::string_view mode(argv[1]);
            Require(mode == "--guarded-unmap" || mode == "--guarded-remap" || mode == "--guarded-alias-unmap" || mode == "--guarded-alias-remap", "unknown array layer target test mode");
#ifdef _WIN32
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            GuardedImageMutation(*device, mode == "--guarded-alias-unmap" || mode == "--guarded-alias-remap", mode == "--guarded-remap" || mode == "--guarded-alias-remap");
            std::puts("guarded image mutation tests passed");
            return 0;
#else
            return VulkanTestSkipped;
#endif
        }
        GuestBlock block;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, block.Data(), TileR64KBX);
        Run(*device, block.Data(), static_cast<std::uint32_t>(AgcDriver::Graphics::ColorTileMode::D64KBX));
        for (std::uint32_t slice = 1; slice < Layers; ++slice) Run(*device, block.Data(), TileR64KBX, true, slice);
        UnregisteredUnusedLayer(*device, 0u);
        UnregisteredUnusedLayer(*device, Layers - 1u);
        RetiredArrayLayer(*device);
        MixedRetiredLayers(*device, 0u, Layers - 1u);
        MixedRetiredLayers(*device, Layers - 1u, 0u);
        MixedRetiredLayers(*device, 1u, 2u);
        MixedRetiredLayers(*device, 2u, 1u);
        PartiallyRegisteredLayer(*device);
        std::puts("array layer target tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

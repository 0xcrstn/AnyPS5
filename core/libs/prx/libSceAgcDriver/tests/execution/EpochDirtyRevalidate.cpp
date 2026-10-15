#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Format8888Unorm = 56;
constexpr std::uint32_t TileRenderTarget64KB = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t SurfaceBytes = 65536;
constexpr std::size_t BlockBytes = 2 * 65536;
constexpr std::size_t OutputOffset = 65536;

alignas(256) constexpr std::array<std::uint32_t, 11> SampleSecondTexelCode{
    0x34060084, 0x7e2802ff, 0x3c400000, 0x7e2a02ff, 0x3b800000, 0xf09c0f08, 0x00610a14, 0xbf8c3f70,
    0xe0781000, 0x80000a03, 0xbf810000,
};

constexpr std::array<std::uint32_t, 4> PointSampler{0x00000092u, 0x00fff000u, 0u, 0u};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        Require(block != nullptr, "epoch dirty revalidate: cannot reserve the shared guest block");
        const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, BlockBytes, nullptr);
        Require(section != nullptr, "epoch dirty revalidate: cannot create the shared guest block");
        GuestArena::GuestArenaMap_nid_postfix(block, BlockBytes, section, 0, PAGE_READWRITE);
        twin = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        Require(twin != nullptr, "epoch dirty revalidate: cannot reserve the second guest mapping");
        GuestArena::GuestArenaMap_nid_postfix(twin, BlockBytes, section, 0, PAGE_READWRITE);
        CloseHandle(section);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "epoch dirty revalidate: cannot allocate the guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(twin, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(twin, BlockBytes);
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
#ifdef _WIN32
    std::uint8_t* twin = nullptr;
#endif
};

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint8_t* texels) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format8888Unorm << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileRenderTarget64KB << 20u) | (Type2D << 28u),
        0u,
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
    request.useCache = true;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

struct Run {
    AgcDriver::VulkanDevice& device;
    std::uint8_t* texels;
    std::uint32_t* output;
    std::size_t texel;
    std::vector<std::uint32_t> userData;
    ShaderRecompiler::RecompileResult program;
    std::uint64_t samples = 0;

    Run(AgcDriver::VulkanDevice& device, std::uint8_t* block) : device(device), texels(block), output(reinterpret_cast<std::uint32_t*>(block + OutputOffset)), userData(24, 0u) {
        const AgcDriver::Graphics::ColorTargetLayout layout(Side, Side, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
        texel = layout.Offset(1, 0);
        const auto texture = TextureDescriptor(texels);
        const auto results = BufferDescriptor(output, Threads * 16u);
        std::copy(results.begin(), results.end(), userData.begin());
        std::copy(texture.begin(), texture.end(), userData.begin() + 4);
        std::copy(PointSampler.begin(), PointSampler.end(), userData.begin() + 12);
    }

    void SetTexel(std::array<std::uint8_t, 4> bytes) {
        std::memcpy(texels + texel, bytes.data(), bytes.size());
    }

    void Sample(std::array<std::uint8_t, 4> expected, const char* when) {
        std::fill(output, output + Threads * 4u, 0xdeadbeefu);
        Dispatch(device, SampleSecondTexelCode, userData);
        ++samples;
        for (std::uint32_t index = 0; index < Threads * 4u; ++index) {
            const auto value = expected[index % 4u] == 0xffu ? 0x3f800000u : 0u;
            char message[256];
            std::snprintf(message, sizeof(message), "sampled %s: component %u of thread %u is 0x%08x, expected 0x%08x", when, index % 4u, index / 4u, output[index], value);
            Require(output[index] == value, message);
        }
    }
};

}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "on";
        Require(mode == "off" || mode == "on" || mode == "verify", "mode must be off, on or verify");
        GuestBlock block;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!AgcDriver::GuestMemory::Watched(AddressOf(block.Data()), BlockBytes)) {
            std::puts("skipped, the guest block is not write-watched");
            return VulkanTestSkipped;
        }
        AgcDriver::GuestMemory::SetEpochDirtySet(mode != "off");
        AgcDriver::GuestMemory::SetEpochDirtySetVerify(mode == "verify");
        Run run(*device, block.Data());
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::GuestMemory::BumpCollectEpoch();
        run.SetTexel({0xff, 0x00, 0xff, 0x00});
        run.Sample({0xff, 0x00, 0xff, 0x00}, "at the first use");
        Require(AgcDriver::GuestMemory::Watched(AddressOf(run.texels), SurfaceBytes), "the sampled surface import lost write tracking");
        run.Sample({0xff, 0x00, 0xff, 0x00}, "at the second use");
        AgcDriver::Graphics::StorageTexture::BumpPendingSerial();
        run.Sample({0xff, 0x00, 0xff, 0x00}, "after the first pending-source invalidation");
        const auto skipsBefore = AgcDriver::Graphics::ShaderResources::EpochSkips();
        const auto verificationsBefore = AgcDriver::GuestMemory::DirtySetStatistics().verifyChecks;
        for (int repeat = 0; repeat < 4; ++repeat) run.Sample({0xff, 0x00, 0xff, 0x00}, "while nothing changed in the epoch");
        const auto skipped = AgcDriver::Graphics::ShaderResources::EpochSkips() - skipsBefore;
        const auto verified = AgcDriver::GuestMemory::DirtySetStatistics().verifyChecks - verificationsBefore;
        if (mode == "off") Require(skipped == 0, "revalidations were skipped with the epoch dirty set off");
        else if (mode == "verify") Require(verified >= 4, "the epoch dirty set verified " + std::to_string(verified) + " of 4 revalidations of an unchanged template");
        else Require(skipped >= 4, "the epoch dirty set skipped " + std::to_string(skipped) + " of 4 revalidations of an unchanged template");
        const auto skipsBeforeInvalidation = AgcDriver::Graphics::ShaderResources::EpochSkips();
        const auto verificationsBeforeInvalidation = AgcDriver::GuestMemory::DirtySetStatistics().verifyChecks;
        AgcDriver::Graphics::StorageTexture::BumpPendingSerial();
        run.Sample({0xff, 0x00, 0xff, 0x00}, "after a pending-registry change no surface of the template saw");
        if (mode == "on") Require(AgcDriver::Graphics::ShaderResources::EpochSkips() > skipsBeforeInvalidation, "a pending-registry change that reached no surface of the template rejected its epoch proof");
        if (mode == "verify") Require(AgcDriver::GuestMemory::DirtySetStatistics().verifyChecks > verificationsBeforeInvalidation, "a pending-registry change that reached no surface of the template rejected its epoch proof");
        run.SetTexel({0x00, 0xff, 0x00, 0xff});
        AgcDriver::GuestMemory::BumpCollectEpoch();
        run.Sample({0x00, 0xff, 0x00, 0xff}, "after a CPU store and an ordering point");
        run.Sample({0x00, 0xff, 0x00, 0xff}, "again after the CPU store");
        const std::array<std::byte, 4> stored{std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x00}};
        AgcDriver::GuestMemory::Write(AddressOf(run.texels) + run.texel, stored);
        run.Sample({0xff, 0xff, 0x00, 0x00}, "after a driver store in the same epoch");
        run.SetTexel({0x00, 0x00, 0xff, 0xff});
        AgcDriver::GuestMemory::MarkWritten(AddressOf(run.texels) + run.texel, 4);
        run.Sample({0x00, 0x00, 0xff, 0xff}, "after a marked CPU store in the same epoch");
        run.SetTexel({0xff, 0xff, 0xff, 0x00});
        AgcDriver::GuestMemory::BumpCollectEpoch();
        run.Sample({0xff, 0xff, 0xff, 0x00}, "after another CPU store and ordering point");
        {
            GuestAllocations::Mutation mutation;
        }
        run.Sample({0xff, 0xff, 0xff, 0x00}, "after a registry mutation");
        if (mode == "verify") {
            Require(AgcDriver::Graphics::ShaderResources::EpochSkipDisagreements() == 0, "the epoch skip disagreed with the full revalidation");
            Require(AgcDriver::GuestMemory::DirtySetStatistics().verifyDisagreements == 0, "the dirty set disagreed with a stamp check");
        }
        std::printf("epoch dirty revalidate tests passed (%s: %llu samples, %llu revalidations skipped, %llu verified)\n", std::string(mode).c_str(), static_cast<unsigned long long>(run.samples), static_cast<unsigned long long>(AgcDriver::Graphics::ShaderResources::EpochSkips()), static_cast<unsigned long long>(AgcDriver::GuestMemory::DirtySetStatistics().verifyChecks));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

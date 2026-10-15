#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::VulkanDevice;
using Verdict = AgcDriver::Graphics::ShaderResources::EpochProofVerdict;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 32;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t OutputOffset = 4096;
alignas(256) constexpr std::array<std::uint32_t, 7> Code{
    0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0xe0701000u, 0x80010201u, 0xbf810000u,
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "dispatch epoch proofs: cannot allocate the guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, false, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Input() { return reinterpret_cast<std::uint32_t*>(block); }
    std::uint32_t* Output() { return reinterpret_cast<std::uint32_t*>(block + OutputOffset); }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

ShaderRecompiler::RecompileResult Compile(VulkanDevice& device, GuestBlock& guest) {
    std::array<std::uint32_t, 8> userData{};
    const auto input = BufferDescriptor(guest.Input(), Lanes * 4u);
    const auto output = BufferDescriptor(guest.Output(), Lanes * 4u);
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.ComputeTarget(Lanes),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::uint64_t Count(Verdict verdict) {
    return VulkanDevice::DispatchProofVerdicts().at(static_cast<std::size_t>(verdict));
}

void Run(VulkanDevice& device, GuestBlock& guest, const ShaderRecompiler::RecompileResult& shader, std::uint32_t seed, bool prepare, bool bumpBetween, const char* what) {
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) guest.Input()[lane] = seed + lane * 0x01010101u;
    std::fill_n(guest.Output(), Lanes, 0u);
    const auto program = reinterpret_cast<std::uintptr_t>(Code.data());
    auto prepared = prepare ? device.PrepareDispatch(shader, {}, Lanes, program) : nullptr;
    if (bumpBetween) AgcDriver::GuestMemory::BumpCollectEpoch();
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.Dispatch(shader, 1, 1, 1, {}, program, std::move(prepared));
    }
    device.WaitIdle();
    Require(std::equal(guest.Input(), guest.Input() + Lanes, guest.Output()), std::string("dispatch epoch proofs: ") + what + " copied other words");
}

}

int main() {
    try {
        if (!AgcDriver::Graphics::ParallelDispatchRevalidate()) {
            std::puts("skipped, APS5_PARALLEL_DISPATCH_REVALIDATE=0");
            return VulkanTestSkipped;
        }
        AgcDriver::GuestMemory::SetEpochDirtySet(true);
        GuestBlock guest;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto shader = Compile(*device, guest);
        AgcDriver::GuestMemory::BumpCollectEpoch();
        AgcDriver::Graphics::StorageTexture::BumpPendingSerial();
        for (std::uint32_t round = 0; round < 2; ++round) Run(*device, guest, shader, 0x1000u + round, true, false, "a dispatch building and proving its template");
        const auto skipsBefore = AgcDriver::Graphics::ShaderResources::EpochSkips();
        Run(*device, guest, shader, 0x2000u, false, false, "a serial dispatch proved by its epoch proof");
        if (AgcDriver::Graphics::ShaderResources::EpochSkips() == skipsBefore) {
            std::puts("skipped, the dispatch template is not served by the resource cache under an epoch proof");
            return VulkanTestSkipped;
        }
        const bool verify = AgcDriver::Graphics::VerifyParallelDispatchRevalidate();
        const auto adoptedBefore = Count(Verdict::Adopted);
        const auto verifiedBefore = VulkanDevice::DispatchProofsVerified();
        Run(*device, guest, shader, 0x3000u, true, false, "a dispatch adopting the proof resolved by its preparation");
        Run(*device, guest, shader, 0x3001u, true, false, "a second dispatch adopting a resolved proof");
        Require(Count(Verdict::Adopted) - adoptedBefore == 2, "dispatch epoch proofs: two proofs resolved by the preparation were not both adopted");
        Require(!verify || VulkanDevice::DispatchProofsVerified() - verifiedBefore == 2, "dispatch epoch proofs: APS5_VERIFY_PARALLEL_DISPATCH_REVALIDATE checked fewer proofs than were adopted");
        const auto stampsBefore = Count(Verdict::Stamps);
        Run(*device, guest, shader, 0x4000u, true, true, "a dispatch whose proof outlived its collect epoch");
        Require(Count(Verdict::Stamps) - stampsBefore == 1 && Count(Verdict::Adopted) - adoptedBefore == 2, "dispatch epoch proofs: a proof whose collect epoch ended before the dispatch was adopted");
        const auto program = reinterpret_cast<std::uintptr_t>(Code.data());
        auto early = device->PrepareDispatch(shader, {}, Lanes, program);
        AgcDriver::GuestMemory::BumpCollectEpoch();
        Run(*device, guest, shader, 0x5000u, false, false, "a serial dispatch proving the template again");
        const auto basisBefore = Count(Verdict::Basis);
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            device->Dispatch(shader, 1, 1, 1, {}, program, std::move(early));
        }
        device->WaitIdle();
        Require(std::equal(guest.Input(), guest.Input() + Lanes, guest.Output()), "dispatch epoch proofs: a dispatch whose proof predates the template's last proof copied other words");
        Require(Count(Verdict::Basis) - basisBefore == 1 && Count(Verdict::Adopted) - adoptedBefore == 2, "dispatch epoch proofs: a proof made before the template was proved again was adopted");
        std::printf("dispatch epoch proofs passed%s\n", verify ? " (verified)" : "");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "dispatch epoch proofs test failed: %s\n", error.what());
        return 1;
    }
}

#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 32;
constexpr std::uint64_t MiB = 1ull << 20u;
constexpr std::uint64_t Window = 2 * MiB;
constexpr std::uint64_t Cells = 3;
constexpr std::uint64_t BlockBytes = Window * Cells;
constexpr std::uint64_t GuestAlignment = 64 * 1024;
constexpr std::uint64_t ResultOffset = GuestAlignment;
constexpr std::uint32_t SourceBytes = Lanes * 4;

alignas(256) constexpr std::array<std::uint32_t, 7> Code{0x34020082, 0xe0301000, 0x80000201, 0xbf8c0000, 0xe0701000, 0x80010201, 0xbf810000};

std::string Hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(GuestAlignment, BlockBytes));
#endif
        Require(block != nullptr, "host import held window: cannot allocate the guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
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

    std::uint8_t* Data() const { return block; }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

class RangeWindow {
public:
    RangeWindow() { Graphics::SetHostImportRangeWindow(Window); }
    ~RangeWindow() { Graphics::SetHostImportRangeWindow(64 * MiB); }
    RangeWindow(const RangeWindow&) = delete;
    RangeWindow& operator=(const RangeWindow&) = delete;
};

std::array<std::uint32_t, 4> BufferDescriptor(std::uint64_t address, std::uint32_t bytes) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

ShaderRecompiler::RecompileRequest Request(const std::vector<std::uint32_t>& userData, const ShaderRecompiler::SpirvTarget& target, std::span<const ShaderRecompiler::MemoryRegion> memory) {
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return request;
}

bool CopyAcrossEdge(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::uint64_t sourceOffset, std::uint32_t seed) {
    auto* source = guest.Data() + sourceOffset;
    auto* result = guest.Data() + ResultOffset;
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        const auto value = seed * 0x9e3779b9u + lane;
        std::memcpy(source + lane * 4u, &value, sizeof(value));
    }
    std::memset(result, 0xcd, SourceBytes);
    std::vector<std::uint32_t> userData;
    for (const auto& descriptor : {BufferDescriptor(guest.Address() + sourceOffset, SourceBytes), BufferDescriptor(guest.Address() + ResultOffset, SourceBytes)}) {
        userData.insert(userData.end(), descriptor.begin(), descriptor.end());
    }
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const auto compiled = ShaderRecompiler::Recompile(Request(userData, device.Target(), memory));
    device.Dispatch(compiled, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    device.WaitIdle();
    const auto run = "host import held window: source at cell offset " + Hex(sourceOffset % Window);
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        std::uint32_t actual = 0;
        std::memcpy(&actual, result + lane * 4u, sizeof(actual));
        const auto expected = seed * 0x9e3779b9u + lane;
        Require(actual == expected, run + ": lane " + std::to_string(lane) + " is " + Hex(actual) + ", expected " + Hex(expected));
    }
    Graphics::Context probe{};
    probe.device = device.Device();
    probe.hostImportAlignment = 1;
    if (!Graphics::HostImportCovers(probe, guest.Address() + ResultOffset, SourceBytes)) return false;
    Require(Graphics::HostImportCovers(probe, guest.Address() + sourceOffset, SourceBytes), run + ": the source across the cell edge has no import while the result's window in the same dispatch has one");
    return true;
}

}

int main() {
    try {
        RangeWindow window;
        GuestBlock guest;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::uint32_t seed = 1;
        for (const auto offset : {Window - 64, Window - 128, Window - 256, 2 * Window - 64, 2 * Window - 192}) {
            if (!CopyAcrossEdge(*device, guest, offset, seed++)) {
                std::printf("skipped, the device does not import guest memory\n");
                return VulkanTestSkipped;
            }
        }
        std::puts("host import held window tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

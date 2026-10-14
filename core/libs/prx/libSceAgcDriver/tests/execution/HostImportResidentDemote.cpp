#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
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
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 32;
constexpr std::uint32_t Bytes = Lanes * 4;
constexpr std::uint64_t MiB = std::uint64_t{1} << 20u;
constexpr std::uint64_t Cell = 2 * MiB;
constexpr std::uint64_t BlockBytes = 2 * Cell;

alignas(256) constexpr std::array<std::uint32_t, 7> CopyCode{0x34020082, 0xe0301000, 0x80000201, 0xbf8c0000, 0xe0701000, 0x80010201, 0xbf810000};
alignas(256) std::array<std::uint32_t, Lanes> Source{};
alignas(256) std::array<std::uint32_t, Lanes> Sink{};

std::string Hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

#ifdef _WIN32
class DirectBlock {
public:
    DirectBlock() {
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        Require(block != nullptr, "resident demote: cannot reserve the guest block");
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, static_cast<DWORD>(BlockBytes), nullptr);
        Require(section != nullptr, "resident demote: cannot create the direct memory");
        GuestArena::GuestArenaMapDirect_nid_postfix(block, Cell, section, 0, PAGE_READWRITE);
        GuestArena::GuestArenaMap_nid_postfix(block + Cell, Cell, section, Cell, PAGE_READWRITE);
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~DirectBlock() {
        GuestAllocations::Mutation().Remove(block);
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
        CloseHandle(section);
    }

    DirectBlock(const DirectBlock&) = delete;
    DirectBlock& operator=(const DirectBlock&) = delete;

    std::uint8_t* Data() const { return block; }
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(block); }

private:
    std::uint8_t* block = nullptr;
    HANDLE section = nullptr;
};

class RangeWindow {
public:
    RangeWindow() { Graphics::SetHostImportRangeWindow(Cell); }
    ~RangeWindow() { Graphics::SetHostImportRangeWindow(64 * MiB); }
    RangeWindow(const RangeWindow&) = delete;
    RangeWindow& operator=(const RangeWindow&) = delete;
};
#endif

std::array<std::uint32_t, 4> BufferDescriptor(std::uint64_t address, std::uint32_t bytes) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void Copy(AgcDriver::VulkanDevice& device, std::uint64_t from, std::uint64_t to) {
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    std::vector<std::uint32_t> userData;
    for (const auto& descriptor : {BufferDescriptor(from, Bytes), BufferDescriptor(to, Bytes)}) userData.insert(userData.end(), descriptor.begin(), descriptor.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(CopyCode.data()), std::as_bytes(std::span(CopyCode))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(CopyCode.data()), CopyCode, 0, {}}, {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}};
    request.useCache = false;
    device.Dispatch(ShaderRecompiler::Recompile(request), 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(CopyCode.data()));
}

}

int main() {
#ifdef _WIN32
    try {
        RangeWindow window;
        DirectBlock guest;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto base = guest.Address();
        Graphics::Context probe{};
        probe.device = device->Device();
        probe.hostImportAlignment = 1;
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) Source[lane] = 0xa5000000u + lane;
        const auto written = base + 0x10000;
        Copy(*device, reinterpret_cast<std::uintptr_t>(Source.data()), written);
        if (!Graphics::HostImportCovers(probe, written, Bytes)) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        Copy(*device, base + Cell - 64, reinterpret_cast<std::uintptr_t>(Sink.data()));
        Require(Graphics::HostImportCovers(probe, base + Cell - 64, Bytes), "resident demote: the region across the edge of the direct memory was not imported");
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            device->WaitIdle();
        }
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            std::uint32_t actual = 0;
            std::memcpy(&actual, guest.Data() + 0x10000 + lane * 4u, sizeof(actual));
            Require(actual == Source[lane], "resident demote: lane " + std::to_string(lane) + " of a dispatch's in-place write into resident direct memory reads " + Hex(actual) + ", expected " + Hex(Source[lane]) + " (an import over the resident pages and a shared view demoted them before the recorded write landed)");
        }
        std::puts("host import resident demote tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#else
    std::printf("skipped, resident direct memory is Windows-only\n");
    return VulkanTestSkipped;
#endif
}

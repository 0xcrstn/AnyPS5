#include "prx/libSceAgcDriver/Execution/include/DispatchFreeze.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "Recompiler.hpp"
#include "execution/VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint64_t WindowAddress = 0x500000000000ull;
constexpr std::size_t WindowBytes = 64u * 1024u;
constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Results = 8;
constexpr std::uint32_t TableDwords = 256;
constexpr std::uint32_t Untouched = 0xdeadbeefu;

alignas(256) constexpr std::array<std::uint32_t, 41> AddtidCode{
    0x34020085, 0x34040082, 0xbe880300, 0x8709ff01, 0x0000ffff, 0x800aff08, 0x00000200, 0x820b8009,
    0x7e080280, 0xd7650004, 0x000208c1, 0xd7660004, 0x000208c1, 0x7e0a02ff, 0xabcdabcd, 0x7e0c02ff,
    0xabcdabcd, 0x7e0e02ff, 0xabcdabcd, 0xe0301000, 0x80000802, 0xdc588040, 0x05080000, 0xdc588fe4,
    0x060a0000, 0xbefe03ff, 0x55555555, 0xdc588000, 0x07080000, 0xbefe03c1, 0xe0701000, 0x80010401,
    0xe0701004, 0x80010501, 0xe0701008, 0x80010601, 0xe070100c, 0x80010701, 0xe0701010, 0x80010801,
    0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(std::uint64_t address, std::uint32_t bytes) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

}

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "usage: agc_dispatch_freeze_tests <capture file>\n";
            return 2;
        }
        const std::filesystem::path capturePath(argv[1]);
        std::filesystem::create_directories(capturePath.parent_path());
        std::filesystem::remove(capturePath);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
#ifdef _WIN32
        auto* window = static_cast<std::uint32_t*>(VirtualAlloc(reinterpret_cast<void*>(WindowAddress), WindowBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        auto* window = static_cast<std::uint32_t*>(mmap(reinterpret_cast<void*>(WindowAddress), WindowBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
#endif
        Require(window == reinterpret_cast<std::uint32_t*>(WindowAddress), "dispatch freeze: the guest window is not free");
        GuestAllocations::Mutation().Add(window, WindowBytes, true, true, true);
        auto* table = window;
        auto* output = window + TableDwords;
        for (std::uint32_t index = 0; index < TableDwords; ++index) table[index] = 0x51000000u + index * 0x00010203u;
        std::fill(output, output + Threads * Results, Untouched);
        std::vector<std::uint32_t> userData(8, 0u);
        const auto tableDescriptor = BufferDescriptor(WindowAddress, TableDwords * 4u);
        const auto outputDescriptor = BufferDescriptor(WindowAddress + TableDwords * 4u, Threads * Results * 4u);
        std::copy(tableDescriptor.begin(), tableDescriptor.end(), userData.begin());
        std::copy(outputDescriptor.begin(), outputDescriptor.end(), userData.begin() + 4);
        const std::span<const std::uint32_t> code(AddtidCode);
        const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
        const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
        ShaderRecompiler::RecompileRequest request{
            {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
            {32u, 0, userData, compute, std::nullopt, std::nullopt, memory},
            device->ComputeTarget(32u),
            {0, 0, 0, 128}
        };
        request.useCache = false;
        const auto result = ShaderRecompiler::Recompile(request);
        AgcDriver::DispatchFreeze::Arm(capturePath, ShaderRecompiler::RequestSerializer{}.Serialize(request));
        device->Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        Require(!AgcDriver::DispatchFreeze::Armed(), "dispatch freeze: the capture stayed armed after the dispatch");
        Require(std::filesystem::exists(capturePath), "dispatch freeze: no capture was written");
        for (std::uint32_t tid = 0; tid < Threads; ++tid) Require(output[tid * Results + 4u] == table[tid], "dispatch freeze: the frozen dispatch computed a wrong buffer load");
        const auto capture = AgcDriver::DispatchFreeze::Read(capturePath);
        Require(capture.groups == std::array<std::uint32_t, 3>{1u, 1u, 1u} && capture.arguments == 0, "dispatch freeze: the capture did not keep the launch");
        const auto covering = std::find_if(capture.ranges.begin(), capture.ranges.end(), [](const auto& range) { return range.begin <= WindowAddress + TableDwords * 4u && range.end >= WindowAddress + (TableDwords + Threads * Results) * 4u && range.writable; });
        Require(covering != capture.ranges.end(), "dispatch freeze: the written output is not a writable captured range");
        const auto offset = static_cast<std::size_t>(WindowAddress + TableDwords * 4u - covering->begin);
        std::uint32_t before = 0;
        std::uint32_t after = 0;
        std::memcpy(&before, covering->before.data() + offset + 16u, 4u);
        std::memcpy(&after, covering->after.data() + offset + 16u, 4u);
        Require(before == Untouched && after == table[0], "dispatch freeze: the capture did not keep the bytes before and after the dispatch");
        std::printf("dispatch freeze capture written: %zu ranges\n", capture.ranges.size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

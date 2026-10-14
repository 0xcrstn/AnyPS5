#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestHeap.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t CounterOffset = 0x10;

alignas(256) std::array<std::uint32_t, 9> AppendCode{0xbefc03ffu, 0x0000ffffu, 0xd8fa0000u | CounterOffset, 0x04000000u, 0xbf8cc07fu, 0x7e060280u, 0xe0702000u, 0x80000403u, 0xbf810000u};

alignas(256) std::array<std::uint32_t, 4> WarmUp{};
alignas(256) std::array<std::uint32_t, 4> First{};
alignas(256) std::array<std::uint32_t, 4> Second{};
alignas(16) std::array<std::uint32_t, 4> Start{};
alignas(16) std::array<std::uint32_t, 4> Restart{};
alignas(16) std::array<std::uint32_t, 4> ReadBack{};
alignas(256) volatile std::uint32_t Done = 0;
std::uint32_t* GpuReadBack = nullptr;

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void Append(std::vector<std::uint32_t>& words, const void* output) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(AppendCode.data()));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, 4u << 1u);
    const std::array<std::uint32_t, 4> descriptor{Low(output), High(output) & 0xffffu, 16u, 0x31016facu};
    std::uint32_t offset = 0x240;
    for (const auto value : descriptor) SetShaderRegister(words, offset++, value);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

void CopyToGds(std::vector<std::uint32_t>& words, const void* source) {
    words.insert(words.end(), {0xc0055000u, 0x60100000u, Low(source), High(source), CounterOffset, 0u, 4u});
}

void CopyFromGds(std::vector<std::uint32_t>& words, const void* destination) {
    words.insert(words.end(), {0xc0055000u, 0x20000000u, CounterOffset, 0u, Low(destination), High(destination), 4u});
}

void Finish(std::vector<std::uint32_t>& words, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(&Done), High(&Done), value});
}

void Submit(std::uint32_t queue, std::vector<std::uint32_t>& words, std::uint32_t done) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require((queue == 0 ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet)) == 0, "submit failed");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Done != done) {
        Require(std::chrono::steady_clock::now() < deadline, "the submission never finished");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    AgcDriverWaitIdle_nid_postfix();
}

void Run(std::uint32_t queue, std::uint32_t round) {
    const std::string where = "queue " + std::to_string(queue) + ": ";
    const auto base = 1000u * (round * 4u + 1u);
    Start[0] = base;
    Restart[0] = base + 500u;
    First.fill(0xdeadbeefu);
    Second.fill(0xdeadbeefu);
    ReadBack.fill(0xdeadbeefu);
    GpuReadBack[0] = 0xdeadbeefu;
    GpuReadBack[1] = 0xdeadbeefu;
    std::vector<std::uint32_t> words;
    CopyToGds(words, Start.data());
    Append(words, First.data());
    CopyToGds(words, Restart.data());
    Append(words, Second.data());
    CopyFromGds(words, GpuReadBack);
    CopyToGds(words, Start.data());
    CopyFromGds(words, GpuReadBack + 1);
    CopyFromGds(words, ReadBack.data());
    Finish(words, round * 2u + 2u);
    Submit(queue, words, round * 2u + 2u);
    Require(First[0] == base, where + "a dispatch appended to " + std::to_string(First[0]) + ", not the GDS counter a DMA_DATA set before it (" + std::to_string(base) + ")");
    Require(Second[0] == base + 500u, where + "a dispatch appended to " + std::to_string(Second[0]) + ", not the GDS counter a DMA_DATA set after the previous dispatch (" + std::to_string(base + 500u) + ")");
    Require(ReadBack[0] == base, where + "a DMA_DATA read " + std::to_string(ReadBack[0]) + " from the GDS, not the counter a DMA_DATA set before it (" + std::to_string(base) + ")");
    Require(GpuReadBack[0] == base + 501u, where + "a DMA_DATA into GPU memory read " + std::to_string(GpuReadBack[0]) + " from the GDS, not the counter the dispatch before it left (" + std::to_string(base + 501u) + ")");
    Require(GpuReadBack[1] == base, where + "a DMA_DATA into GPU memory read " + std::to_string(GpuReadBack[1]) + " from the GDS, not the counter a DMA_DATA set before it (" + std::to_string(base) + ")");
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        GpuReadBack = static_cast<std::uint32_t*>(GuestHeap::GuestHeapAlignForGpu_nid_postfix(65536, 65536));
        Require(GpuReadBack != nullptr, "cannot allocate GPU memory for the read-back");
        std::vector<std::uint32_t> warmUp;
        Start[0] = 7;
        CopyToGds(warmUp, Start.data());
        Append(warmUp, WarmUp.data());
        CopyFromGds(warmUp, ReadBack.data());
        Finish(warmUp, 1);
        Submit(0x20, warmUp, 1);
        Require(WarmUp[0] == 7 && ReadBack[0] == 8, "the warm-up dispatch did not append to the GDS counter (" + std::to_string(WarmUp[0]) + ", read back " + std::to_string(ReadBack[0]) + ")");
        std::uint32_t round = 0;
        for (const std::uint32_t queue : {0x20u, 0u, 0x20u, 0u}) Run(queue, round++);
        GuestHeap::GuestHeapFree_nid_postfix(GpuReadBack);
        std::cout << "gds dma dispatch order tests passed\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}

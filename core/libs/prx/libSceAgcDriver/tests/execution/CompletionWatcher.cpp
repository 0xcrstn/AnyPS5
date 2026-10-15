#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" int APS5_VABI sceKernelCreateEqueue(KernelEqueue* eq, const char* name);
extern "C" int APS5_VABI sceKernelDeleteEqueue(KernelEqueue eq);

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::uint32_t, 15> SpinCode{0xf4080200u, 0xfa000000u, 0xbe840381u, 0x9304ff04u, 0x0019660du, 0x8004ff04u, 0x3c6ef35fu, 0x80828102u, 0xbf078002u, 0xbf85fff9u, 0xbf8cc07fu, 0x7e000204u, 0xe0700000u, 0x80020000u, 0xbf810000u};

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Rounds = 8;

struct Slot {
    alignas(256) std::array<std::uint32_t, 4> output{};
    alignas(256) std::array<std::uint32_t, 4> descriptor{};
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "completion watcher: cannot allocate the guest block");
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

    Slot& Data() { return *reinterpret_cast<Slot*>(block); }

private:
    std::uint8_t* block = nullptr;
};

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void Spin(std::vector<std::uint32_t>& words, Slot& slot, std::uint32_t iterations) {
    slot.descriptor = {Low(slot.output.data()), High(slot.output.data()) & 0xffffu, sizeof(slot.output), 0x31016facu};
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(SpinCode.data()));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, 3u << 1u);
    SetShaderRegister(words, 0x240, Low(slot.descriptor.data()));
    SetShaderRegister(words, 0x241, High(slot.descriptor.data()));
    SetShaderRegister(words, 0x242, iterations);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

void ReleaseInterrupt(std::vector<std::uint32_t>& words) {
    words.insert(words.end(), {0xc0064900u, 0u, 1u << 24u, 0u, 0u, 0u, 0u, 0u});
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        GuestBlock block;
        auto& slot = block.Data();
        KernelEqueue eq = 0;
        Require(sceKernelCreateEqueue(&eq, "AGC completion watcher") == 0, "completion watcher: event queue creation failed");
        auto owner = EqueuePin_nid_postfix(eq);
        int tag = 0;
        Require(sceAgcDriverAddEqEvent(eq, 0, &tag) == 0, "completion watcher: event registration failed");
        for (std::uint32_t round = 1; round <= Rounds; ++round) {
            std::vector<std::uint32_t> words;
            Spin(words, slot, 1u << 16u);
            ReleaseInterrupt(words);
            Packet graphics{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
            Require(sceAgcDriverSubmitDcb(&graphics) == 0, "completion watcher: submit failed");
            const auto start = std::chrono::steady_clock::now();
            std::array<KernelEvent, 2> events{};
            while (owner->GetTriggeredEvents(events.data(), 2) == 0) {
                Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), "completion watcher: an end-of-pipe interrupt behind GPU work was not delivered while the CPU waited without a driver call (round " + std::to_string(round) + ")");
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            Require(events[0].udata == &tag, "completion watcher: the interrupt reached another registration");
        }
        Require(sceAgcDriverDeleteEqEvent(eq, 0) == 0, "completion watcher: event deletion failed");
        owner.reset();
        Require(sceKernelDeleteEqueue(eq) == 0, "completion watcher: event queue deletion failed");
        std::cout << "completion watcher: " << Rounds << " end-of-pipe interrupts behind GPU work delivered without a driver call\n";
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

#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
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
#include <string_view>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::uint32_t, 15> SpinCode{0xf4080200u, 0xfa000000u, 0xbe840381u, 0x9304ff04u, 0x0019660du, 0x8004ff04u, 0x3c6ef35fu, 0x80828102u, 0xbf078002u, 0xbf85fff9u, 0xbf8cc07fu, 0x7e000204u, 0xe0700000u, 0x80020000u, 0xbf810000u};

constexpr std::uint32_t ProducerSpin = 1u << 20u;
constexpr std::uint32_t ConsumerSpin = 1u << 8u;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Rounds = 12;

struct Slot {
    alignas(256) std::array<std::uint32_t, 4> producerOutput{};
    alignas(256) std::array<std::uint32_t, 4> producerDescriptor{};
    alignas(256) std::array<std::uint32_t, 4> consumerOutput{};
    alignas(256) std::array<std::uint32_t, 4> consumerDescriptor{};
    alignas(256) volatile std::uint32_t label = 0;
    alignas(256) volatile std::uint32_t done = 0;
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "same-family order: cannot allocate the guest block");
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

void Spin(std::vector<std::uint32_t>& words, std::array<std::uint32_t, 4>& descriptor, std::array<std::uint32_t, 4>& output, std::uint32_t iterations) {
    descriptor = {Low(output.data()), High(output.data()) & 0xffffu, sizeof(output), 0x31016facu};
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(SpinCode.data()));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, 3u << 1u);
    SetShaderRegister(words, 0x240, Low(descriptor.data()));
    SetShaderRegister(words, 0x241, High(descriptor.data()));
    SetShaderRegister(words, 0x242, iterations);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

void ReleaseLabel(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), Low(target), High(target), value, 0u, 0u});
}

void WriteData(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(target), High(target), value});
}

void WaitEqual(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(target), High(target), value, 0xffffffffu, 0x19u});
}

std::string Round(Slot& slot, std::uint32_t value) {
    slot.done = 0;
    std::vector<std::uint32_t> consumer;
    WaitEqual(consumer, &slot.label, value);
    Spin(consumer, slot.consumerDescriptor, slot.consumerOutput, ConsumerSpin);
    WriteData(consumer, &slot.done, value);
    std::vector<std::uint32_t> producer;
    Spin(producer, slot.producerDescriptor, slot.producerOutput, ProducerSpin);
    ReleaseLabel(producer, &slot.label, value);
    Packet graphics{consumer.data(), static_cast<std::uint32_t>(consumer.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&graphics) == 0, "graphics submit failed");
    Packet compute{producer.data(), static_cast<std::uint32_t>(producer.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x20, &compute) == 0, "compute submit failed");
    const auto start = std::chrono::steady_clock::now();
    while (slot.done != value) {
        try {
            AgcDriverWaitIdle_nid_postfix();
        } catch (const std::exception& error) {
            return error.what();
        }
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(20), "the consumer never finished after the compute label");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(slot.label == value, "the compute label did not reach memory");
    return {};
}

}

int main(int argc, char** argv) {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        const bool missing = argc == 2 && std::string_view(argv[1]) == "--missing-order";
        AgcDriver::Graphics::Recorder::SuppressOrderWaitsForTest(missing);
        GuestBlock block;
        auto& slot = block.Data();
        std::string failure;
        for (std::uint32_t round = 1; round <= Rounds && failure.empty(); ++round) failure = Round(slot, round);
        if (missing) {
            Require(failure.find("APS5_QUEUE_TOPOLOGY_VERIFY") != std::string::npos, "a graphics batch that skipped its compute dependency did not fail the verify mode (" + failure + ")");
            std::cout << "same-family missing order failed as required\n";
            try {
                AgcDriverShutdown_nid_postfix();
            } catch (const std::exception&) {
            }
            return 0;
        }
        Require(failure.empty(), "same-family order: " + failure);
        std::cout << "same-family order: " << Rounds << " graphics waits on compute labels completed\n";
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

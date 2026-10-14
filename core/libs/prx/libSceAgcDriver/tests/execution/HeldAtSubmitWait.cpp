#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Waits = 16;

struct Slot {
    alignas(256) std::array<volatile std::uint32_t, Waits * 64> labels{};
    alignas(256) volatile std::uint32_t done = 0;
};

static_assert(sizeof(Slot) <= BlockBytes);

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "held-at-submit wait: cannot allocate the guest block");
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

volatile std::uint32_t* Label(Slot& slot, std::uint32_t index) {
    return &slot.labels[index * 64];
}

void WriteData(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(target), High(target), value});
}

void WaitEqual(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(target), High(target), value, 0xffffffffu, 0x19u});
}

enum class Mode { NoWaits, StoredBeforeSubmit, StoredAfterSubmit };

std::uint64_t Run(Slot& slot, std::uint32_t value, Mode mode) {
    slot.done = 0;
    for (std::uint32_t index = 0; index < Waits; ++index) *Label(slot, index) = mode == Mode::StoredBeforeSubmit ? value + index : 0;
    std::vector<std::uint32_t> words;
    if (mode != Mode::NoWaits) {
        for (std::uint32_t index = 0; index < Waits; ++index) WaitEqual(words, Label(slot, index), value + index);
    }
    WriteData(words, &slot.done, 1);
    const auto before = AgcDriver::GuestMemory::CollectEpochBumps();
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "compute submit failed");
    if (mode == Mode::StoredAfterSubmit) {
        for (std::uint32_t index = 0; index < Waits; ++index) *Label(slot, index) = value + index;
    }
    const auto start = std::chrono::steady_clock::now();
    while (slot.done == 0) {
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(10), "the submission did not finish");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    AgcDriverWaitIdle_nid_postfix();
    return AgcDriver::GuestMemory::CollectEpochBumps() - before;
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        GuestBlock block;
        auto& slot = block.Data();
        Run(slot, 1, Mode::NoWaits);
        std::uint64_t heldExtra = ~std::uint64_t{0};
        for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
            const auto base = 100 + attempt * 3 * Waits;
            const auto without = Run(slot, base, Mode::NoWaits);
            const auto held = Run(slot, base + Waits, Mode::StoredBeforeSubmit);
            const auto late = Run(slot, base + 2 * Waits, Mode::StoredAfterSubmit);
            std::cout << "attempt " << attempt << ": " << without << " bumps without waits, " << held << " with " << Waits << " waits satisfied before the submit, " << late << " with " << Waits << " waits the CPU satisfied after it\n";
            Require(late >= without + Waits, "a WAIT_REG_MEM the CPU satisfied after the submit started no collect epoch: " + std::to_string(late) + " bumps against " + std::to_string(without) + " without waits");
            heldExtra = std::min(heldExtra, held > without ? held - without : 0);
        }
        Require(heldExtra < Waits / 2, "WAIT_REG_MEMs whose value was in memory when the submission was received started new collect epochs: " + std::to_string(heldExtra) + " extra bumps for " + std::to_string(Waits) + " waits");
        std::cout << "held-at-submit wait test passed\n";
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

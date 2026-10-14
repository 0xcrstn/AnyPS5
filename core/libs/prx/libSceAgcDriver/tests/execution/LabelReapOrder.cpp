#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Labels = 64;
constexpr std::uint64_t ScratchOffset = 4096;
constexpr std::uint64_t ScratchBytes = 4096;
constexpr std::uint64_t CounterOffset = 0;
constexpr std::uint64_t MirrorOffset = 64;
constexpr std::uint64_t GateOffset = 128;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "label reap order: cannot allocate the guest block");
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

    std::uint32_t Word(std::uint64_t offset) const { return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(block + offset)).load(std::memory_order_acquire); }

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

int RecordLabel(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t value, std::uint64_t stamp) {
    return device.WriteLabelOnGpu(address, std::as_bytes(std::span(&value, 1)), stamp, 0, false);
}

bool BoundaryReap(AgcDriver::VulkanDevice& device) {
    if (!device.ReapMayRetire()) return false;
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    device.ReapRecorded();
    return true;
}

void WaitFor(const GuestBlock& block, std::uint64_t offset, std::uint32_t atLeast, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (block.Word(offset) < atLeast) {
        Require(std::chrono::steady_clock::now() < deadline, what);
        std::this_thread::yield();
    }
}

int RunReapGate(AgcDriver::VulkanDevice& device, GuestBlock& block) {
    const std::array<std::uint32_t, 4> pattern{1u, 2u, 3u, 4u};
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.ReapRecorded();
        Require(!device.ReapMayRetire(), "label reap order: an idle recorder reports a batch that may retire");
        Require(device.FillBuffer(block.Address() + ScratchOffset, ScratchBytes, pattern), "label reap order: the fill before the gate label was not recorded");
        Require(RecordLabel(device, block.Address() + GateOffset, 7, 1) == 0, "label reap order: the gate label was not recorded on the GPU");
    }
    std::uint64_t epoch = 0;
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        epoch = device.SubmitAndEpoch();
    }
    Require(epoch != 0, "label reap order: the gate batch has no serial");
    device.WaitRecorded(epoch);
    Require(device.ReapMayRetire(), "label reap order: a completed batch is not reported as one a reap retires");
    Require(block.Word(GateOffset) == 7, "label reap order: the gate label is not visible after its batch completed");
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.ReapRecorded();
        Require(!device.ReapMayRetire(), "label reap order: a reap that retired every batch still reports one that may retire");
    }
    return 0;
}

int RunOrder(AgcDriver::VulkanDevice& device, GuestBlock& block) {
    const std::array<std::uint32_t, 4> pattern{5u, 6u, 7u, 8u};
    std::atomic<bool> stop{false};
    std::atomic<bool> regressed{false};
    std::atomic<std::uint32_t> regressedFrom{0}, regressedTo{0};
    std::atomic<std::uint32_t> observedChanges{0};
    std::thread watcher([&] {
        std::uint32_t last = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const auto mirror = block.Word(MirrorOffset);
            const auto counter = block.Word(CounterOffset);
            if (counter < last || mirror > counter) {
                regressedFrom = last;
                regressedTo = counter < last ? counter : mirror;
                regressed = true;
            }
            if (counter != last) ++observedChanges;
            last = counter;
            if (last == Labels) break;
        }
    });
    std::atomic<bool> waitSatisfied{false};
    std::thread waiter([&] {
        try {
            WaitFor(block, CounterOffset, Labels / 2, "label reap order: a wait for the middle label was never satisfied");
            waitSatisfied = true;
        } catch (...) {
        }
    });
    std::uint64_t stamp = 10;
    std::uint64_t reaps = 0;
    for (std::uint32_t value = 1; value <= Labels; ++value) {
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            Require(device.FillBuffer(block.Address() + ScratchOffset, ScratchBytes, pattern), "label reap order: the fill before a label was not recorded");
            Require(RecordLabel(device, block.Address() + CounterOffset, value, ++stamp) == 0, "label reap order: a counter label was not recorded on the GPU");
            Require(RecordLabel(device, block.Address() + MirrorOffset, value, ++stamp) == 0, "label reap order: a mirror label was not recorded on the GPU");
            if (value % 4 == 0) device.SubmitRecorded(false);
        }
        if (BoundaryReap(device)) ++reaps;
    }
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.SubmitRecorded(false);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (block.Word(CounterOffset) != Labels || block.Word(MirrorOffset) != Labels) {
        Require(std::chrono::steady_clock::now() < deadline, "label reap order: the last labels never became visible");
        if (BoundaryReap(device)) ++reaps;
        std::this_thread::yield();
    }
    stop = true;
    watcher.join();
    waiter.join();
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.WaitIdle();
        device.ReapRecorded();
    }
    const std::string order = "label reap order: a later label's value was overtaken (" + std::to_string(regressedFrom.load()) + " then " + std::to_string(regressedTo.load()) + ")";
    Require(!regressed.load(), order.c_str());
    Require(waitSatisfied.load(), "label reap order: the waiter for the middle label did not finish");
    Require(!device.ReapMayRetire(), "label reap order: a drained recorder still reports a batch that may retire");
    std::printf("label reap order: %u labels in order, %u changes observed, %llu boundary reaps taken\n", Labels, observedChanges.load(), static_cast<unsigned long long>(reaps));
    return 0;
}

int RunHeldTry() {
    auto& mutex = AgcDriver::GuestMemory::GpuMutex();
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        std::lock_guard lock(mutex);
        held = true;
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!held.load(std::memory_order_acquire)) std::this_thread::yield();
    Require(mutex.HeldByOtherThread(), "label reap order: the mutex held by another thread is not seen as held");
    Require(!mutex.try_lock(), "label reap order: a try succeeded while another thread held the mutex");
    release = true;
    holder.join();
    Require(!mutex.HeldByOtherThread(), "label reap order: a released mutex is still seen as held");
    Require(mutex.try_lock(), "label reap order: a try failed on a free mutex");
    Require(!mutex.HeldByOtherThread() && mutex.HeldByThisThread(), "label reap order: the holder sees its own hold as another thread's");
    Require(mutex.try_lock(), "label reap order: a recursive try by the holder failed");
    mutex.unlock();
    mutex.unlock();
    return 0;
}

void ReportCosts(AgcDriver::VulkanDevice& device) {
    constexpr int Rounds = 20000;
    auto& mutex = AgcDriver::GuestMemory::GpuMutex();
    const auto lockedStart = std::chrono::steady_clock::now();
    for (int round = 0; round < Rounds; ++round) {
        std::lock_guard lock(mutex);
        device.ReapRecorded();
    }
    const auto checkStart = std::chrono::steady_clock::now();
    int mayRetire = 0;
    for (int round = 0; round < Rounds; ++round) mayRetire += device.ReapMayRetire() ? 1 : 0;
    const auto checkEnd = std::chrono::steady_clock::now();
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        std::lock_guard lock(mutex);
        held = true;
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!held.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto tryStart = std::chrono::steady_clock::now();
    int acquired = 0;
    for (int round = 0; round < Rounds; ++round) {
        if (mutex.try_lock()) {
            ++acquired;
            mutex.unlock();
        }
    }
    const auto tryEnd = std::chrono::steady_clock::now();
    release = true;
    holder.join();
    const auto ns = [](auto from, auto to) { return std::chrono::duration<double, std::nano>(to - from).count() / Rounds; };
    std::printf("label reap order: empty locked reap %.0f ns, lock-free reap check %.0f ns (%d may retire), try on a held mutex %.0f ns (%d acquired)\n", ns(lockedStart, checkStart), ns(checkStart, checkEnd), mayRetire, ns(tryStart, tryEnd), acquired);
}

}

int main() {
    try {
        RunHeldTry();
        GuestBlock block;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!device->CanWaitUnlocked()) {
            std::printf("skipped, the device has no timeline semaphore\n");
            return VulkanTestSkipped;
        }
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            const std::array<std::uint32_t, 4> pattern{0u, 0u, 0u, 0u};
            if (!device->FillBuffer(block.Address() + ScratchOffset, ScratchBytes, pattern)) {
                std::printf("skipped, the device does not import guest memory\n");
                return VulkanTestSkipped;
            }
            device->WaitIdle();
        }
        RunReapGate(*device, block);
        RunOrder(*device, block);
        ReportCosts(*device);
        std::puts("label reap order tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

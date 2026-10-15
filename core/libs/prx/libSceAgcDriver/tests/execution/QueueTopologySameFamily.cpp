#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint64_t LabelOffset = 0;
constexpr std::uint64_t PayloadOffset = 4096;
constexpr std::uint64_t DestinationOffset = 8192;
constexpr std::size_t PayloadBytes = 4096;
constexpr std::uint32_t ComputeQueue = 0x20;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "same-family topology: cannot allocate the guest block");
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

void ProduceOnCompute(AgcDriver::VulkanDevice& device, GuestBlock& block, std::uint32_t value) {
    const std::array<std::uint32_t, 4> pattern{value, value + 1, value + 2, value + 3};
    AgcDriver::GuestMemory::TagGpuLockThread(ComputeQueue);
    Require(device.FillBuffer(block.Address() + PayloadOffset, PayloadBytes, pattern), "same-family topology: the producer fill was not recorded on the GPU");
    const std::uint32_t label = value;
    const int reason = device.WriteLabelOnGpu(block.Address() + LabelOffset, std::as_bytes(std::span(&label, 1)), value, ComputeQueue, false);
    Require(reason == 0 || reason == 5, "same-family topology: the producer label was not recorded on the GPU (reason " + std::to_string(reason) + ")");
    AgcDriver::GuestMemory::TagGpuLockThread(0);
}

void ConsumeOnGraphics(AgcDriver::VulkanDevice& device, GuestBlock& block) {
    const auto outcome = device.CopyBuffer(block.Address() + DestinationOffset, block.Address() + PayloadOffset, PayloadBytes, 0, std::numeric_limits<std::size_t>::max(), 0, 0, 0, [](std::span<const std::byte>, std::uint64_t) {});
    Require(outcome.path == 1 || outcome.path == 3, "same-family topology: the consumer copy did not run on the GPU (path " + std::to_string(outcome.path) + ")");
}

int RunOrder(AgcDriver::VulkanDevice& device) {
    auto& graphics = device.RecorderFor(0);
    auto& compute = device.RecorderFor(ComputeQueue);
    Require(&graphics != &compute, "same-family topology: guest compute queues record on the graphics recorder");
    GuestBlock block;
    for (std::uint32_t round = 1; round <= 16; ++round) {
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            ProduceOnCompute(device, block, round * 16);
            device.OrderAfter(0, ComputeQueue);
            ConsumeOnGraphics(device, block);
            device.SubmitRecorded(false);
        }
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            device.WaitIdle();
            device.ReapRecorded();
        }
        Require(block.Word(LabelOffset) == round * 16, "same-family topology: the producer label never landed");
        for (std::uint64_t offset = 0; offset < PayloadBytes; offset += 4) {
            const auto expected = round * 16 + static_cast<std::uint32_t>((offset / 4) % 4);
            const auto copied = block.Word(DestinationOffset + offset);
            Require(copied == expected, "same-family topology: the graphics copy read payload " + std::to_string(copied) + " instead of " + std::to_string(expected) + " at byte " + std::to_string(offset) + " in round " + std::to_string(round));
        }
    }
    Require(compute.Submissions() != 0 && graphics.Submissions() != 0, "same-family topology: one of the two queues never submitted");
    const auto order = graphics.QueueOrderCounts();
    Require(order.requiredWaits != 0 && order.waitedSubmits != 0, "same-family topology: the graphics queue never waited for the compute queue");
    std::printf("same-family topology: 16 rounds ordered, graphics %llu submits (%llu waited), compute %llu submits\n", static_cast<unsigned long long>(graphics.Submissions()), static_cast<unsigned long long>(order.waitedSubmits), static_cast<unsigned long long>(compute.Submissions()));
    return 0;
}

int RunVerify(AgcDriver::VulkanDevice& device) {
    Require(AgcDriver::Graphics::Recorder::QueueOrderVerify(), "same-family verify: run with APS5_QUEUE_TOPOLOGY_VERIFY=1");
    GuestBlock block;
    std::string failure;
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        ProduceOnCompute(device, block, 7);
        device.OrderAfter(0, ComputeQueue);
        device.RecorderFor(0).DropOrderWaitsForTest();
        ConsumeOnGraphics(device, block);
        try {
            device.RecorderFor(0).Submit();
        } catch (const std::runtime_error& error) {
            failure = error.what();
        }
        device.WaitIdle();
        device.ReapRecorded();
    }
    Require(failure.find("APS5_QUEUE_TOPOLOGY_VERIFY") != std::string::npos, "same-family verify: a graphics batch submitted without its compute dependency did not fail the verify mode (" + failure + ")");
    std::puts("same-family verify: the dropped dependency failed as required");
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc == 2 && std::string_view(argv[1]) == "--verify") return RunVerify(*device);
        return RunOrder(*device);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

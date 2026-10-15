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
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::uint64_t LabelOffset = 0;
constexpr std::uint64_t PayloadOffset = 256;
constexpr std::uint64_t FillOffset = 4096;
constexpr std::uint32_t OldPayload = 0x11111111u;
constexpr std::uint32_t NewPayload = 0x22222222u;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "label publication order: cannot allocate the guest block");
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
    void SetWord(std::uint64_t offset, std::uint32_t value) { std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(block + offset)).store(value, std::memory_order_release); }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

int RecordReleaseAfterWriteBack(AgcDriver::VulkanDevice& device, GuestBlock& block, std::uint64_t& epoch) {
    const std::array<std::uint32_t, 4> pattern{1u, 2u, 3u, 4u};
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    device.ReapRecorded();
    Require(device.FillBuffer(block.Address() + FillOffset, 4096, pattern), "label publication order: the fill before the release was not recorded");
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    Require(recorder != nullptr, "label publication order: no active recorder");
    const auto payload = block.Address() + PayloadOffset;
    recorder->OnComplete([payload] {
        const std::uint32_t value = NewPayload;
        AgcDriver::GuestMemory::StoreOwnBytes(payload, std::as_bytes(std::span(&value, 1)));
    });
    const std::uint32_t label = 1;
    const int reason = device.WriteLabelOnGpu(block.Address() + LabelOffset, std::as_bytes(std::span(&label, 1)), 1, 0, false);
    epoch = device.SubmitAndEpoch();
    return reason;
}

int RunOrder(AgcDriver::VulkanDevice& device) {
    GuestBlock block;
    block.SetWord(PayloadOffset, OldPayload);
    std::uint64_t epoch = 0;
    const int reason = RecordReleaseAfterWriteBack(device, block, epoch);
    if (reason == 3) {
        device.WaitIdle();
        std::puts("skipped, the device imports no host memory");
        return VulkanTestSkipped;
    }
    Require(epoch != 0, "label publication order: the release batch has no serial");
    device.WaitRecorded(epoch);
    const auto labelBeforeReap = block.Word(LabelOffset);
    const auto payloadBeforeReap = block.Word(PayloadOffset);
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.ReapRecorded();
    }
    const auto labelAfterReap = block.Word(LabelOffset);
    const auto payloadAfterReap = block.Word(PayloadOffset);
    Require(labelAfterReap == 1 && payloadAfterReap == NewPayload, "label publication order: the release or its payload never landed (label " + std::to_string(labelAfterReap) + ", payload " + std::to_string(payloadAfterReap) + ")");
    Require(labelBeforeReap == 0 || payloadBeforeReap == NewPayload, "label publication order: the label was visible to a native reader before the write-back of its payload (reason " + std::to_string(reason) + ")");
    std::printf("label publication order: reason %d, before the reap label %u payload 0x%x\n", reason, labelBeforeReap, payloadBeforeReap);
    return 0;
}

int RunVerify(AgcDriver::VulkanDevice& device) {
    Require(AgcDriver::Graphics::Recorder::LabelPublicationVerify() && !AgcDriver::Graphics::Recorder::LabelPublicationOrdered(), "label publication verify: run with APS5_LABEL_PUBLICATION_VERIFY=1 and early publication");
    GuestBlock block;
    std::uint64_t epoch = 0;
    std::string failure;
    try {
        RecordReleaseAfterWriteBack(device, block, epoch);
    } catch (const std::runtime_error& error) {
        failure = error.what();
    }
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    device.WaitIdle();
    device.ReapRecorded();
    Require(failure.find("APS5_LABEL_PUBLICATION_VERIFY") != std::string::npos, "label publication verify: a label stored on the GPU ahead of an owed write-back did not fail the verify mode (" + failure + ")");
    std::puts("label publication verify: the injected inversion failed as required");
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

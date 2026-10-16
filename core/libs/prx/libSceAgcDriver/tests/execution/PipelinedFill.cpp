#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferFill.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using namespace AgcDriver::DriverDetail;
alignas(256) constexpr auto Code = PatternFillKernel;
alignas(256) std::array<std::uint32_t, 4> Pattern{};
alignas(256) std::array<std::uint32_t, 4> Control{};
alignas(256) std::array<std::uint32_t, 260> Output{};
alignas(256) std::array<std::uint32_t, 64> Labels{};
alignas(256) std::array<std::uint32_t, 4> ScratchPattern{0x0f0f0f0fu, 0, 0, 0};
alignas(256) std::array<std::uint32_t, 4> ScratchControl{256, 0, 0, 0};
alignas(256) std::array<std::uint32_t, 260> Scratch{};

std::array<std::uint32_t, 12> UserData(const void* pattern, const void* control, void* output) {
    std::array<std::uint32_t, 12> data{};
    const auto descriptor = [&](std::size_t first, const void* pointer, std::uint32_t stride, std::uint32_t count, std::uint32_t format) {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        data[first] = static_cast<std::uint32_t>(address);
        data[first + 1] = static_cast<std::uint32_t>(address >> 32u) | (stride << 16u);
        data[first + 2] = count;
        data[first + 3] = format;
    };
    descriptor(0, pattern, 4, 4, 0x14004u);
    descriptor(4, output, 4, 256, 0x14004u);
    descriptor(8, control, 16, 1, 0x4dfacu);
    return data;
}

void AppendFill(std::vector<std::uint32_t>& words, const void* pattern, const void* control, void* output) {
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{64, 1, 1};
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(Code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{(12u << 1u) | (1u << 7u)};
    registers(0x213, resource);
    registers(0x240, UserData(pattern, control, output));
    words.insert(words.end(), {0xc0031500u, 4, 1, 1, 0x41u | 0x8000u});
}

void AppendScratchFill(std::vector<std::uint32_t>& words) {
    AppendFill(words, ScratchPattern.data(), ScratchControl.data(), Scratch.data());
}

void AppendOutputFill(std::vector<std::uint32_t>& words) {
    AppendFill(words, Pattern.data(), Control.data(), Output.data());
}

void AppendLabel(std::vector<std::uint32_t>& words, std::uint32_t* target, std::uint32_t value) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    words.insert(words.end(), {0xc0064900u, 0x514u, 1u << 29u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value, 0, 0});
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
}

void Finish() {
    auto idle = std::async(std::launch::async, [] { AgcDriverWaitIdle_nid_postfix(); });
    if (idle.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        std::fputs("queued fill: the queue worker and the record jobs did not finish\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    }
    idle.get();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output.data()), sizeof(Output));
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Scratch.data()), sizeof(Scratch));
}

void InputsAfterQueuedLabels() {
    Output.fill(0xcafebabeu);
    Pattern = {0x40404040u, 0, 0, 0};
    Control = {256, 0, 0, 0};
    std::vector<std::uint32_t> words;
    AppendScratchFill(words);
    AppendLabel(words, &Control[0], 128);
    AppendLabel(words, &Pattern[0], 0xababababu);
    AppendOutputFill(words);
    const auto before = LabelRecordJobTotals();
    Submit(words);
    Finish();
    Require(LabelRecordJobTotals().jobs > before.jobs, "fill input labels did not use candidate record jobs");
    for (std::size_t i = 0; i < Output.size(); ++i) {
        const auto expected = i < 128 ? 0xababababu : 0xcafebabeu;
        Require(Output[i] == expected, "a fill read its control or pattern before the queued labels that write them, at " + std::to_string(i));
    }
}

void FillAfterManyQueuedLabels() {
    Output.fill(0xcafebabeu);
    Pattern = {0x5a5a5a5au, 0, 0, 0};
    Control = {256, 0, 0, 0};
    Labels.fill(0);
    const auto labelCount = Labels.size();
    std::vector<std::uint32_t> words;
    AppendScratchFill(words);
    for (std::size_t i = 0; i < labelCount; ++i) AppendLabel(words, &Labels[i], static_cast<std::uint32_t>(i + 1));
    AppendOutputFill(words);
    const auto before = LabelRecordJobTotals();
    Submit(words);
    Finish();
    Require(LabelRecordJobTotals().jobs > before.jobs, "many fill labels did not use candidate record jobs");
    for (std::size_t i = 0; i < labelCount; ++i) Require(Labels[i] == i + 1, "a queued label was not stored");
    for (std::size_t i = 0; i < Output.size(); ++i) Require(Output[i] == (i < 256 ? 0x5a5a5a5au : 0xcafebabeu), "a fill behind many queued labels did not land at " + std::to_string(i));
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_RECORD_THREAD", "1");
    _putenv_s("APS5_PREPARE_WORKERS", "2");
    _putenv_s("APS5_PREPARE_AHEAD", "4");
    _putenv_s("APS5_LABEL_RECORD_JOBS", "1");
#else
    setenv("APS5_RECORD_THREAD", "1", 1);
    setenv("APS5_PREPARE_WORKERS", "2", 1);
    setenv("APS5_PREPARE_AHEAD", "4", 1);
    setenv("APS5_LABEL_RECORD_JOBS", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        InputsAfterQueuedLabels();
        FillAfterManyQueuedLabels();
        AgcDriverShutdown_nid_postfix();
        std::puts("queued fill tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}

#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::VulkanDevice;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 32;
constexpr std::uint32_t Fill = 0xdeadbeefu;
alignas(256) std::array<std::uint32_t, Lanes> Input{};
alignas(256) std::array<std::uint32_t, Lanes + 8> Output{};
alignas(256) constexpr std::array<std::uint32_t, 7> Code{
    0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0xe0701000u, 0x80010201u, 0xbf810000u,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

ShaderRecompiler::RecompileResult Compile(VulkanDevice& device) {
    std::array<std::uint32_t, 8> userData{};
    const auto input = BufferDescriptor(Input.data(), sizeof(Input));
    const auto output = BufferDescriptor(Output.data(), Lanes * sizeof(std::uint32_t));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.ComputeTarget(Lanes),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void FillInput(std::uint32_t seed) {
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) Input[lane] = seed + lane * 0x01010101u;
}

void Dispatch(VulkanDevice& device, const ShaderRecompiler::RecompileResult& shader) {
    Output.fill(Fill);
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    device.Dispatch(shader, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    device.WaitIdle();
    Require(std::equal(Input.begin(), Input.end(), Output.begin()), "pipeline compile ahead returned stale or incorrect guest data");
    Require(std::all_of(Output.begin() + Lanes, Output.end(), [](std::uint32_t value) { return value == Fill; }), "pipeline compile ahead wrote outside its guest output descriptor");
}

VulkanDevice::PipelineAheadStatistics Await(VulkanDevice& device) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto counts = device.PipelineAheadCounters();
        if (counts.outstanding == 0) return counts;
        Require(std::chrono::steady_clock::now() < deadline, "pipeline compile ahead did not settle within five seconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Run(VulkanDevice& device, bool enabled) {
    FillInput(0x12345678u);
    const auto foreground = Compile(device);
    Require(foreground.PipelineVariantId() != 0, "foreground fixture has no runtime pipeline identity");
    Dispatch(device, foreground);
    const auto expected = Output;
    const auto shader = Compile(device);
    Require(shader.PipelineVariantId() != 0 && shader.PipelineVariantId() != foreground.PipelineVariantId(), "independent compilation reused the foreground fixture identity");
    FillInput(0x87654321u);
    device.BuildComputePipelineAhead(shader, reinterpret_cast<std::uintptr_t>(Code.data()));
    const auto ready = Await(device);
    Require(ready.failed == 0 && ready.outstanding == 0 && ready.retainedBytes == 0, "settled compile ahead retained a request or failed");
    Require(ready.accepted == (enabled ? 1u : 0u) && ready.built == (enabled ? 1u : 0u), "compile ahead did not respect its feature switch");
    device.BuildComputePipelineAhead(shader, reinterpret_cast<std::uintptr_t>(Code.data()));
    const auto repeated = device.PipelineAheadCounters();
    Require(repeated.accepted == ready.accepted && repeated.built == ready.built, "same-key compile hint rebuilt a finished pipeline");
    FillInput(0x12345678u);
    Dispatch(device, shader);
    Require(Output == expected, "foreground and compile-ahead pipelines produced different GPU results");
    FillInput(0x76543210u);
    Dispatch(device, shader);
    const auto refreshed = Output;
    Dispatch(device, foreground);
    Require(Output == refreshed, "reused compile-ahead pipeline differed from foreground on fresh guest data");
    const auto used = device.PipelineAheadCounters();
    Require(used.accepted == ready.accepted && used.built == ready.built && used.failed == 0, "dispatch changed compile-ahead admission or build outcomes");
    Require(used.useful == (enabled ? 1u : 0u), "foreground demand did not consume the finished compile-ahead pipeline once");
    const auto closing = Compile(device);
    const auto late = Compile(device);
    Require(late.PipelineVariantId() != closing.PipelineVariantId(), "post-close fixture reused the closing pipeline identity");
    device.BuildComputePipelineAhead(closing, reinterpret_cast<std::uintptr_t>(Code.data()));
    {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        device.PrepareForReplacement();
    }
    const auto closed = device.PipelineAheadCounters();
    Require(closed.outstanding == 0 && closed.retainedBytes == 0 && closed.failed == 0, "device replacement did not settle compile-ahead ownership");
    Require(closed.accepted == (enabled ? 2u : 0u) && closed.built + closed.present + closed.cancelled == closed.accepted, "device replacement lost accepted compile work");
    device.BuildComputePipelineAhead(late, reinterpret_cast<std::uintptr_t>(Code.data()));
    const auto after = device.PipelineAheadCounters();
    Require(after.accepted == closed.accepted && after.outstanding == 0 && after.retainedBytes == 0, "device replacement reopened its compile queue");
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "off"), "expected no argument or off");
        const bool enabled = argc == 1;
        Require(VulkanDevice::PipelineCompileAheadEnabled() == enabled, "pipeline compile ahead fixture environment does not match its mode");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, enabled);
        std::puts("compute pipeline ahead GPU parity and lifecycle passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

#include "prx/libSceAgcDriver/Execution/include/CompileService.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::VulkanDevice;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 32;
alignas(256) std::array<std::uint32_t, Lanes> Input{};
alignas(256) std::array<std::uint32_t, Lanes> Output{};
alignas(256) constexpr std::array<std::uint32_t, 7> Code{
    0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0xe0701000u, 0x80010201u, 0xbf810000u,
};

void SetVariable(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

ShaderRecompiler::RecompileResult Compile(VulkanDevice& device) {
    std::array<std::uint32_t, 8> userData{};
    const auto input = BufferDescriptor(Input.data(), sizeof(Input));
    const auto output = BufferDescriptor(Output.data(), sizeof(Output));
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

void Dispatch(VulkanDevice& device, const ShaderRecompiler::RecompileResult& shader, std::uint32_t seed) {
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) Input[lane] = seed + lane * 0x01010101u;
    Output.fill(0u);
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    device.Dispatch(shader, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    device.WaitIdle();
    Require(std::equal(Input.begin(), Input.end(), Output.begin()), "a compute pipeline created from the pipeline cache returned incorrect guest data");
}

}

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() / ("anyps5-compute-pipeline-cache-probe-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        SetVariable("ANYPS5_NO_SHADER_CACHE", "");
        SetVariable("ANYPS5_SHADER_CACHE_DIR", directory.string());
        SetVariable("APS5_COMPILE_SCHEDULER", "priority");
        SetVariable("APS5_COMPILE_PARK", "1");
        SetVariable("APS5_PIPELINE_PREWARM", "0");
        {
            const auto first = OpenVulkanTestDevice();
            if (!first) return VulkanTestSkipped;
            const auto shader = Compile(*first);
            if (auto ticket = first->TryComputePipeline(shader, reinterpret_cast<std::uintptr_t>(Code.data()))) ticket->Wait();
            Dispatch(*first, shader, 0x12345678u);
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            first->PrepareForReplacement();
        }
        const auto second = OpenVulkanTestDevice();
        Require(second != nullptr, "the second Vulkan device could not be opened");
        const auto before = second->PipelineCacheProbeCounters();
        const auto shader = Compile(*second);
        const auto ticket = second->TryComputePipeline(shader, reinterpret_cast<std::uintptr_t>(Code.data()));
        const auto after = second->PipelineCacheProbeCounters();
        Require(ticket == nullptr && after.computeHits == before.computeHits + 1u, "a compute pipeline in the Vulkan pipeline cache was parked instead of created from the cache");
        Dispatch(*second, shader, 0x87654321u);
        std::filesystem::remove_all(directory, error);
        std::printf("compute pipeline cache probe passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "compute pipeline cache probe test failed: %s\n", error.what());
        return 1;
    }
}

#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 16;
constexpr std::uint32_t Rows = 256;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 40> Code{
    0x34020084, 0x34060086, 0xe0381000, 0x80000401, 0x7e2a0280, 0xbf8c3f70,
    0x1e140b04,
    0x20160b04,
    0xd557000c, 0x041a0b04,
    0x7e1a6b04,
    0x7e1c6d04,
    0x7e1e4f04,
    0x7e204104,
    0x7e221504,
    0xd5038012, 0x00020b04,
    0xd4010008, 0x00020b04,
    0xd5010013, 0x00210280,
    0xd488000a, 0x00020f04,
    0xd5010014, 0x00290280,
    0xcc210015, 0x041a0b04,
    0x102c0b04,
    0x7e2ec104,
    0x7e30af04,
    0x7e32bf04,
    0xe0781000, 0x80010a03,
    0xe0781010, 0x80010e03,
    0xe0781020, 0x80011203,
    0xe0781030, 0x80011603,
    0xbf810000,
};

constexpr const char* Names[Results] = {
    "v_min_f32 v10, v4, v5",
    "v_max_f32 v11, v4, v5",
    "v_med3_f32 v12, v4, v5, v6",
    "v_sin_f32 v13, v4",
    "v_cos_f32 v14, v4",
    "v_log_f32 v15, v4",
    "v_fract_f32 v16, v4",
    "v_cvt_f16_f32 v17, v4",
    "v_add_f32_e64 v18, v4, v5 clamp",
    "v_cmp_lt_f32_e64 s8, v4, v5",
    "v_cmp_class_f32_e64 s10, v4, v7",
    "v_fma_mixlo_f16 v21, v4, v5, v6",
    "v_mul_f32 v22, v4, v5",
    "v_sin_f16 v23, v4",
    "v_log_f16 v24, v4",
    "v_fract_f16 v25, v4",
};

constexpr std::array<std::uint32_t, 35> F32Edges{
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00400000u,
    0x00800000u, 0x80800000u, 0x3f800000u, 0xbf800000u, 0x3f7fffffu, 0x3f800001u, 0x3e800000u,
    0xbf400000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
    0x7f800001u, 0xff812345u, 0x7fc12345u, 0x477fe000u, 0x477ff000u, 0xc77ff000u, 0x38800000u,
    0x387fc000u, 0x33800000u, 0x33000000u, 0x4b000001u, 0x501502f9u, 0x40490fdbu, 0x3dcccccdu,
};

constexpr std::array<std::uint32_t, 16> F16Edges{
    0x0000u, 0x8000u, 0x0001u, 0x03ffu, 0x0400u, 0x3c00u, 0xbc00u, 0x7bffu,
    0x7c00u, 0xfc00u, 0x7e00u, 0x7d00u, 0xfe01u, 0x3555u, 0x4248u, 0x5640u,
};

std::array<std::uint32_t, Inputs> Row(std::uint32_t row) {
    const auto edge = [](std::uint32_t index) { return F32Edges[index % F32Edges.size()]; };
    const std::uint32_t first = row < 192u ? edge(row) : F16Edges[row % 16u] | (F16Edges[(row / 16u) % 16u] << 16u);
    const std::uint32_t mask = row % 11u == 0u ? 0x3ffu : (1u << (row % 10u)) | (row % 3u == 0u ? 1u << ((row / 10u) % 10u) : 0u);
    return {first, edge(row * 7u + 3u), edge(row * 13u + 5u), mask};
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

void SetShared(bool shared) {
#ifdef _WIN32
    _putenv_s("APS5_NO_SHARED_FLOAT_FUNCTIONS", shared ? "" : "1");
#else
    if (shared) unsetenv("APS5_NO_SHARED_FLOAT_FUNCTIONS");
    else setenv("APS5_NO_SHARED_FLOAT_FUNCTIONS", "1", 1);
#endif
}

struct Calls {
    std::set<std::string> called;
    std::size_t total = 0;
};

Calls CalledFunctions(std::span<const std::uint32_t> words) {
    Require(words.size() > 5u, "shared float functions: the recompiler produced no SPIR-V module");
    std::map<std::uint32_t, std::string> names;
    std::vector<std::uint32_t> callees;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> 16u;
        const auto opcode = words[offset] & 0xffffu;
        Require(count != 0u && offset + count <= words.size(), "shared float functions: the module has a malformed instruction");
        if (opcode == spv::OpName) names[words[offset + 1]] = reinterpret_cast<const char*>(&words[offset + 2]);
        if (opcode == spv::OpFunctionCall) callees.push_back(words[offset + 3]);
        offset += count;
    }
    Calls calls;
    for (const auto callee : callees) {
        const auto name = names.find(callee);
        if (name == names.end()) continue;
        if (name->second.starts_with("outlined_") || name->second == "flush_f32_denormal" || name->second == "is_nan_f32_bits") {
            calls.called.insert(name->second);
            ++calls.total;
        }
    }
    return calls;
}

struct Mode {
    const char* name;
    std::optional<ShaderRecompiler::ShaderFloatMode> floatMode;
    bool flushesF32;
};

const std::array<Mode, 4> Modes{{
    {"default", std::nullopt, true},
    {"FLOAT_MODE 0xc0", ShaderRecompiler::ShaderFloatMode{0xc0u, false, false, false}, true},
    {"FLOAT_MODE 0xf0 IEEE", ShaderRecompiler::ShaderFloatMode{0xf0u, true, true, false}, false},
    {"FLOAT_MODE 0xcc", ShaderRecompiler::ShaderFloatMode{0xccu, true, false, false}, true},
}};

std::vector<std::uint32_t> Run(AgcDriver::VulkanDevice& device, std::uint32_t waveSize, const Mode& mode, std::uint32_t chunk, bool shared, Calls& calls) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto row = Row(chunk * Threads + tid);
        std::copy(row.begin(), row.end(), Input.begin() + tid * Inputs);
    }
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    request.context.floatMode = mode.floatMode;
    SetShared(shared);
    const auto result = ShaderRecompiler::Recompile(request);
    SetShared(true);
    calls = CalledFunctions(result.spirv.Words());
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    return {Output.begin(), Output.end()};
}

void Compare(std::uint32_t waveSize, const Mode& mode, std::uint32_t chunk, const std::vector<std::uint32_t>& shared, const std::vector<std::uint32_t>& inlined) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto row = Row(chunk * Threads + tid);
        for (std::uint32_t result = 0; result < Results; ++result) {
            const auto index = tid * Results + result;
            Require(shared[index] != 0xdeadbeefu, "shared float functions: " + std::string(Names[result]) + " was not stored");
            Require(shared[index] == inlined[index], "shared float functions, wave" + std::to_string(waveSize) + ", " + mode.name + ": " + Names[result] + " of (" + Hex(row[0]) + ", " + Hex(row[1]) + ", " + Hex(row[2]) + ", " + Hex(row[3]) + ") is " + Hex(shared[index]) + " through the shared function and " + Hex(inlined[index]) + " expanded in place");
        }
    }
}

void RequireCalled(const Calls& calls, const Mode& mode, std::uint32_t waveSize) {
    std::vector<std::string> expected{"outlined_FPMin32", "outlined_FPMax32", "outlined_FPMedTri32", "outlined_FPSin", "outlined_FPCos", "outlined_FPLog2", "outlined_FPFract32", "outlined_FPSaturate32", "outlined_FPCmpClass32", "outlined_FPInterpolateF16", "outlined_PackHalf2x16", "outlined_ConvertF16F32", "is_nan_f32_bits"};
    if (mode.flushesF32) expected.insert(expected.end(), {"outlined_FPOrdLessThan32", "flush_f32_denormal"});
    for (const auto& name : expected) {
        Require(calls.called.contains(name), "shared float functions, wave" + std::to_string(waveSize) + ", " + mode.name + ": the shader does not call " + name + ", so the comparison does not cover it");
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!TargetHasCapability(device->Target(), spv::CapabilityFloat64)) {
            std::puts("skipped, v_fma_mixlo_f16 is computed in f64 and the device has no shaderFloat64");
            return VulkanTestSkipped;
        }
        std::set<std::string> covered;
        for (const std::uint32_t waveSize : {32u, 64u}) {
            for (const auto& mode : Modes) {
                for (std::uint32_t chunk = 0; chunk < Rows / Threads; ++chunk) {
                    Calls sharedCalls;
                    Calls inlinedCalls;
                    const auto shared = Run(*device, waveSize, mode, chunk, true, sharedCalls);
                    const auto inlined = Run(*device, waveSize, mode, chunk, false, inlinedCalls);
                    Require(inlinedCalls.total == 0u, std::string("shared float functions: APS5_NO_SHARED_FLOAT_FUNCTIONS still calls ") + (inlinedCalls.called.empty() ? std::string("a shared function") : *inlinedCalls.called.begin()));
                    if (chunk == 0u) RequireCalled(sharedCalls, mode, waveSize);
                    covered.insert(sharedCalls.called.begin(), sharedCalls.called.end());
                    Compare(waveSize, mode, chunk, shared, inlined);
                }
            }
        }
        std::string list;
        for (const auto& name : covered) list += (list.empty() ? "" : ", ") + name;
        std::printf("shared float functions tests passed (%s)\n", list.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

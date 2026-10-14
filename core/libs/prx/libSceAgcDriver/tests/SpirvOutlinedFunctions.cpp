#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <spirv/unified1/spirv.hpp>

using namespace ShaderRecompiler;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint32_t> recompiled(std::span<const std::uint32_t> code) {
    const std::array<std::uint32_t, 4> userData{0x10000000u, 0x00100000u, 0x40u, 0x00027facu};
    const std::array<std::uint32_t, 2> capabilities{spv::CapabilityShader, spv::CapabilityStorageImageExtendedFormats};
    const std::array<std::string_view, 1> extensions{"SPV_KHR_storage_buffer_storage_class"};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x20000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.target.vulkanVersion = 0x00403000u;
    request.target.spirvVersion = 0x00010600u;
    request.target.subgroupSize = 32u;
    request.target.bdaAbiVersion = 1;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
    request.target.maxWorkgroupInvocations = 1024;
    request.target.maxWorkgroupSharedMemoryBytes = 49152;
    request.layout = {0, 0, 0, 128};
    request.useCache = false;
    const auto& words = Recompile(request).spirv.Words();
    return {words.begin(), words.end()};
}

struct Module {
    std::map<std::string, std::uint32_t> functionsByName;
    std::set<std::uint32_t> functions;
    std::map<std::uint32_t, std::size_t> calls;
    std::map<std::uint32_t, std::size_t> opcodesInMain;
};

Module parse(std::span<const std::uint32_t> words) {
    require(words.size() > 5u && words[0] == spv::MagicNumber, "the recompiler did not produce a SPIR-V module");
    Module module;
    std::map<std::uint32_t, std::string> names;
    std::uint32_t current = 0;
    std::uint32_t entry = 0;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> 16u;
        const auto opcode = words[offset] & 0xffffu;
        require(count != 0u && offset + count <= words.size(), "the module has a malformed instruction");
        if (opcode == spv::OpName) names[words[offset + 1]] = reinterpret_cast<const char*>(&words[offset + 2]);
        if (opcode == spv::OpEntryPoint) entry = words[offset + 2];
        if (opcode == spv::OpFunction) {
            current = words[offset + 2];
            module.functions.insert(current);
        }
        if (opcode == spv::OpFunctionCall) ++module.calls[words[offset + 3]];
        if (current != 0u && current == entry) ++module.opcodesInMain[opcode];
        if (opcode == spv::OpFunctionEnd) current = 0;
        offset += count;
    }
    for (const auto& [id, name] : names) {
        if (module.functions.contains(id)) module.functionsByName[name] = id;
    }
    return module;
}

}

int main() {
    try {
        const std::array<std::uint32_t, 7> code{
            0x10020100u,
            0x10040300u,
            0x10060500u,
            0x20020503u,
            0xe0700000u, 0x80000100u,
            0xbf810000u,
        };
        const auto words = recompiled(code);
        const auto module = parse(words);
        for (const auto& [target, count] : module.calls) require(module.functions.contains(target), "a call names no function of the module");
        const auto maximum = module.functionsByName.find("outlined_FPMax32");
        require(maximum != module.functionsByName.end(), "v_max_f32 was not emitted as one shared function");
        require(module.calls.contains(maximum->second), "the shared v_max_f32 function is never called");
        const auto flush = module.functionsByName.find("flush_f32_denormal");
        require(flush != module.functionsByName.end() && module.calls.at(flush->second) >= 4u, "the denormal flushes of the float operands are not calls of one function");
        const auto again = recompiled(code);
        require(again == words, "recompiling the same shader emitted a different module");
        std::printf("agc_spirv_outlined_functions_tests passed (%zu words, %zu functions)\n", words.size(), module.functions.size());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_spirv_outlined_functions_tests: %s\n", error.what());
        return 1;
    }
    return 0;
}

#include "SpirvBackend/SpirvModuleShape.hpp"
#include <spirv/unified1/spirv.hpp>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Emit(std::vector<std::uint32_t>& words, spv::Op op, std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1u) << 16u) | static_cast<std::uint32_t>(op));
    words.insert(words.end(), operands);
}

struct Options {
    std::uint32_t binding = 3;
    bool helper = false;
    std::uint32_t firstId = 1;
    bool extraCapability = false;
};

std::vector<std::uint32_t> Module(const Options& options) {
    std::vector<std::uint32_t> words{spv::MagicNumber, 0x00010300u, 0u, 100u, 0u};
    const auto id = [&](std::uint32_t index) { return options.firstId + index; };
    Emit(words, spv::OpCapability, {spv::CapabilityShader});
    if (options.extraCapability) Emit(words, spv::OpCapability, {spv::CapabilityInt64});
    Emit(words, spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    Emit(words, spv::OpEntryPoint, {spv::ExecutionModelGLCompute, id(0), 0x6e69616du, 0u});
    Emit(words, spv::OpExecutionMode, {id(0), spv::ExecutionModeLocalSize, 64u, 1u, 1u});
    Emit(words, spv::OpDecorate, {id(5), spv::DecorationBinding, options.binding});
    Emit(words, spv::OpDecorate, {id(5), spv::DecorationDescriptorSet, 0u});
    Emit(words, spv::OpDecorate, {id(3), spv::DecorationBlock});
    Emit(words, spv::OpMemberDecorate, {id(3), 0u, spv::DecorationOffset, 0u});
    Emit(words, spv::OpDecorate, {id(8), spv::DecorationSpecId, 7u});
    Emit(words, spv::OpTypeVoid, {id(1)});
    Emit(words, spv::OpTypeInt, {id(2), 32u, 0u});
    Emit(words, spv::OpTypeStruct, {id(3), id(2)});
    Emit(words, spv::OpTypePointer, {id(4), spv::StorageClassStorageBuffer, id(3)});
    Emit(words, spv::OpVariable, {id(4), id(5), spv::StorageClassStorageBuffer});
    Emit(words, spv::OpTypeFunction, {id(6), id(1)});
    Emit(words, spv::OpSpecConstant, {id(2), id(8), 5u});
    Emit(words, spv::OpFunction, {id(1), id(0), 0u, id(6)});
    Emit(words, spv::OpLabel, {id(7)});
    if (options.helper) Emit(words, spv::OpFunctionCall, {id(1), id(9)});
    Emit(words, spv::OpSelectionMerge, {id(10), 0u});
    Emit(words, spv::OpSwitch, {id(8), id(10), 1u, id(10), 2u, id(10)});
    Emit(words, spv::OpLabel, {id(10)});
    Emit(words, spv::OpReturn, {});
    Emit(words, spv::OpFunctionEnd, {});
    if (options.helper) {
        Emit(words, spv::OpFunction, {id(1), id(9), spv::FunctionControlDontInlineMask, id(6)});
        Emit(words, spv::OpLabel, {id(11)});
        Emit(words, spv::OpReturn, {});
        Emit(words, spv::OpFunctionEnd, {});
    }
    return words;
}

void Interfaces() {
    const auto reference = Module({});
    Require(!ShaderRecompiler::CompareSpirvInterfaces(reference, reference).has_value(), "a module differs from itself");
    Require(!ShaderRecompiler::CompareSpirvInterfaces(reference, Module({.helper = true})).has_value(), "an added helper function was reported as an interface change");
    Require(!ShaderRecompiler::CompareSpirvInterfaces(reference, Module({.firstId = 40})).has_value(), "renumbered ids were reported as an interface change");
    const auto binding = ShaderRecompiler::CompareSpirvInterfaces(reference, Module({.binding = 4}));
    Require(binding.has_value() && binding->find("variable") != std::string::npos, "an injected binding mismatch was not detected");
    const auto capability = ShaderRecompiler::CompareSpirvInterfaces(reference, Module({.extraCapability = true}));
    Require(capability.has_value() && capability->find("capability") != std::string::npos, "an injected capability mismatch was not detected");
    auto truncated = reference;
    truncated.pop_back();
    truncated.back() = 0xffff0000u;
    bool threw = false;
    try {
        static_cast<void>(ShaderRecompiler::CompareSpirvInterfaces(reference, truncated));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw, "a truncated module was accepted");
}

void Shape() {
    const auto shape = ShaderRecompiler::DescribeSpirvShape(Module({.helper = true}));
    Require(shape.functions == 2 && shape.functionCalls == 1, "function counts are wrong");
    Require(shape.switches == 1 && shape.switchTargets == 3 && shape.largestSwitchTargets == 3, "switch counts are wrong");
    Require(shape.helperFunctionWords == 9 && shape.entryFunctionWords > shape.helperFunctionWords && shape.largestHelperWords == 9, "function word counts are wrong");
}

}

int main() {
    try {
        Interfaces();
        Shape();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SPIR-V interface test failed: %s\n", error.what());
        return 1;
    }
    std::puts("SPIR-V interface tests passed");
    return 0;
}

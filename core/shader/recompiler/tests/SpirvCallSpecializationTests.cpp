#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#undef SPV_ENABLE_UTILITY_CODE
#include "SpirvBackend/SpirvSpecialization.hpp"
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Emit(std::vector<std::uint32_t>& words, spv::Op op, std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1u) << 16u) | static_cast<std::uint32_t>(op));
    words.insert(words.end(), operands);
}

std::vector<std::uint32_t> Module(bool runtimeSelector) {
    std::vector<std::uint32_t> words{spv::MagicNumber, 0x00010300u, 0u, 40u, 0u};
    Emit(words, spv::OpCapability, {spv::CapabilityShader});
    Emit(words, spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    Emit(words, spv::OpEntryPoint, {spv::ExecutionModelGLCompute, 1u, 0x6e69616du, 0u});
    Emit(words, spv::OpExecutionMode, {1u, spv::ExecutionModeLocalSize, 1u, 1u, 1u});
    Emit(words, spv::OpDecorate, {21u, spv::DecorationNoContraction});
    Emit(words, spv::OpTypeVoid, {2u});
    Emit(words, spv::OpTypeInt, {3u, 32u, 0u});
    Emit(words, spv::OpTypeFunction, {4u, 2u});
    Emit(words, spv::OpTypeFunction, {5u, 3u, 3u, 3u});
    Emit(words, spv::OpConstant, {3u, 6u, 1u});
    Emit(words, spv::OpConstant, {3u, 7u, 2u});
    Emit(words, spv::OpConstant, {3u, 8u, 10u});
    Emit(words, spv::OpConstant, {3u, 9u, 20u});
    Emit(words, spv::OpConstant, {3u, 20u, 0u});
    Emit(words, spv::OpTypePointer, {10u, spv::StorageClassPrivate, 3u});
    Emit(words, spv::OpVariable, {10u, 11u, spv::StorageClassPrivate});
    Emit(words, spv::OpFunction, {3u, 12u, spv::FunctionControlDontInlineMask, 5u});
    Emit(words, spv::OpFunctionParameter, {3u, 13u});
    Emit(words, spv::OpFunctionParameter, {3u, 14u});
    Emit(words, spv::OpLabel, {15u});
    Emit(words, spv::OpSelectionMerge, {19u, spv::SelectionControlMaskNone});
    Emit(words, spv::OpSwitch, {13u, 18u, 1u, 16u, 2u, 17u});
    Emit(words, spv::OpLabel, {16u});
    Emit(words, spv::OpIAdd, {3u, 21u, 14u, 8u});
    Emit(words, spv::OpBranch, {19u});
    Emit(words, spv::OpLabel, {17u});
    Emit(words, spv::OpIMul, {3u, 22u, 14u, 9u});
    Emit(words, spv::OpBranch, {19u});
    Emit(words, spv::OpLabel, {18u});
    Emit(words, spv::OpBranch, {19u});
    Emit(words, spv::OpLabel, {19u});
    Emit(words, spv::OpPhi, {3u, 23u, 21u, 16u, 22u, 17u, 20u, 18u});
    Emit(words, spv::OpReturnValue, {23u});
    Emit(words, spv::OpFunctionEnd, {});
    Emit(words, spv::OpFunction, {2u, 1u, spv::FunctionControlMaskNone, 4u});
    Emit(words, spv::OpLabel, {24u});
    if (runtimeSelector) {
        Emit(words, spv::OpLoad, {3u, 30u, 11u});
        Emit(words, spv::OpFunctionCall, {3u, 25u, 12u, 30u, 7u});
        Emit(words, spv::OpFunctionCall, {3u, 26u, 12u, 30u, 6u});
    } else {
        Emit(words, spv::OpFunctionCall, {3u, 25u, 12u, 6u, 7u});
        Emit(words, spv::OpFunctionCall, {3u, 26u, 12u, 7u, 6u});
    }
    Emit(words, spv::OpFunctionCall, {3u, 27u, 12u, 6u, 20u});
    Emit(words, spv::OpIAdd, {3u, 28u, 25u, 26u});
    Emit(words, spv::OpIAdd, {3u, 29u, 28u, 27u});
    Emit(words, spv::OpStore, {11u, 29u});
    Emit(words, spv::OpReturn, {});
    Emit(words, spv::OpFunctionEnd, {});
    return words;
}

struct Shape {
    std::size_t functions = 0;
    std::size_t switches = 0;
    std::set<std::uint32_t> callees;
    bool decorationsDefined = true;
    bool idsBelowBound = true;
};

Shape Describe(const std::vector<std::uint32_t>& words) {
    Shape shape;
    std::set<std::uint32_t> defined;
    std::vector<std::uint32_t> decorated;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        const auto op = static_cast<spv::Op>(words[cursor] & 0xffffu);
        Require(count != 0u && cursor + count <= words.size(), "the specialized module is truncated");
        bool hasResult = false;
        bool hasType = false;
        spv::HasResultAndType(op, &hasResult, &hasType);
        if (hasResult) {
            const auto result = words[cursor + (hasType ? 2u : 1u)];
            defined.insert(result);
            shape.idsBelowBound &= result < words[3];
        }
        if (op == spv::OpFunction) ++shape.functions;
        if (op == spv::OpSwitch) ++shape.switches;
        if (op == spv::OpFunctionCall) shape.callees.insert(words[cursor + 3u]);
        if (op == spv::OpDecorate) decorated.push_back(words[cursor + 1u]);
        cursor += count;
    }
    for (const auto target : decorated) shape.decorationsDefined &= defined.contains(target);
    return shape;
}

void ConstantSelectors() {
    const auto shape = Describe(ShaderRecompiler::SpecializeSpirv(Module(false)));
    Require(shape.functions == 3u, "constant selectors did not give one helper per selector value");
    Require(shape.switches == 0u, "a switch on a constant selector argument was not folded");
    Require(shape.callees.size() == 2u && !shape.callees.contains(12u), "calls still reach the shared helper");
    Require(shape.decorationsDefined, "a decoration names an id the specialized module does not define");
    Require(shape.idsBelowBound, "a cloned id is not below the module bound");
}

void RuntimeSelector() {
    const auto shape = Describe(ShaderRecompiler::SpecializeSpirv(Module(true)));
    Require(shape.functions == 3u, "a runtime selector did not keep the shared helper beside the constant one");
    Require(shape.callees.contains(12u), "calls with a runtime selector left the shared helper");
    Require(shape.switches == 1u, "the shared helper lost its switch");
    Require(shape.decorationsDefined && shape.idsBelowBound, "the partly specialized module is malformed");
}

}

int main() {
    try {
        ConstantSelectors();
        RuntimeSelector();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SPIR-V call specialization test failed: %s\n", error.what());
        return 1;
    }
    std::puts("SPIR-V call specialization tests passed");
    return 0;
}

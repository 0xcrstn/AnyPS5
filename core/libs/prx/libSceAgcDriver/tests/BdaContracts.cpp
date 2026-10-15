#include "BdaShader.hpp"
#include "BdaTranslationFixtures.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvStatistics.hpp"
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template<typename TAction>
void reject(TAction action, const char* expected) {
    try { action(); }
    catch (const std::runtime_error& error) {
        AgcDriver::Graphics::Require(std::string(error.what()).find(expected) != std::string::npos, std::string("unexpected contract error: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("missing contract rejection: ") + expected);
}

}

void RunBdaContractTests() {
    using namespace ShaderRecompiler;
    const std::array<std::uint32_t, 3> capabilities{spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    const std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    SpirvTargetOptions target{0x00401000u, 0x00010300u, 32, BdaAbi::Version, capabilities, extensions};
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.Info().usesDma = true;
    ValidateBdaTarget(program, target);
    target.bdaAbiVersion = 0;
    reject([&] { ValidateBdaTarget(program, target); }, "ABI version");
    target.bdaAbiVersion = BdaAbi::Version;
    target.supportedCapabilities = {};
    reject([&] { ValidateBdaTarget(program, target); }, "capability");
    target.supportedCapabilities = capabilities;
    target.supportedExtensions = {};
    reject([&] { ValidateBdaTarget(program, target); }, "extension");
    target.supportedExtensions = extensions;
    auto& block = program.CreateBlock();
    block.AppendInstruction(&program.CreateValue(IrOpcode::Barrier, IrType::Void));
    program.BlockOrder().push_back(&block);
    ValidateBdaTarget(program, target);
    AgcDriver::Graphics::Require(!BdaInvocationsMayStop(program), "a BDA program with barriers must keep its invocations running");
    program.Resources().stage = IrShaderStage::TessellationControl;
    ValidateBdaTarget(program, target);
    program.Resources().stage = IrShaderStage::Compute;
    RecompileRequest request{};
    request.target.bdaAbiVersion = BdaAbi::Version;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.fragmentShaderBarycentricEnabled = false;
    const RequestSerializer serializer;
    const auto encoded = serializer.Serialize(request);
    const auto decoded = serializer.Deserialize(encoded);
    AgcDriver::Graphics::Require(decoded.request.target.bdaAbiVersion == BdaAbi::Version && decoded.request.target.supportedCapabilities.size() == capabilities.size() && decoded.request.target.supportedExtensions[1] == extensions[1], "BDA request serialization changed target contract");
    auto invalid = encoded;
    invalid[0] = invalid[0] == 'A' ? 'B' : 'A';
    reject([&] { static_cast<void>(serializer.Deserialize(invalid)); }, "request signature");
    reject([&] { static_cast<void>(serializer.Deserialize("NVNQQWMAAAA=")); }, "serialization version");
}

void RunBdaTableContractTests() {
    namespace Abi = ShaderRecompiler::BdaAbi;
    namespace Table = ShaderRecompiler::BdaTable;
    using AgcDriver::Graphics::Require;
    Require(Table::ParseTranslation(nullptr) == Table::Translation::Linear && Table::ParseTranslation("linear") == Table::Translation::Linear && Table::ParseTranslation("linear_verify") == Table::Translation::LinearVerify && Table::ParseTranslation("legacy") == Table::Translation::Legacy && Table::ParseTranslation("radix") == Table::Translation::Radix && Table::ParseTranslation("verify") == Table::Translation::Verify, "APS5_BDA_ABI values were not parsed");
    reject([] { static_cast<void>(Table::ParseTranslation("on")); }, "APS5_BDA_ABI");
    const auto tables = BdaTableFixtures();
    for (std::size_t fixture = 0; fixture < tables.size(); ++fixture) {
        const auto& ranges = tables[fixture];
        const auto label = "BDA radix fixture " + std::to_string(fixture) + ": ";
        const auto legacy = Table::Build(ranges, Table::Translation::Legacy);
        Table::Validate(legacy, Table::Translation::Legacy);
        Require(legacy.size() == Table::HeaderWords + ranges.size() * Table::RangeWords && legacy[0] == Abi::Version && legacy[3] == 0, label + "the legacy table layout changed");
        const auto words = Table::Build(ranges, Table::Translation::Radix);
        Table::Validate(words, Table::Translation::Radix);
        Require(words == Table::Build(ranges, Table::Translation::Verify), label + "radix and verify tables differ");
        for (const auto& query : BdaFixtureQueries(ranges, fixture == 3)) {
            if (query.bytes != 1u) continue;
            Require(Table::RadixSelected(words, query.address) == Table::SelectedByBegins(ranges, query.address), label + "radix selection differs from the range search");
        }
        if (ranges.empty()) continue;
        const auto rootWord = words[3];
        for (std::size_t index = 0; index < words.size(); ++index) {
            if (index >= Table::RadixRangeWord && index < rootWord) continue;
            if (words.size() > 4096u && index >= rootWord && index % 61u != 0u) continue;
            auto corrupted = words;
            corrupted[index] ^= 1u;
            reject([&] { Table::Validate(corrupted, Table::Translation::Radix); }, "invalid BDA table");
        }
    }
    std::vector<Abi::Range> unsorted{{0x2000, 0x3000, 0x1000, Abi::Read, 0}, {0x1000, 0x2000, 0x2000, Abi::Read, 0}};
    reject([&] { static_cast<void>(Table::Build(unsorted, Table::Translation::Radix)); }, "sorted by begin");
}

void RunSpirvStatisticsTests() {
    using AgcDriver::Graphics::Require;
    const auto translation = BdaTestTranslation();
    const auto relative = BdaTestRelative();
    SetBdaTestTranslation(ShaderRecompiler::BdaTable::Translation::Legacy);
    SetBdaTestRelative(ShaderRecompiler::BdaTable::Relative::Off);
    SetBdaTestSpanHelpers(ShaderRecompiler::RewriteMode::Off);
    const auto statistics = ShaderRecompiler::AnalyzeSpirv(MakeBdaSpanReadTestShader(0x7fff12340000ULL, 0u, 0b0101u, false, true));
    SetBdaTestSpanHelpers(ShaderRecompiler::BdaSpanHelpersSetting());
    SetBdaTestRelative(relative);
    SetBdaTestTranslation(translation);
    const auto calls = [&](const char* name) {
        const auto found = statistics.callsByCallee.find(name);
        return found != statistics.callsByCallee.end() ? found->second : 0u;
    };
    Require(calls("read_bda_span_stop") == 1u && calls("read_bda_span") == 0u && calls("probe_bda_pointer") == 2u && calls("load_bda_span") == 2u && calls("read_bda_dword_bytes_stop") == 1u && calls("read_bda_dword_bytes") == 1u && calls("get_bda_pointer") == 2u, "SPIR-V statistics miscounted the BDA helper calls");
    Require(statistics.functions >= 5u && statistics.physicalLoads >= 2u && statistics.loops >= 2u && statistics.float64Arithmetic == 0u && statistics.lines == 0u, "SPIR-V statistics miscounted the BDA test shader");
    Require(statistics.instructionsByFunction.contains("get_bda_pointer") && ShaderRecompiler::SpirvStatisticsJson(statistics).find("\"probe_bda_pointer\":2") != std::string::npos, "SPIR-V statistics JSON lost the helper counts");
}

void RunBdaInlineFastPathTests() {
    using AgcDriver::Graphics::Require;
    namespace Table = ShaderRecompiler::BdaTable;
    const auto control = [](const std::vector<std::uint32_t>& words, const std::string& name) {
        std::uint32_t target = 0;
        for (std::size_t at = 5; at < words.size(); at += words[at] >> 16u) {
            const auto opcode = words[at] & 0xffffu;
            if (opcode == spv::OpName && std::string(reinterpret_cast<const char*>(&words[at + 2])) == name) target = words[at + 1];
            if (opcode == spv::OpFunction && target != 0 && words[at + 2] == target) return words[at + 3];
        }
        throw std::runtime_error("no function " + name + " in the test shader");
    };
    const auto previousTranslation = BdaTestTranslation();
    const auto previousRelative = BdaTestRelative();
    SetBdaTestTranslation(Table::Translation::Radix);
    for (const auto relative : {Table::Relative::Off, Table::Relative::On}) {
        SetBdaTestRelative(relative);
        for (const bool inlined : {false, true}) {
            SetBdaTestInlineFastPath(inlined);
            const auto expected = inlined && relative == Table::Relative::On ? static_cast<std::uint32_t>(spv::FunctionControlInlineMask) : static_cast<std::uint32_t>(spv::FunctionControlDontInlineMask);
            Require(control(MakeBdaTestShader(0x7fff12340000ULL, 8u), "get_bda_pointer") == expected, "the BDA lookup has the wrong inline control");
        }
    }
    SetBdaTestInlineFastPath(Table::InlineFastPathSetting());
    SetBdaTestRelative(previousRelative);
    SetBdaTestTranslation(previousTranslation);
}

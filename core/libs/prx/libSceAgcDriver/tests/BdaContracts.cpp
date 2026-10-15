#include "BdaShader.hpp"
#include "BdaTranslationFixtures.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include <array>
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
    Require(Table::ParseTranslation(nullptr) == Table::Translation::Legacy && Table::ParseTranslation("radix") == Table::Translation::Radix && Table::ParseTranslation("verify") == Table::Translation::Verify, "APS5_BDA_ABI values were not parsed");
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

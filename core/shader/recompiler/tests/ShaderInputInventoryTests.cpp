#include "ShaderInputInventory.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {


void Require(bool condition) {
    if (!condition) throw std::runtime_error("shader input inventory assertion failed");
}

template<typename Function>
void RequireRejected(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    Require(rejected);
}

void SetDirectory(const std::filesystem::path& directory) {
#ifdef _WIN32
    Require(_wputenv_s(L"ANYPS5_SHADER_INPUT_DIR", directory.c_str()) == 0);
    Require(_putenv_s("ANYPS5_NO_SHADER_INPUTS", "0") == 0);
    Require(_putenv_s("APS5_NO_SHADER_INPUTS", "0") == 0);
#else
    Require(setenv("ANYPS5_SHADER_INPUT_DIR", directory.c_str(), 1) == 0);
    Require(setenv("ANYPS5_NO_SHADER_INPUTS", "0", 1) == 0);
    Require(setenv("APS5_NO_SHADER_INPUTS", "0", 1) == 0);
#endif
}

}

int main(int argc, char** argv) {
    using namespace ShaderRecompiler;
    if (argc == 2 && std::string_view(argv[1]) == "--disabled") {
#ifdef _WIN32
        Require(_putenv_s("ANYPS5_NO_SHADER_INPUTS", "1") == 0);
#else
        Require(setenv("ANYPS5_NO_SHADER_INPUTS", "1", 1) == 0);
#endif
        const auto result = std::make_shared<const RecompileResult>();
        Require(!ShouldCaptureShaderInput(result));
        Require(CaptureShaderInput(RecompileRequest{}, result) == ShaderInputCaptureStatus::Disabled);
        Require(ShaderInputCaptureTotals().queued == 0u);
        return 0;
    }
    const auto root = std::filesystem::temp_directory_path() / ("anyps5-shader-input-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int status = 0;
    try {
        Require(ShaderInputInventory::Identity("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        Require(ShaderInputInventory::Identity("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        Require(ShaderInputInventory::Identity(std::string(55u, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
        Require(ShaderInputInventory::Identity(std::string(56u, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
        Require(ShaderInputInventory::Identity(std::string(64u, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

        std::array<std::uint32_t, 3> code{0xbe800000u, 0x12345678u, 0xbf810000u};
        std::array<std::byte, 4> header{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        std::array<std::uint32_t, 4> userData{0x2000u, 0u, 7u, 9u};
        std::array<std::byte, 8> memoryBytes{std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
        std::array<MemoryRegion, 1> memory{{{0x2000u, memoryBytes}}};
        std::array<std::uint32_t, 1> fallback{17u};
        std::array<std::uint32_t, 2> capabilities{11u, 4427u};
        std::string extension = "SPV_KHR_shader_draw_parameters";
        std::array<std::string_view, 1> extensions{extension};
        std::array<LinkedProgram, 1> linked{{{ProgramRole::Domain, {ShaderStage::TessellationEvaluation, 0x4000u, code, 0x5000u, header}, 8u, 12u, userData}}};
        RecompileRequest request{};
        request.shader = {ShaderStage::Compute, 0x1000u, code, 0x1100u, header};
        request.context.waveSize = 32u;
        request.context.userDataBaseRegister = 8u;
        request.context.userData = userData;
        request.context.memory = memory;
        request.context.linkedCallFallbacks = fallback;
        request.context.compute = ShaderComputeStageInfo{{8u, 4u, 2u}, 64u, {true, false, true}, true, 3u, {4u, 2u, 1u}, 32u};
        request.context.floatMode = ShaderFloatMode{193u, true, true, true};
        request.target.vulkanVersion = 0x00403000u;
        request.target.spirvVersion = 0x00010600u;
        request.target.subgroupSize = 32u;
        request.target.bdaAbiVersion = 12u;
        request.target.supportedCapabilities = capabilities;
        request.target.supportedExtensions = extensions;
        request.target.fragmentShaderBarycentricEnabled = true;
        request.target.maxWorkgroupSize = {1024u, 1024u, 64u};
        request.target.maxWorkgroupInvocations = 1024u;
        request.target.maxWorkgroupSharedMemoryBytes = 32768u;
        request.target.mesh = MeshTargetLimits{{128u, 2u, 1u}, 128u, 32768u, 256u, 128u, 256u, 65536u, 32u, 32u};
        request.target.tessellation = TessellationTargetLimits{32u, 128u, 128u, 120u, 4096u, 128u, 128u};
        request.target.nonConstantImageOffsets = true;
        request.target.srgbDecodeFormats = 0x15u;
        request.target.narrowSubgroupClock = true;
        request.target.fixedPushSlots = true;
        request.target.float64DenormPreserve = true;
        request.target.registrationModules = true;
        request.target.registrationWordLimit = 12345u;
        request.target.vertexSubgroups = false;
        request.target.float16DenormPreserve = true;
        request.layout = {3u, 7u, 16u, 112u};
        request.graphics = GraphicsCompileContext{12u, linked, std::nullopt, TessellationConfiguration{3u, 4u, 2u, 1u, 2u, 63u, 37u, false, 0u, false}, {0x6000u, 57u, 4u, 9u}};
        const auto original = RequestSerializer{}.SerializeBinary(request);
        const auto rawIdentity = ShaderInputInventory::Identity(original);
        ShaderInputInventory inventory(root / "roundtrip");
        Require(inventory.Store(request) == ShaderInputCaptureStatus::Stored);
        Require(inventory.Store(request) == ShaderInputCaptureStatus::Duplicate);
        const auto records = inventory.List();
        Require(records.size() == 1u);
        const auto identity = records.front().identity;

        code[0] ^= 1u;
        header[0] = std::byte{9};
        userData[2] ^= 1u;
        memoryBytes[0] = std::byte{9};
        fallback[0] = 19u;
        capabilities[0] = 12u;
        extension[0] = 'X';
        const auto replay = inventory.Load(records.front().path);
        Require(RequestSerializer{}.SerializeBinary(replay.request) == original);
        Require(replay.request.target.fixedPushSlots && replay.request.layout.pushConstantOffsetBytes == 16u);
        Require(replay.request.context.memory.front().bytes.front() == std::byte{5});
        Require(replay.request.graphics->linkedPrograms.front().binary.code.front() == 0xbe800000u);
        Require(TessellationInstancePatches(replay.request) == 19u);
        std::string configurationMismatch(8u, '\0');
        configurationMismatch[0] = 1;
        configurationMismatch.push_back('\xff');
        configurationMismatch.append(original);
        const auto mismatchIdentity = ShaderInputInventory::Identity(configurationMismatch);
        const auto mismatchPath = root / "roundtrip" / (mismatchIdentity + ".input");
        {
            std::ifstream stored(records.front().path, std::ios::binary);
            std::string storedHeader;
            std::getline(stored, storedHeader);
            const auto dialectBegin = storedHeader.find(' ', storedHeader.find(' ') + 1u) + 1u;
            const auto dialectEnd = storedHeader.find(' ', dialectBegin);
            std::ofstream file(mismatchPath, std::ios::binary);
            file << "APS5INPUT 1 " << storedHeader.substr(dialectBegin, dialectEnd - dialectBegin) << ' ' << mismatchIdentity << ' ' << configurationMismatch.size() << '\n';
            file.write(configurationMismatch.data(), static_cast<std::streamsize>(configurationMismatch.size()));
        }
        RequireRejected([&] { static_cast<void>(inventory.Load(mismatchPath)); });
        std::filesystem::remove(mismatchPath);

        auto changed = replay.request;
        changed.target.fixedPushSlots = false;
        Require(ShaderInputInventory::Identity(RequestSerializer{}.SerializeBinary(changed)) != rawIdentity);
        changed = replay.request;
        changed.layout.descriptorSet += 1u;
        Require(ShaderInputInventory::Identity(RequestSerializer{}.SerializeBinary(changed)) != rawIdentity);
        changed = replay.request;
        changed.graphics->draw.indexCount += 3u;
        Require(ShaderInputInventory::Identity(RequestSerializer{}.SerializeBinary(changed)) != rawIdentity);
        Require(TessellationInstancePatches(changed) == 20u);
        auto smallRequest = replay.request;
        smallRequest.graphics.reset();
        smallRequest.context.memory = {};
        smallRequest.context.userData = {};
        const auto smallBytes = RequestSerializer{}.SerializeBinary(smallRequest);
        ShaderInputInventory limited(root / "limited", {4096u, 1u, 8192u});
        Require(limited.StoreBinary(smallBytes) == ShaderInputCaptureStatus::Stored);
        smallRequest.layout.descriptorSet += 1u;
        Require(limited.Store(smallRequest) == ShaderInputCaptureStatus::Full);
        Require(limited.StoreBinary(std::string(4097u, 'x')) == ShaderInputCaptureStatus::TooLarge);
        Require(limited.StoreBinary("bad") == ShaderInputCaptureStatus::Invalid);
        ShaderInputInventory firstWriter(root / "concurrent", {4096u, 1u, 8192u});
        ShaderInputInventory secondWriter(root / "concurrent", {4096u, 1u, 8192u});
        ShaderInputCaptureStatus firstStatus = ShaderInputCaptureStatus::Invalid;
        ShaderInputCaptureStatus secondStatus = ShaderInputCaptureStatus::Invalid;
        const auto secondBytes = RequestSerializer{}.SerializeBinary(smallRequest);
        std::thread first([&] { firstStatus = firstWriter.StoreBinary(smallBytes); });
        std::thread second([&] { secondStatus = secondWriter.StoreBinary(secondBytes); });
        first.join();
        second.join();
        Require(firstWriter.List().size() == 1u);
        Require((firstStatus == ShaderInputCaptureStatus::Stored) != (secondStatus == ShaderInputCaptureStatus::Stored));
        const auto fileBytes = limited.List().front().bytes;
        ShaderInputInventory quota(root / "quota", {fileBytes, 4u, fileBytes * 2u - 1u});
        Require(quota.StoreBinary(smallBytes) == ShaderInputCaptureStatus::Stored);
        Require(quota.StoreBinary(secondBytes) == ShaderInputCaptureStatus::Full);
        ShaderInputInventory tiny(root / "tiny", {64u, 1u, 128u});
        Require(tiny.Store(request) == ShaderInputCaptureStatus::TooLarge);
        Require(tiny.List().empty());
        auto overlapping = replay.request;
        const std::array<MemoryRegion, 2> invalidMemory{{{0x2000u, memoryBytes}, {0x2004u, memoryBytes}}};
        overlapping.context.memory = invalidMemory;
        Require(inventory.Store(overlapping) == ShaderInputCaptureStatus::Invalid);
        auto wrongAddress = replay.request;
        const std::array<MemoryRegion, 1> overflowMemory{{{~std::uint64_t{0}, memoryBytes}}};
        wrongAddress.context.memory = overflowMemory;
        Require(inventory.Store(wrongAddress) == ShaderInputCaptureStatus::Invalid);

        const auto truncatedPath = root / "roundtrip" / (std::string(64u, 'a') + ".input");
        std::filesystem::copy_file(records.front().path, truncatedPath);
        RequireRejected([&] { static_cast<void>(inventory.Load(truncatedPath)); });
        std::filesystem::remove(truncatedPath);
        {
            std::fstream file(records.front().path, std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(10u);
            file.put('2');
        }
        RequireRejected([&] { static_cast<void>(inventory.Load(records.front().path)); });
        {
            std::fstream file(records.front().path, std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(10u);
            file.put('1');
            file.seekp(12u);
            file.put('B');
        }
        RequireRejected([&] { static_cast<void>(inventory.Load(records.front().path)); });
        {
            std::fstream file(records.front().path, std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(12u);
            file.put('L');
        }
        {
            std::fstream file(records.front().path, std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(-1, std::ios::end);
            file.put('\x7f');
        }
        RequireRejected([&] { static_cast<void>(inventory.Load(records.front().path)); });
        const auto malformed = root / "roundtrip" / "bad.input";
        { std::ofstream file(malformed); file << "invalid"; }
        RequireRejected([&] { static_cast<void>(inventory.List()); });
        std::filesystem::remove(malformed);

        { std::ofstream blocked(root / "automatic"); blocked << "not a directory"; }
        SetDirectory(root / "automatic");
        const auto result = std::make_shared<const RecompileResult>();
        Require(CaptureShaderInput(replay.request, result) == ShaderInputCaptureStatus::Queued);
        const auto copiedResult = *result;
        FlushShaderInputs();
        Require(FindShaderInputIdentity(result).empty() && FindShaderInputIdentity(copiedResult).empty());
        Require(!ShouldCaptureShaderInput(result));
        const auto incompleteResult = std::make_shared<const RecompileResult>();
        Require(CaptureShaderInput(replay.request, incompleteResult, false) == ShaderInputCaptureStatus::Invalid);
        Require(!ShouldCaptureShaderInput(incompleteResult));
        std::filesystem::remove(root / "automatic");
        std::this_thread::sleep_for(std::chrono::milliseconds(5100));
        Require(ShouldCaptureShaderInput(result) && ShouldCaptureShaderInput(incompleteResult));
        Require(CaptureShaderInput(replay.request, result) == ShaderInputCaptureStatus::Queued);
        FlushShaderInputs();
        Require(FindShaderInputIdentity(result) == identity && FindShaderInputIdentity(*result) == identity);
        Require(FindShaderInputIdentity(copiedResult) == identity);
        Require(!ShouldCaptureShaderInput(result));
        Require(CaptureShaderInput(replay.request, result) == ShaderInputCaptureStatus::Duplicate);
        Require(CaptureShaderInput(RecompileRequest{}, result) == ShaderInputCaptureStatus::Duplicate);
        SetDebugProbeActive(true);
        Require(CaptureShaderInput(replay.request, result) == ShaderInputCaptureStatus::Invalid);
        SetDebugProbeActive(false);
        Require(CaptureShaderInput(replay.request, result, false) == ShaderInputCaptureStatus::Invalid);
        const auto totals = ShaderInputCaptureTotals();
        Require(totals.queued == 2u && totals.stored == 1u && totals.duplicate == 2u && totals.ioErrors == 1u && totals.invalid == 3u && totals.cooldown >= 2u && totals.pendingEntries == 0u && totals.pendingBytes == 0u);
        ShaderInputInventory automatic(root / "automatic");
        Require(automatic.List().size() == 1u && RequestSerializer{}.SerializeBinary(automatic.Load(automatic.List().front().path).request) == original);
        const auto unassociated = std::make_shared<const RecompileResult>();
        Require(FindShaderInputIdentity(unassociated).empty());
        std::cout << "shader input identity, roundtrip, snapshot, target, corruption, bounds and capture cases passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return status;
}

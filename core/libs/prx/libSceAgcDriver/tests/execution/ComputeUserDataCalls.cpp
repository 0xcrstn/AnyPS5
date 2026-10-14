#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "SceShaders.hpp"
#include "ShaderDiskCache.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Untouched = 0xcafebabeu;

alignas(256) constexpr std::array<std::uint32_t, 64> Caller{
    0x7e020280u, 0xbe940383u, 0xbe8e0300u, 0xbe8f0301u, 0xbe8e210eu, 0xbe8e0302u, 0xbe8f0303u, 0xbe8e210eu,
    0x80948114u, 0xbf078014u, 0xbf85fff7u, 0x34040082u, 0xe0701000u, 0x80010102u, 0xbf810000u, 0xbf9f0000u,
    0x30306c73u, 0x00000040u, 0x00000008u};
alignas(256) constexpr std::array<std::uint32_t, 64> SecondCaller = Caller;
alignas(256) constexpr std::array<std::uint32_t, 2> AddFive{0x4a020285u, 0xbe80200eu};
alignas(256) constexpr std::array<std::uint32_t, 2> AddSixtyFour{0x4a0202c0u, 0xbe80200eu};
alignas(256) constexpr std::array<std::uint32_t, 2> AddOne{0x4a020281u, 0xbe80200eu};
alignas(256) std::array<std::uint32_t, Threads> Output{};

std::array<std::uint32_t, 8> UserData(const void* first, const void* second) {
    const auto low = reinterpret_cast<std::uintptr_t>(first);
    const auto high = reinterpret_cast<std::uintptr_t>(second);
    const auto output = reinterpret_cast<std::uintptr_t>(Output.data());
    return {static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32u), static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32u),
        static_cast<std::uint32_t>(output), static_cast<std::uint32_t>((output >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(Output)), 0x31016facu};
}

std::vector<std::uint32_t> Commands(const std::array<std::uint32_t, 8>& data, bool wave32, const std::uint32_t* caller) {
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{Threads, 1, 1};
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(caller);
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{static_cast<std::uint32_t>(data.size()) << 1u};
    registers(0x213, resource);
    registers(0x240, data);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x41u | (wave32 ? 0x8000u : 0u)});
    return words;
}

void Dispatch(const void* first, const void* second, bool wave32, const std::uint32_t* caller = Caller.data()) {
    Output.fill(Untouched);
    const auto words = Commands(UserData(first, second), wave32, caller);
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output.data()), sizeof(Output));
}

void Check(const void* first, const void* second, bool wave32, std::uint32_t expected, const std::string& what, const std::uint32_t* caller = Caller.data()) {
    Dispatch(first, second, wave32, caller);
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        Require(Output[lane] == expected, what + (wave32 ? " (wave32)" : " (wave64)") + ": lane " + std::to_string(lane) + " is " + std::to_string(Output[lane]) + ", expected " + std::to_string(expected));
    }
}

void Register(bool wave32, const std::array<std::uint32_t, 64>& caller) {
    struct Header {
        Shader shader{};
        std::array<ShaderRegister, 7> registers{};
        ShaderSpecialRegs specials{};
    };
    alignas(8) static std::array<Header, 2> headers{};
    auto& header = headers[&caller == &Caller ? 0 : 1];
    const auto address = reinterpret_cast<std::uintptr_t>(caller.data());
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = sizeof(caller);
    header.shader.code = caller.data();
    header.shader.sh_registers = header.registers.data();
    header.shader.num_sh_registers = header.registers.size();
    header.shader.specials = &header.specials;
    header.specials.dispatch_modifier = wave32 ? 0x8000u : 0u;
    header.registers = {{{0x20c, static_cast<std::uint32_t>(address >> 8u)}, {0x20d, static_cast<std::uint32_t>(address >> 40u)}, {0x207, Threads}, {0x208, 1}, {0x209, 1}, {0x212, 0}, {0x213, 8u << 1u}}};
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

template <typename TAction>
void ExpectFailure(TAction action, const char* expected) {
    try {
        action();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, std::string("unexpected failure: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("expected a failure containing ") + expected);
}

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void RequireCounts(std::uint64_t ahead, std::uint64_t atUse, const std::string& what) {
    const auto counts = AgcDriver::DriverDetail::CountUserDataCalleePreparations();
    Require(counts.ahead == ahead && counts.atUse == atUse, what + ": " + std::to_string(counts.ahead) + " preparations ahead of use and " + std::to_string(counts.atUse) + " at use, expected " + std::to_string(ahead) + " and " + std::to_string(atUse));
}

void PersistCallees(const char* self) {
    Register(true, Caller);
    Check(AddFive.data(), AddSixtyFour.data(), true, 3u * (5u + 64u), "the first process linking two user-data callees");
    RequireCounts(0, 1, "the first process");
    ShaderRecompiler::ShaderDiskCache::Flush();
    const auto stored = ShaderRecompiler::ShaderDiskCache::LoadUserDataCallees();
    Require(stored.size() == 2 && stored[0].linkRegister == 14u && stored[1].linkRegister == 14u && stored[0].userDataRegister != stored[1].userDataRegister, "the first process did not store both linked callees, it stored " + std::to_string(stored.size()));
    const std::string command = "\"" + std::string(self) + "\" --persisted-load";
    Require(std::system(command.c_str()) == 0, "the process loading the persisted callees failed");
}

void LoadPersistedCallees() {
    Register(true, SecondCaller);
    Check(AddFive.data(), AddSixtyFour.data(), true, 3u * (5u + 64u), "a registered caller prepared with the callees an earlier process linked", SecondCaller.data());
    RequireCounts(1, 0, "a registered caller with persisted callees");
    Check(AddOne.data(), AddSixtyFour.data(), true, 3u * (1u + 64u), "a persisted callee whose guest code changed", SecondCaller.data());
    RequireCounts(1, 1, "a persisted callee whose guest code changed");
}

}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "";
        Require(argc == 1 || (argc == 2 && (mode == "--registered" || mode == "--persisted" || mode == "--persisted-load")), "usage: [--registered | --persisted]");
        const bool registered = argc == 2;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        if (mode == "--persisted-load") {
            LoadPersistedCallees();
            AgcDriverShutdown_nid_postfix();
            return 0;
        }
        if (mode == "--persisted") {
            const auto directory = std::filesystem::temp_directory_path() / ("aps5-user-data-callees-test-" + std::to_string(std::random_device{}()));
            std::filesystem::remove_all(directory);
            SetEnvironment("ANYPS5_NO_SHADER_CACHE", "0");
            SetEnvironment("ANYPS5_SHADER_CACHE_DIR", directory.string());
            std::exception_ptr failure;
            try {
                PersistCallees(argv[0]);
            } catch (...) {
                failure = std::current_exception();
            }
            AgcDriverShutdown_nid_postfix();
            std::error_code error;
            std::filesystem::remove_all(directory, error);
            if (failure != nullptr) std::rethrow_exception(failure);
            std::puts("persisted user-data callee tests passed");
            return 0;
        }
        if (registered) {
            Register(true, Caller);
            Register(true, SecondCaller);
        }
        for (const bool wave32 : registered ? std::vector<bool>{true} : std::vector<bool>{false, true}) {
            Check(AddFive.data(), AddSixtyFour.data(), wave32, 3u * (5u + 64u), "two user-data callees called from a loop");
            Check(AddOne.data(), AddSixtyFour.data(), wave32, 3u * (1u + 64u), "the first callee replaced");
            Check(AddSixtyFour.data(), AddSixtyFour.data(), wave32, 3u * (64u + 64u), "one callee behind both pointers");
        }
        if (registered) {
            Check(AddSixtyFour.data(), AddSixtyFour.data(), true, 3u * (64u + 64u), "a second registered caller prepared with the callees another dispatch linked", SecondCaller.data());
            Check(AddFive.data(), AddOne.data(), true, 3u * (5u + 1u), "a second registered caller with callees no dispatch linked before", SecondCaller.data());
        }
        const auto* unmapped = reinterpret_cast<const void*>(std::uintptr_t{0x1000u});
        ExpectFailure([&] { Dispatch(AddFive.data(), unmapped, registered); }, "is not readable");
        ExpectFailure([] { AgcDriverShutdown_nid_postfix(); }, "is not readable");
        std::puts("compute user-data call tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception&) {
        }
        return 1;
    }
}

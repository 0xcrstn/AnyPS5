#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

alignas(256) std::array<std::uint32_t, 12> ReaderCode{0xf4040100u, 0xfa000000u, 0xf4080201u, 0xfa000000u, 0xbf8cc07fu, 0xf4000182u, 0xfa000000u, 0xbf8cc07fu, 0x7e000206u, 0xe0700000u, 0x80020000u, 0xbf810000u};

struct Slot {
    alignas(256) std::array<std::uint32_t, 4> value{};
    alignas(256) std::array<std::uint32_t, 2> table{};
    alignas(256) std::array<std::uint32_t, 4> output{};
    alignas(256) std::array<std::uint32_t, 4> outputDescriptor{};
    alignas(256) volatile std::uint32_t done = 0;
};

Slot Partial;

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void SubmitPartial(Slot& slot, std::uint32_t value) {
    slot.value[0] = value;
    slot.table = {Low(slot.value.data()), High(slot.value.data())};
    slot.outputDescriptor = {Low(slot.output.data()), High(slot.output.data()) & 0xffffu, static_cast<std::uint32_t>(sizeof(slot.output)), 0x31016facu};
    std::vector<std::uint32_t> words;
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ReaderCode.data()));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    SetShaderRegister(words, 0x207, 0x00100040u);
    SetShaderRegister(words, 0x208, 0x00010001u);
    SetShaderRegister(words, 0x209, 0x00010001u);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, 4u << 1u);
    std::uint32_t offset = 0x240;
    for (const auto data : {Low(slot.table.data()), High(slot.table.data()), Low(slot.outputDescriptor.data()), High(slot.outputDescriptor.data())}) SetShaderRegister(words, offset++, data);
    words.insert(words.end(), {0xc0031500u, 2u, 1u, 1u, 0x8043u});
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(&slot.done), High(&slot.done), 1u});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x28, &packet) == 0, "the partial-group dispatch submit failed");
}

void SetVariable(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

}

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() / ("anyps5-partial-groups-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        SetVariable("ANYPS5_NO_SHADER_CACHE", "");
        SetVariable("ANYPS5_SHADER_CACHE_DIR", directory.string());
        {
            auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        SubmitPartial(Partial, 0x4321u);
        const auto start = std::chrono::steady_clock::now();
        while (Partial.done == 0) {
            Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(60), "the partial-group dispatch did not finish");
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        Require(Partial.output[0] == 0x4321u, "the partial-group dispatch stored " + std::to_string(Partial.output[0]));
        AgcDriverWaitIdle_nid_postfix();
        AgcDriverShutdown_nid_postfix();
        std::ifstream file(directory / "shader-use.bin", std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto key = AgcDriver::DriverDetail::PartialGroupKey(ReaderCode);
        bool recorded = false;
        for (std::size_t offset = 0; offset + 16u <= bytes.size(); offset += 16u) {
            std::uint64_t stored = 0;
            std::memcpy(&stored, bytes.data() + offset, sizeof(stored));
            recorded |= stored == key;
        }
        Require(recorded, "a shader dispatched with partial thread groups was not recorded for preparation ahead of use");
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        std::cout << "partial thread group dispatch recorded\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}

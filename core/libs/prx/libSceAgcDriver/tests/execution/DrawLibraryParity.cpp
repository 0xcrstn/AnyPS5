#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
constexpr std::byte Kept{0x40};
using Image = std::array<std::byte, 1024>;
using Frames = std::vector<Image>;
alignas(256) Image Pixels{};
alignas(256) constexpr std::array<std::uint32_t, 16> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e0002f2u, 0x7e0202f0u, 0x7e040280u, 0x7e0602f2u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 13> SampledPixelCode{
    0xbe8c047eu, 0xbefe0a7eu, 0x100000ffu, 0x3c800000u, 0x100202ffu, 0x3d000000u, 0xf0800f08u, 0x00400200u,
    0xbf8c3f70u, 0x87fe0c7eu, 0xf800180fu, 0x05040302u, 0xbf810000u,
};
constexpr std::uint32_t TextureWidth = 64;
constexpr std::uint32_t TextureHeight = 32;
constexpr std::uint32_t Format8888Unorm = 56;
constexpr std::uint32_t Type2D = 9;
alignas(4096) std::array<std::byte, TextureWidth * TextureHeight * 4u> Texels{};

struct Swizzle {
    std::uint32_t swap;
    std::uint32_t textureSelect;
};

constexpr std::uint32_t Identity = 4u | (5u << 3u) | (6u << 6u) | (7u << 9u);
constexpr std::uint32_t Reversed = 6u | (5u << 3u) | (4u << 6u) | (7u << 9u);
constexpr std::uint32_t RedZero = 0u | (5u << 3u) | (6u << 6u) | (7u << 9u);
constexpr std::uint32_t RedOne = 1u | (4u << 3u) | (4u << 6u) | (7u << 9u);
constexpr std::array<Swizzle, 8> Draws{{{0u, 0u}, {0u, 0u}, {1u, 0u}, {0u, 0u}, {2u, 0u}, {3u, 0u}, {1u, 0u}, {0u, 0u}}};
constexpr std::array<Swizzle, 9> SampledDraws{{{0u, Identity}, {0u, Identity}, {0u, Reversed}, {0u, Identity}, {0u, RedZero}, {0u, RedOne}, {1u, RedZero}, {0u, Reversed}, {0u, Identity}}};

std::array<std::uint32_t, 8> TextureDescriptor(std::uint32_t select) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Texels.data()));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format8888Unorm << 20u) | (((TextureWidth - 1u) & 3u) << 30u),
        ((TextureWidth - 1u) >> 2u) | ((TextureHeight - 1u) << 14u),
        select | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
}

void FillTexture() {
    const auto mips = AgcDriver::Graphics::ComputeMipLayout(AgcDriver::Graphics::TextureTileMode::kLinear, Format8888Unorm, TextureWidth, TextureHeight, 1u);
    Require(AgcDriver::Graphics::ComputeSurfaceSize(mips, 1) <= Texels.size(), "the parity texture does not fit its storage");
    constexpr std::array<std::uint8_t, 4> color{0x20u, 0x80u, 0xc0u, 0xffu};
    for (std::uint64_t offset = mips[0].tiledOffset; offset < mips[0].tiledOffset + mips[0].tiledSize; offset += 4u) std::memcpy(Texels.data() + offset, color.data(), color.size());
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

Image Draw(const Swizzle& swizzle, bool sampled) {
    Pixels.fill(Kept);
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = sampled ? reinterpret_cast<std::uintptr_t>(SampledPixelCode.data()) : reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028u | (swizzle.swap << 11u)},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
        {0x1b3, sampled ? 0x300u : 0u}, {0x1b4, sampled ? 0x300u : 0u},
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 0u},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, sampled ? 12u << 1u : 0u},
    };
    if (sampled) {
        const auto texture = TextureDescriptor(swizzle.textureSelect);
        const std::array<std::uint32_t, 4> sampler{0u, 0xfffu << 12u, 1u << 26u, 0u};
        for (std::uint32_t index = 0; index < 8u; ++index) shader.emplace_back(0x00cu + index, texture[index]);
        for (std::uint32_t index = 0; index < 4u; ++index) shader.emplace_back(0x014u + index, sampler[index]);
    }
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    const auto color = reinterpret_cast<std::uintptr_t>(Pixels.data());
    Append(commands, 0x69, {0x318, static_cast<std::uint32_t>(color >> 8u)});
    Append(commands, 0x69, {0x390, static_cast<std::uint32_t>(color >> 40u)});
    Append(commands, 0x2d, {3, 2});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the parity draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(color, Pixels.size());
    return Pixels;
}

Frames Render() {
    FillTexture();
    Frames frames;
    for (const auto& swizzle : Draws) frames.push_back(Draw(swizzle, false));
    for (const auto& swizzle : SampledDraws) frames.push_back(Draw(swizzle, true));
    AgcDriverShutdown_nid_postfix();
    return frames;
}

std::string Describe(const Image& frame) {
    const auto drawn = std::find_if(frame.begin(), frame.end(), [](std::byte value) { return value != Kept; });
    const auto offset = drawn == frame.end() ? 0u : static_cast<std::size_t>(drawn - frame.begin()) & ~std::size_t{3};
    char text[64];
    std::snprintf(text, sizeof(text), "%02x %02x %02x %02x at byte %zu", static_cast<unsigned>(frame[offset]), static_cast<unsigned>(frame[offset + 1]), static_cast<unsigned>(frame[offset + 2]), static_cast<unsigned>(frame[offset + 3]), offset);
    return text;
}

void Write(const std::filesystem::path& path, const Frames& frames) {
    std::ofstream file(path, std::ios::binary);
    for (const auto& frame : frames) file.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    Require(static_cast<bool>(file), "the whole-pipeline frames could not be written");
}

Frames Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Frames frames;
    Image frame{};
    while (file.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()))) frames.push_back(frame);
    return frames;
}

void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value == '\0') unsetenv(name);
    else setenv(name, value, 1);
#endif
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--whole") {
            Require(std::getenv("APS5_NO_GPL") != nullptr, "the whole-pipeline render runs without APS5_NO_GPL");
            Write(argv[2], Render());
            return 0;
        }
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if (!device->GraphicsPipelineLibraries()) return VulkanTestSkipped;
        }
        const auto output = std::filesystem::temp_directory_path() / ("anyps5-draw-library-parity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
        SetVariable("APS5_NO_GPL", "1");
        const auto command = "\"\"" + std::string(argv[0]) + "\" --whole \"" + output.string() + "\"\"";
        Require(std::system(command.c_str()) == 0, "the whole-pipeline render failed");
        SetVariable("APS5_NO_GPL", "");
        const auto whole = Read(output);
        std::error_code error;
        std::filesystem::remove(output, error);
        const auto libraries = Render();
        Require(libraries.size() == Draws.size() + SampledDraws.size() && whole.size() == libraries.size(), "a render made the wrong number of frames");
        for (std::size_t index = 0; index < libraries.size(); ++index) {
            Require(std::count(whole[index].begin(), whole[index].end(), Kept) != static_cast<std::ptrdiff_t>(whole[index].size()), "draw " + std::to_string(index) + " wrote no pixel with whole pipelines");
            Require(libraries[index] == whole[index], "draw " + std::to_string(index) + " differs between graphics pipeline libraries (" + Describe(libraries[index]) + ") and whole pipelines (" + Describe(whole[index]) + ")");
        }
        Require(libraries[0] != libraries[2] && libraries[2] != libraries[4], "the component swaps did not change the stored color: " + Describe(libraries[0]) + ", " + Describe(libraries[2]) + ", " + Describe(libraries[4]));
        const auto sampled = Draws.size();
        Require(libraries[sampled] != libraries[sampled + 2u] && libraries[sampled] != libraries[sampled + 4u] && libraries[sampled + 4u] != libraries[sampled + 5u], "the texture swizzles did not change the stored color: " + Describe(libraries[sampled]) + ", " + Describe(libraries[sampled + 2u]) + ", " + Describe(libraries[sampled + 4u]) + ", " + Describe(libraries[sampled + 5u]));
        std::cout << "draws with graphics pipeline libraries match whole pipelines: " << Describe(libraries[0]) << ", " << Describe(libraries[2]) << ", " << Describe(libraries[4]) << ", " << Describe(libraries[5]) << "; sampled " << Describe(libraries[sampled]) << ", " << Describe(libraries[sampled + 2u]) << ", " << Describe(libraries[sampled + 4u]) << ", " << Describe(libraries[sampled + 5u]) << ", " << Describe(libraries[sampled + 6u]) << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

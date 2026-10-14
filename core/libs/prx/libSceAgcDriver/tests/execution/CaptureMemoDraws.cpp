#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 32;
constexpr std::size_t Block = 65536;
constexpr std::size_t ColorBytes = Width * Height * 4;
constexpr std::uint32_t SetContextReg = 0x69u;
constexpr std::uint32_t SetShReg = 0x76u;
constexpr std::uint32_t SetUconfigReg = 0x79u;

alignas(256) constexpr std::array<std::uint32_t, 12> VertexCode{
    0xf4080304u, 0xfa000000u, 0xbf8cc07fu, 0xe0382000u, 0x80030005u, 0xbf8c3f70u,
    0xf80008cfu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 12> LoadPixelCode{
    0xf4000100u, 0xfa000000u, 0xbf8cc07fu, 0x7e000204u, 0x7e020280u, 0x7e040280u,
    0x7e0602f2u, 0xf800080fu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 16> PointerPixelCode{
    0xf4040080u, 0xfa000000u, 0xbf8cc07fu, 0xf4000101u, 0xfa000000u, 0xbf8cc07fu, 0x7e000204u, 0x7e020280u,
    0x7e040280u, 0x7e0602f2u, 0xf800080fu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};

alignas(16) std::array<std::uint32_t, 4> PointerTable{};
alignas(16) std::array<std::array<float, 4>, 4> Vertices{{{-3.0f, -1.0f, 0.5f, 1.0f}, {1.0f, -1.0f, 0.5f, 1.0f}, {1.0f, 3.0f, 0.5f, 1.0f}, {1.0f, 3.0f, 0.5f, 1.0f}}};
alignas(16) std::array<std::uint32_t, 4> VertexTable{};
alignas(16) std::array<std::array<float, 4>, 6> Blocks{};
alignas(64) volatile std::uint32_t EndOfPipe = 0;
std::byte* ColorMemory = nullptr;

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header LoadPixelHeader{};
alignas(8) Header PointerPixelHeader{};

std::byte* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    return static_cast<std::byte*>(block);
}

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t records, std::uint32_t format) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, format};
}

void SetRegister(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    words.push_back(0xc0010000u | (opcode << 8u));
    words.push_back(offset);
    words.push_back(value);
}

const std::vector<std::pair<std::uint32_t, std::uint32_t>> ContextBaseline{
    {0x2d5, 0x2000},
    {0x1b6, 0}, {0x200, 0}, {0x203, 0x800}, {0x207, 0},
    {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
    {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
    {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
    {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
    {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
    {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
    {0x31b, 0}, {0x31d, 0}, {0x3b8, 0x9000000}, {0x1e0, 0},
    {0xc, 0},
    {0x81, 0x80000000}, {0x90, 0x80000000}, {0x94, 0x80000000},
    {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0x113, std::bit_cast<std::uint32_t>(1.0f)},
};

std::vector<std::uint32_t> Frame(const std::uint32_t* pixelCode, const void* pixelPointer) {
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(ColorMemory);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixelAddress = reinterpret_cast<std::uintptr_t>(pixelCode);
    const auto vertexTable = reinterpret_cast<std::uintptr_t>(VertexTable.data());
    const auto pixelTable = reinterpret_cast<std::uintptr_t>(pixelPointer);
    std::vector<std::uint32_t> words;
    for (const auto& [offset, value] : ContextBaseline) SetRegister(words, SetContextReg, offset, value);
    SetRegister(words, SetContextReg, 0x318, static_cast<std::uint32_t>(colorAddress >> 8u));
    SetRegister(words, SetContextReg, 0x390, static_cast<std::uint32_t>(colorAddress >> 40u));
    SetRegister(words, SetContextReg, 0x3b0, ((Width - 1u) << 14u) | (Height - 1u));
    SetRegister(words, SetContextReg, 0x10f, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x110, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x111, std::bit_cast<std::uint32_t>(-(Height / 2.0f)));
    SetRegister(words, SetContextReg, 0x112, std::bit_cast<std::uint32_t>(Height / 2.0f));
    const auto extent = (Height << 16u) | Width;
    SetRegister(words, SetContextReg, 0xd, extent);
    SetRegister(words, SetContextReg, 0x82, extent);
    SetRegister(words, SetContextReg, 0x91, extent);
    SetRegister(words, SetContextReg, 0x95, extent);
    SetRegister(words, SetUconfigReg, 0x242, 4u);
    SetRegister(words, SetShReg, 0x008, static_cast<std::uint32_t>(pixelAddress >> 8u));
    SetRegister(words, SetShReg, 0x009, static_cast<std::uint32_t>(pixelAddress >> 40u));
    SetRegister(words, SetShReg, 0x00b, 2u << 1u);
    SetRegister(words, SetShReg, 0x00c, static_cast<std::uint32_t>(pixelTable));
    SetRegister(words, SetShReg, 0x00d, static_cast<std::uint32_t>(pixelTable >> 32u));
    SetRegister(words, SetShReg, 0x0c8, static_cast<std::uint32_t>(vertexAddress >> 8u));
    SetRegister(words, SetShReg, 0x0c9, static_cast<std::uint32_t>(vertexAddress >> 40u));
    SetRegister(words, SetShReg, 0x08b, 2u << 1u);
    SetRegister(words, SetShReg, 0x08c, static_cast<std::uint32_t>(vertexTable));
    SetRegister(words, SetShReg, 0x08d, static_cast<std::uint32_t>(vertexTable >> 32u));
    words.insert(words.end(), {0xc0012d00u, 3u, 2u});
    const auto labelAddress = reinterpret_cast<std::uintptr_t>(&EndOfPipe);
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(labelAddress), static_cast<std::uint32_t>(static_cast<std::uint64_t>(labelAddress) >> 32u), 1u, 0u, 0u});
    return words;
}

bool MemoEnabled() {
    return std::getenv("APS5_NO_CAPTURE_MEMO") == nullptr;
}

void Draw(const std::uint32_t* pixelCode, const void* pixelPointer, std::uint32_t expectedRed, std::uint64_t served, const std::string& what) {
    std::fill_n(ColorMemory, ColorBytes, std::byte{0});
    EndOfPipe = 0;
    const auto before = AgcDriver::ShaderMemory::MemoizedCaptures();
    auto words = Frame(pixelCode, pixelPointer);
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, what + ": the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    Require(EndOfPipe == 1u, what + ": the end-of-pipe label was not written");
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    const auto center = ((Height / 2u) * Width + Width / 2u) * 4u;
    const auto red = std::to_integer<std::uint32_t>(colors[center]);
    Require(red + 1u >= expectedRed && red <= expectedRed + 1u, what + ": the centre pixel's red is " + std::to_string(red) + ", expected " + std::to_string(expectedRed));
    const auto memoized = AgcDriver::ShaderMemory::MemoizedCaptures() - before;
    const auto expected = MemoEnabled() ? served : 0u;
    Require(memoized == expected, what + ": " + std::to_string(memoized) + " of the draw's captures were served from the memo, expected " + std::to_string(expected));
}

void SetPointer(std::size_t block) {
    const auto address = reinterpret_cast<std::uintptr_t>(Blocks[block].data());
    PointerTable = {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 0u, 0u};
}

void LoadedDataPatched() {
    Blocks[0] = {1.0f, 0.0f, 0.0f, 0.0f};
    Draw(LoadPixelCode.data(), Blocks[0].data(), 255, 0, "a pixel shader exporting a loaded value");
    Draw(LoadPixelCode.data(), Blocks[0].data(), 255, 2, "the loaded value drawn again");
    Blocks[0][0] = 0.5f;
    Draw(LoadPixelCode.data(), Blocks[0].data(), 128, 2, "the loaded value changed in place");
    Blocks[0][0] = 0.25f;
    Draw(LoadPixelCode.data(), Blocks[0].data(), 64, 2, "the loaded value changed in place again");
}

void MovedUserPointerPatched() {
    Blocks[1] = {1.0f, 0.0f, 0.0f, 0.0f};
    Blocks[2] = {0.5f, 0.0f, 0.0f, 0.0f};
    Draw(LoadPixelCode.data(), Blocks[1].data(), 255, 2, "the user pointer moved to a new block with the same value");
    Draw(LoadPixelCode.data(), Blocks[2].data(), 128, 2, "the user pointer moved to a block with another value");
    Draw(LoadPixelCode.data(), Blocks[1].data(), 255, 2, "the user pointer moved back");
    Blocks[2][0] = 0.25f;
    Draw(LoadPixelCode.data(), Blocks[2].data(), 64, 2, "the user pointer moved to a block whose value changed meanwhile");
}

void MovedBaseWordFollowed() {
    Blocks[3] = {1.0f, 0.0f, 0.0f, 0.0f};
    Blocks[4] = {0.5f, 0.0f, 0.0f, 0.0f};
    Blocks[5] = {1.0f, 0.0f, 0.0f, 0.0f};
    SetPointer(3);
    Draw(PointerPixelCode.data(), PointerTable.data(), 255, 1, "a pixel shader loading through a pointer read from its table");
    SetPointer(4);
    Draw(PointerPixelCode.data(), PointerTable.data(), 128, 2, "the table's pointer moved to a block with another value");
    SetPointer(5);
    Draw(PointerPixelCode.data(), PointerTable.data(), 255, 2, "the table's pointer moved to a block with the first value");
    Blocks[5][0] = 0.25f;
    Draw(PointerPixelCode.data(), PointerTable.data(), 64, 2, "the value behind the moved pointer changed in place");
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        ColorMemory = AllocateWatched(Block);
        VertexTable = BufferDescriptor(Vertices.data(), sizeof(Vertices[0]), 3u, 0x01016facu);
        Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(LoadPixelHeader, LoadPixelCode.data(), sizeof(LoadPixelCode), 1);
        Register(PointerPixelHeader, PointerPixelCode.data(), sizeof(PointerPixelCode), 1);
        LoadedDataPatched();
        MovedUserPointerPatched();
        MovedBaseWordFollowed();
        AgcDriverShutdown_nid_postfix();
        std::cout << "Memoized captures bind the current data with the memo " << (MemoEnabled() ? "on" : "off") << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}

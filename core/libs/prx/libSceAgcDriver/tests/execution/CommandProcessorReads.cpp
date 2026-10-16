#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
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
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::CommandProcessorReads;

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

alignas(256) constexpr std::array<std::uint32_t, 16> BufferPixelCode{
    0xf4080100u, 0xfa000000u, 0xbf8cc07fu, 0xf4200202u, 0xfa000000u, 0xbf8cc07fu, 0x7e000208u, 0x7e020280u,
    0x7e040280u, 0x7e0602f2u, 0xf800080fu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};

alignas(16) std::array<std::array<float, 4>, 4> Vertices{{{-3.0f, -1.0f, 0.5f, 1.0f}, {1.0f, -1.0f, 0.5f, 1.0f}, {1.0f, 3.0f, 0.5f, 1.0f}, {1.0f, 3.0f, 0.5f, 1.0f}}};
alignas(16) std::array<std::uint32_t, 4> VertexTable{};
alignas(16) std::array<std::uint32_t, 4> FullTable{};
alignas(16) std::array<std::uint32_t, 4> HalfTable{};
alignas(16) std::array<float, 4> Full{1.0f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<float, 4> Half{0.5f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<std::uint32_t, 4> RegisterPairs{};
alignas(64) volatile std::uint32_t EndOfPipe = 0;
std::byte* ColorMemory = nullptr;

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header BufferPixelHeader{};

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

std::vector<std::uint32_t> Frame() {
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(ColorMemory);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixelAddress = reinterpret_cast<std::uintptr_t>(BufferPixelCode.data());
    const auto vertexTable = reinterpret_cast<std::uintptr_t>(VertexTable.data());
    const auto fullTable = reinterpret_cast<std::uintptr_t>(FullTable.data());
    const auto pairs = reinterpret_cast<std::uintptr_t>(RegisterPairs.data());
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
    SetRegister(words, SetShReg, 0x00c, static_cast<std::uint32_t>(fullTable));
    SetRegister(words, SetShReg, 0x00d, static_cast<std::uint32_t>(fullTable >> 32u));
    SetRegister(words, SetShReg, 0x0c8, static_cast<std::uint32_t>(vertexAddress >> 8u));
    SetRegister(words, SetShReg, 0x0c9, static_cast<std::uint32_t>(vertexAddress >> 40u));
    SetRegister(words, SetShReg, 0x08b, 2u << 1u);
    SetRegister(words, SetShReg, 0x08c, static_cast<std::uint32_t>(vertexTable));
    SetRegister(words, SetShReg, 0x08d, static_cast<std::uint32_t>(vertexTable >> 32u));
    words.insert(words.end(), {0xc0012d00u, 3u, 2u});
    words.insert(words.end(), {0xc0001000u, 0u});
    words.insert(words.end(), {0xc0004600u, 0x407u});
    words.insert(words.end(), {0xc0036300u, static_cast<std::uint32_t>(pairs), static_cast<std::uint32_t>(static_cast<std::uint64_t>(pairs) >> 32u), 0x80000000u, 2u});
    words.insert(words.end(), {0xc0012d00u, 3u, 2u});
    const auto labelAddress = reinterpret_cast<std::uintptr_t>(&EndOfPipe);
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(labelAddress), static_cast<std::uint32_t>(static_cast<std::uint64_t>(labelAddress) >> 32u), 1u, 0u, 0u});
    return words;
}

std::uint32_t CentreRed(const std::string& what) {
    std::fill_n(ColorMemory, ColorBytes, std::byte{0});
    EndOfPipe = 0;
    auto words = Frame();
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, what + ": the submission failed");
    AgcDriverWaitIdle_nid_postfix();
    Require(EndOfPipe == 1u, what + ": the end-of-pipe label was not written");
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    const auto center = ((Height / 2u) * Width + Width / 2u) * 4u;
    return std::to_integer<std::uint32_t>(colors[center]);
}

void PointPairsAt(const void* table) {
    const auto address = reinterpret_cast<std::uintptr_t>(table);
    RegisterPairs = {0x00cu, static_cast<std::uint32_t>(address), 0x00du, static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u)};
}

void ParsesTheSwitch() {
    using AgcDriver::DriverDetail::ParseCommandProcessorReads;
    Require(ParseCommandProcessorReads(nullptr) == CommandProcessorReads::Overlap, "an unset APS5_CP_READS_OVERLAP does not overlap");
    Require(ParseCommandProcessorReads("0") == CommandProcessorReads::Drain && ParseCommandProcessorReads("off") == CommandProcessorReads::Drain, "APS5_CP_READS_OVERLAP=0 or off does not drain");
    Require(ParseCommandProcessorReads("1") == CommandProcessorReads::Overlap && ParseCommandProcessorReads("on") == CommandProcessorReads::Overlap, "APS5_CP_READS_OVERLAP=1 or on does not overlap");
    Require(ParseCommandProcessorReads("verify") == CommandProcessorReads::Verify, "APS5_CP_READS_OVERLAP=verify does not verify");
    bool refused = false;
    try {
        static_cast<void>(ParseCommandProcessorReads("2"));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    Require(refused, "APS5_CP_READS_OVERLAP=2 was accepted");
}

void DescribesTheReads() {
    const auto pairs = reinterpret_cast<std::uintptr_t>(RegisterPairs.data());
    const std::array<std::uint32_t, 5> paired{0xc0036300u, static_cast<std::uint32_t>(pairs), static_cast<std::uint32_t>(static_cast<std::uint64_t>(pairs) >> 32u), 0x80000000u, 2u};
    const auto pairRead = AgcDriver::Pm4::CommandProcessorReadOf(paired);
    Require(pairRead.has_value() && pairRead->address == pairs && pairRead->bytes == 16u, "a paired SET_SH_REG_INDIRECT does not read its 2 offset and value pairs");
    const std::array<std::uint32_t, 5> values{0xc0039f00u, 0x1000u, 0x2u, 0x10u, 3u};
    const auto valueRead = AgcDriver::Pm4::CommandProcessorReadOf(values);
    Require(valueRead.has_value() && valueRead->address == 0x200001000ull && valueRead->bytes == 12u, "a SET_CONTEXT_REG_INDIRECT with a base offset does not read its 3 values");
    const std::array<std::uint32_t, 4> attributes{0xc0029100u, 0x1000u, 0u, 2u};
    const auto attributeRead = AgcDriver::Pm4::CommandProcessorReadOf(attributes);
    Require(attributeRead.has_value() && attributeRead->address == 0x1020u && attributeRead->bytes == 16u, "INDEX_ATTRIBUTES_INDIRECT does not read its 16-byte record");
    const std::array<std::uint32_t, 6> copy{0xc0044000u, 0x10001u, 0x1000u, 2u, 0xa010u, 0u};
    const auto copyRead = AgcDriver::Pm4::CommandProcessorReadOf(copy);
    Require(copyRead.has_value() && copyRead->address == 0x200001000ull && copyRead->bytes == 8, "COPY_DATA to context registers does not describe its source memory");
    const std::array<std::uint32_t, 3> draw{0xc0012d00u, 3u, 2u};
    Require(!AgcDriver::Pm4::CommandProcessorReadOf(draw).has_value(), "DRAW_INDEX_AUTO was taken for a command processor read");
}

void DrawsSeeTheIndirectRegisters(CommandProcessorReads mode, const std::string& name) {
    AgcDriver::DriverDetail::SetCommandProcessorReadsMode(mode);
    const auto before = AgcDriver::DriverDetail::CommandProcessorReadTotals();
    PointPairsAt(HalfTable.data());
    const auto half = CentreRed(name + ": a draw after SET_SH_REG_INDIRECT to the half table");
    Require(half + 1u >= 128u && half <= 129u, name + ": the draw after SET_SH_REG_INDIRECT wrote red " + std::to_string(half) + ", expected 128 from the indirectly loaded table");
    PointPairsAt(FullTable.data());
    const auto full = CentreRed(name + ": a draw after SET_SH_REG_INDIRECT to the full table");
    Require(full >= 254u, name + ": the draw after SET_SH_REG_INDIRECT wrote red " + std::to_string(full) + ", expected 255 after the CPU changed the pairs");
    const auto after = AgcDriver::DriverDetail::CommandProcessorReadTotals();
    if (mode == CommandProcessorReads::Drain) {
        Require(after.reads == before.reads && after.registerState == before.registerState && after.syncsDeferred == before.syncsDeferred, name + ": a drained SET_SH_REG_INDIRECT, NOP or EVENT_WRITE was counted as taken without a drain");
        return;
    }
    Require(after.syncDrains - before.syncDrains == after.syncsDeferred - before.syncsDeferred && after.syncsDeferred - before.syncsDeferred <= 2u, name + ": " + std::to_string(after.syncsDeferred - before.syncsDeferred) + " EVENT_WRITE syncs were deferred but " + std::to_string(after.syncDrains - before.syncDrains) + " drained at the next SET_SH_REG_INDIRECT");
    Require(after.reads - before.reads + after.syncDrains - before.syncDrains + after.unknownDrains - before.unknownDrains == 2u, name + ": indirect reads were not accounted for as overlap, deferred sync or unknown-writer drain");
    Require(after.registerState == before.registerState + 2u, name + ": " + std::to_string(after.registerState - before.registerState) + " NOP packets taken without a drain, expected 2");
    if (mode == CommandProcessorReads::Verify) Require(after.verified - before.verified == after.ahead - before.ahead, name + ": " + std::to_string(after.verified - before.verified) + " of " + std::to_string(after.ahead - before.ahead) + " reads ahead of pending records were verified");
    else Require(after.verified == before.verified, name + ": reads were verified with verification off");
}

void SnapshotsSurviveCpuReuseAndObserveDriverStores() {
    using AgcDriver::DriverDetail::RegisterListSnapshot;
    const auto address = reinterpret_cast<std::uintptr_t>(RegisterPairs.data());
    const std::array<std::uint32_t, 5> packet{0xc0036300u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u), 0x80000000u, 2u};
    RegisterPairs = {0x00cu, 11u, 0x00du, 22u};
    const auto original = RegisterPairs;
    const auto captured = RegisterListSnapshot::Capture(packet);
    RegisterPairs[1] = 33u;
    Require(captured.Resolve(packet) == std::vector<std::uint32_t>(original.begin(), original.end()), "CPU reuse after Submit replaced the captured register list");
    alignas(16) std::array<std::uint32_t, 4> unrelated{};
    AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(unrelated.data()), sizeof(unrelated), AgcDriver::GuestMemory::WriteExtent::Exact);
    Require(captured.Resolve(packet) == captured.values, "an unrelated driver store discarded the register-list snapshot");
    const std::array<std::uint32_t, 4> written{0x00cu, 44u, 0x00du, 55u};
    std::thread producer([&] {
        AgcDriver::GuestMemory::Write(address, std::as_bytes(std::span(written)), 4);
    });
    producer.join();
    Require(captured.Resolve(packet) == std::vector<std::uint32_t>(written.begin(), written.end()), "a driver store on another thread after the snapshot was ignored");
    const auto beforePending = RegisterListSnapshot::Capture(packet);
    struct FlushReset {
        ~FlushReset() { AgcDriver::GuestMemory::SetFlushHook(nullptr); }
    } flushReset;
    AgcDriver::GuestMemory::SetFlushHook([](std::uint64_t begin, std::size_t bytes) {
        if (begin != reinterpret_cast<std::uintptr_t>(RegisterPairs.data()) || bytes != sizeof(RegisterPairs)) return;
        AgcDriver::GuestMemory::SetFlushHook(nullptr);
        const std::array<std::uint32_t, 4> pending{0x00cu, 66u, 0x00du, 77u};
        AgcDriver::GuestMemory::StoreOwnBytes(begin, std::as_bytes(std::span(pending)));
    });
    Require(beforePending.Resolve(packet) == std::vector<std::uint32_t>({0x00cu, 66u, 0x00du, 77u}), "a pending store published by the consumption flush was ignored");
    const auto beforeOverflow = RegisterListSnapshot::Capture(packet);
    RegisterPairs[1] = 88u;
    for (std::size_t i = 0; i <= 65536; ++i) AgcDriver::GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(unrelated.data()), sizeof(unrelated), AgcDriver::GuestMemory::WriteExtent::Exact);
    Require(beforeOverflow.Resolve(packet) == beforeOverflow.values, "driver-store history eviction discarded the CPU submit-time register-list snapshot");
    const std::array<std::uint32_t, 4> afterOverflow{0x00cu, 99u, 0x00du, 111u};
    AgcDriver::GuestMemory::Write(address, std::as_bytes(std::span(afterOverflow)), 4);
    Require(beforeOverflow.Resolve(packet) == std::vector<std::uint32_t>(afterOverflow.begin(), afterOverflow.end()), "driver-store history eviction lost an active register-list watch");
    std::promise<void> copying;
    auto copyingStarted = copying.get_future();
    std::promise<void> releaseCopy;
    auto mayCopy = releaseCopy.get_future();
    std::thread delayed([&] {
        AgcDriver::GuestMemory::StoreOwnBytes(address, sizeof(RegisterPairs), [&] {
            copying.set_value();
            mayCopy.wait();
            RegisterPairs[1] = 123u;
        });
    });
    copyingStarted.wait();
    auto duringCopy = std::async(std::launch::async, [&] { return RegisterListSnapshot::Capture(packet); });
    const auto captureBlocked = duringCopy.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    releaseCopy.set_value();
    delayed.join();
    const auto afterCopy = duringCopy.get();
    Require(captureBlocked && afterCopy.values[1] == 123u, "a snapshot did not serialize its copy with an unfinished driver store");
    RegisterPairs[1] = 456u;
    Require(afterCopy.Resolve(packet)[1] == 123u, "a serialized snapshot lost its CPU submit-time contents");
    bool refused = false;
    try {
        static_cast<void>(captured.Resolve({}));
    } catch (const std::runtime_error&) {
        refused = true;
    }
    Require(refused, "a malformed register-list packet reused a snapshot");
}

void CowAliasesPreserveFrozenRegisterSnapshots() {
#ifdef _WIN32
    using AgcDriver::DriverDetail::RegisterListSnapshot;
    struct Mapping {
        HANDLE section = nullptr;
        std::byte* memory = nullptr;
        Mapping() {
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Block, nullptr);
            Require(section != nullptr, "COW register snapshot section allocation failed");
            memory = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(3 * Block, Block));
            if (memory == nullptr) {
                CloseHandle(section);
                throw std::runtime_error("COW register snapshot arena allocation failed");
            }
            try {
                GuestArena::GuestArenaMap_nid_postfix(memory, Block, section, 0, PAGE_WRITECOPY);
                GuestArena::GuestArenaMap_nid_postfix(memory + Block, Block, section, 0, PAGE_READWRITE);
                GuestArena::GuestArenaMap_nid_postfix(memory + 2 * Block, Block, section, 0, PAGE_READWRITE);
            } catch (...) {
                Release();
                throw;
            }
        }
        void Release() {
            if (memory != nullptr) {
                GuestArena::GuestArenaReset_nid_postfix(memory, 3 * Block);
                GuestArena::GuestArenaRelease_nid_postfix(memory, 3 * Block);
                memory = nullptr;
            }
            if (section != nullptr) {
                GuestArena::GuestArenaForgetSection_nid_postfix(section);
                CloseHandle(section);
                section = nullptr;
            }
        }
        ~Mapping() { Release(); }
    } mapping;
    const auto packetAt = [](const void* pointer) {
        const auto address = reinterpret_cast<std::uint64_t>(pointer);
        return std::array<std::uint32_t, 5>{0xc0036300u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 0x80000000u, 2u};
    };
    const std::array<std::uint32_t, 4> original{0x00cu, 11u, 0x00du, 22u};
    AgcDriver::GuestMemory::CopyDriverMemory(mapping.memory + Block, original.data(), sizeof(original));
    const auto cowPacket = packetAt(mapping.memory);
    const auto frozen = RegisterListSnapshot::Capture(cowPacket);
    auto* cowWords = reinterpret_cast<volatile std::uint32_t*>(mapping.memory);
    cowWords[1] = 33u;
    const std::array<std::uint32_t, 4> sharedWritten{0x00cu, 44u, 0x00du, 55u};
    AgcDriver::GuestMemory::StoreOwnBytes(reinterpret_cast<std::uint64_t>(mapping.memory + Block), std::as_bytes(std::span(sharedWritten)));
    Require(cowWords[1] == 33u && frozen.values == std::vector<std::uint32_t>(original.begin(), original.end()), "a shared driver write changed detached COW or frozen register words");
    const auto refusesUnknown = [&] {
        bool refused = false;
        try { static_cast<void>(frozen.Resolve(cowPacket)); }
        catch (const std::runtime_error& error) { refused = std::string_view(error.what()).find("alias provenance is unknown") != std::string_view::npos; }
        Require(refused, "an uncertain COW driver alias silently refreshed frozen CPU register words");
    };
    refusesUnknown();
    const auto sharedPacket = packetAt(mapping.memory + Block);
    const auto sharedSnapshot = RegisterListSnapshot::Capture(sharedPacket);
    reinterpret_cast<volatile std::uint32_t*>(mapping.memory + Block)[1] = 66u;
    Require(sharedSnapshot.Resolve(sharedPacket) == std::vector<std::uint32_t>(sharedWritten.begin(), sharedWritten.end()), "CPU reuse changed shared submit-time register words");
    const std::array<std::uint32_t, 4> laterWritten{0x00cu, 77u, 0x00du, 88u};
    AgcDriver::GuestMemory::StoreOwnBytes(reinterpret_cast<std::uint64_t>(mapping.memory + 2 * Block), std::as_bytes(std::span(laterWritten)));
    Require(sharedSnapshot.Resolve(sharedPacket) == std::vector<std::uint32_t>(laterWritten.begin(), laterWritten.end()), "a proven shared driver alias failed to refresh register words");
    refusesUnknown();
#endif
}

void SnapshotPayloadsPreserveUnwrittenAndReusedWords() {
    using namespace AgcDriver::GuestMemory;
    using AgcDriver::DriverDetail::RegisterListSnapshot;
    const auto address = reinterpret_cast<std::uintptr_t>(RegisterPairs.data());
    const std::array<std::uint32_t, 5> packet{0xc0036300u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u), 0x80000000u, 2u};
    RegisterPairs = {0x00cu, 11u, 0x00du, 22u};
    const auto frozen = RegisterListSnapshot::Capture(packet);
    RegisterPairs[3] = 777u;
    const std::uint32_t written = 44u;
    StoreOwnBytes(address + sizeof(std::uint32_t), std::as_bytes(std::span(&written, 1)));
    RegisterPairs[1] = 999u;
    Require(frozen.Resolve(packet) == std::vector<std::uint32_t>({0x00cu, 44u, 0x00du, 22u}), "a partial driver payload reread CPU-reused written or untouched words");
    const auto beforeSourceReuse = RegisterListSnapshot::Capture(packet);
    std::uint32_t source = 55u;
    StoreOwnBytes(address + sizeof(std::uint32_t), std::as_bytes(std::span(&source, 1)), [&](std::span<const std::byte> bytes) {
        source = 666u;
        CopyDriverMemory(RegisterPairs.data() + 1, bytes.data(), bytes.size());
    });
    Require(RegisterPairs[1] == 55u && beforeSourceReuse.Resolve(packet)[1] == 55u, "the driver store and retained payload used different source contents");
    bool copiedDirectly = false;
    alignas(8) std::uint64_t unobserved = 0;
    const std::uint64_t source64 = 123u;
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(&unobserved), std::as_bytes(std::span(&source64, 1)), [&](std::span<const std::byte> bytes) {
        copiedDirectly = bytes.data() == reinterpret_cast<const std::byte*>(&source64);
        CopyDriverMemory(&unobserved, bytes.data(), bytes.size());
    });
    Require(copiedDirectly && unobserved == source64, "an unobserved driver store copied its source for snapshot retention");
    std::array<std::byte, 8> labelSaved{};
    const auto labelWatch = ReadDriverSnapshot(reinterpret_cast<std::uintptr_t>(&unobserved), labelSaved, 8);
    AgcDriverStoreGuestLabel_nid_postfix(&unobserved, 456u);
    unobserved = 789u;
    ApplyDriverSnapshot(*labelWatch, labelSaved);
    std::uint64_t labelPayload = 0;
    CopyDriverMemory(&labelPayload, labelSaved.data(), labelSaved.size());
    Require(labelPayload == 456u, "an atomic label payload was replaced by later CPU reuse");
    const auto beforeGpuIntent = RegisterListSnapshot::Capture(packet);
    MarkWritten(address + sizeof(std::uint32_t), sizeof(std::uint32_t), WriteExtent::Exact);
    bool refused = false;
    try { static_cast<void>(beforeGpuIntent.Resolve(packet)); }
    catch (const std::runtime_error& error) { refused = std::string_view(error.what()).find("bounded payload") != std::string_view::npos; }
    Require(refused, "a GPU intent without an actual payload refreshed a frozen register word");
    RegisterPairs = {11u, 22u, 0u, 0u};
    auto contiguous = packet;
    contiguous[3] = 0x00cu;
    const auto contiguousSnapshot = RegisterListSnapshot::Capture(contiguous);
    Require(contiguousSnapshot.Resolve(contiguous) == std::vector<std::uint32_t>({0x00cu, 11u, 0x00du, 22u}), "an unchanged contiguous register snapshot lost its register indices");
    RegisterPairs[1] = 777u;
    StoreOwnBytes(address, std::as_bytes(std::span(&written, 1)));
    RegisterPairs[0] = 999u;
    Require(contiguousSnapshot.Resolve(contiguous) == std::vector<std::uint32_t>({0x00cu, 44u, 0x00du, 22u}), "a contiguous register snapshot mixed payload offsets with parsed register indices");
}

void SnapshotPayloadLimitsRefuseWithoutWidening() {
    using namespace AgcDriver::GuestMemory;
    alignas(8) std::uint64_t word = 0;
    std::array<std::byte, 8> saved{};
    auto watch = ReadDriverSnapshot(reinterpret_cast<std::uintptr_t>(&word), saved, 8);
    const auto beforeRanges = DriverStoreWatch::LimitTotals();
    for (std::uint64_t value = 1; value <= DriverStoreWatch::MaximumRanges; ++value) StoreOwnBytes(reinterpret_cast<std::uintptr_t>(&word), std::as_bytes(std::span(&value, 1)));
    ApplyDriverSnapshot(*watch, saved);
    std::uint64_t last = 0;
    CopyDriverMemory(&last, saved.data(), saved.size());
    Require(last == DriverStoreWatch::MaximumRanges, "the last bounded register payload was not retained");
    const std::uint64_t overflow = 999u;
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(&word), std::as_bytes(std::span(&overflow, 1)));
    Require(watch->State() == DriverStoredSince::Unknown && DriverStoreWatch::LimitTotals().ranges == beforeRanges.ranges + 1, "register payload range overflow did not explicitly refuse and count");
    bool refused = false;
    try { ApplyDriverSnapshot(*watch, saved); }
    catch (const std::runtime_error&) { refused = true; }
    Require(refused, "range overflow silently widened the snapshot refresh");
    std::vector<std::byte> storage(32768, std::byte{1}), source(storage.size(), std::byte{2}), snapshot(storage.size());
    watch = ReadDriverSnapshot(reinterpret_cast<std::uintptr_t>(storage.data()), snapshot);
    const auto beforeBytes = DriverStoreWatch::LimitTotals();
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(storage.data()), source);
    Require(watch->State() == DriverStoredSince::Stored, "one cross-page store retained duplicate payloads for each page");
    source.front() = std::byte{3};
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(storage.data()), source);
    ApplyDriverSnapshot(*watch, snapshot);
    Require(snapshot == source && watch->State() == DriverStoredSince::Stored, "two payloads within the byte limit were not retained");
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(storage.data()), source);
    Require(watch->State() == DriverStoredSince::Unknown && DriverStoreWatch::LimitTotals().bytes == beforeBytes.bytes + 1, "register payload byte overflow did not explicitly refuse and count");
    storage.resize(DriverStoreWatch::MaximumBytes + 8);
    source.resize(storage.size(), std::byte{4});
    snapshot.resize(storage.size());
    watch = ReadDriverSnapshot(reinterpret_cast<std::uintptr_t>(storage.data()), snapshot);
    const auto beforeLarge = DriverStoreWatch::LimitTotals();
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(storage.data()), source);
    Require(storage == source && watch->State() == DriverStoredSince::Unknown && DriverStoreWatch::LimitTotals().bytes == beforeLarge.bytes + 1, "an oversized actual store was truncated into a valid register payload");
}

void SetEnvironment(const char* name) {
#ifdef _WIN32
    Require(_putenv_s(name, "1") == 0, std::string("cannot set ") + name);
#else
    Require(setenv(name, "1", 1) == 0, std::string("cannot set ") + name);
#endif
}

void CpuCopiesRetainRegisterPayloads(AgcDriver::VulkanDevice& device) {
    using AgcDriver::DriverDetail::RegisterListSnapshot;
    const auto address = reinterpret_cast<std::uintptr_t>(RegisterPairs.data());
    const std::array<std::uint32_t, 5> packet{0xc0036300u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u), 0x80000000u, 2u};
    RegisterPairs = {0x00cu, 11u, 0x00du, 22u};
    const auto frozen = RegisterListSnapshot::Capture(packet);
    const std::uint32_t source = 44u;
    RegisterPairs[3] = 777u;
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto outcome = device.CopyBuffer(address + sizeof(std::uint32_t), reinterpret_cast<std::uintptr_t>(&source), sizeof(source), sizeof(source), sizeof(source), 0, 0, 0, [](std::span<const std::byte>, std::uint64_t) {});
    Require(outcome.path == 0 && RegisterPairs[1] == source, "the register payload CPU-copy fixture did not take the actual CPU copy path");
    RegisterPairs[1] = 999u;
    Require(frozen.Resolve(packet) == std::vector<std::uint32_t>({0x00cu, 44u, 0x00du, 22u}), "an actual CPU DMA copy lost its payload or reread CPU-reused register words");
}

}

int main() {
    try {
        ParsesTheSwitch();
        DescribesTheReads();
        SnapshotsSurviveCpuReuseAndObserveDriverStores();
        CowAliasesPreserveFrozenRegisterSnapshots();
        SnapshotPayloadsPreserveUnwrittenAndReusedWords();
        SnapshotPayloadLimitsRefuseWithoutWidening();
        SetEnvironment("APS5_RECORD_THREAD");
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            CpuCopiesRetainRegisterPayloads(*device);
        }
        ColorMemory = AllocateWatched(Block);
        VertexTable = BufferDescriptor(Vertices.data(), sizeof(Vertices[0]), 3u, 0x01016facu);
        FullTable = BufferDescriptor(Full.data(), 0, 16u, 0x31016facu);
        HalfTable = BufferDescriptor(Half.data(), 0, 16u, 0x31016facu);
        Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(BufferPixelHeader, BufferPixelCode.data(), sizeof(BufferPixelCode), 1);
        DrawsSeeTheIndirectRegisters(CommandProcessorReads::Drain, "drained");
        DrawsSeeTheIndirectRegisters(CommandProcessorReads::Overlap, "overlapped");
        DrawsSeeTheIndirectRegisters(CommandProcessorReads::Verify, "verified");
        AgcDriverShutdown_nid_postfix();
        std::cout << "Draws on the record thread see registers loaded by SET_SH_REG_INDIRECT whether queue 0 drains, overlaps or verifies the read" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}

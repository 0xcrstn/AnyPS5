#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
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

alignas(256) constexpr std::array<std::uint32_t, 16> BufferPixelCode{
    0xf4080100u, 0xfa000000u, 0xbf8cc07fu, 0xf4200202u, 0xfa000000u, 0xbf8cc07fu, 0x7e000208u, 0x7e020280u,
    0x7e040280u, 0x7e0602f2u, 0xf800080fu, 0x03020100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
};

alignas(16) std::array<std::array<float, 4>, 4> Vertices{{{-3.0f, -1.0f, 0.5f, 1.0f}, {1.0f, -1.0f, 0.5f, 1.0f}, {1.0f, 3.0f, 0.5f, 1.0f}, {1.0f, 3.0f, 0.5f, 1.0f}}};
alignas(16) std::array<std::uint32_t, 4> VertexTable{};
alignas(16) std::array<std::uint32_t, 4> FullTable{};
alignas(16) std::array<std::uint32_t, 4> ValueTable{};
alignas(16) std::array<float, 4> Full{1.0f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<float, 12> Values{};
alignas(16) volatile std::uint32_t Condition = 1;
bool NopBeforeCondition = false;
bool WideStore = false;
bool TableWrite = false;
int PredicateValue = -1;
alignas(16) volatile std::uint32_t Predicate = 0;
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

void SetPixelTable(std::vector<std::uint32_t>& words, const void* table) {
    const auto address = reinterpret_cast<std::uintptr_t>(table);
    SetRegister(words, SetShReg, 0x00c, static_cast<std::uint32_t>(address));
    SetRegister(words, SetShReg, 0x00d, static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u));
}

void ReleaseMemory(std::vector<std::uint32_t>& words, const volatile void* target, std::uint32_t value, std::uint32_t interrupt) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (interrupt << 24u), static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32u), value, 0u, 0u});
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

std::vector<std::uint32_t> Frame(bool writeData) {
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(ColorMemory);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixelAddress = reinterpret_cast<std::uintptr_t>(BufferPixelCode.data());
    const auto vertexTable = reinterpret_cast<std::uintptr_t>(VertexTable.data());
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
    SetPixelTable(words, FullTable.data());
    SetRegister(words, SetShReg, 0x0c8, static_cast<std::uint32_t>(vertexAddress >> 8u));
    SetRegister(words, SetShReg, 0x0c9, static_cast<std::uint32_t>(vertexAddress >> 40u));
    SetRegister(words, SetShReg, 0x08b, 2u << 1u);
    SetRegister(words, SetShReg, 0x08c, static_cast<std::uint32_t>(vertexTable));
    SetRegister(words, SetShReg, 0x08d, static_cast<std::uint32_t>(vertexTable >> 32u));
    words.insert(words.end(), {0xc0012d00u, 3u, 2u});
    if (WideStore) {
        const auto values = reinterpret_cast<std::uintptr_t>(Values.data());
        words.insert(words.end(), {0xc00b3700u, 0x500u, static_cast<std::uint32_t>(values), static_cast<std::uint32_t>(static_cast<std::uint64_t>(values) >> 32u)});
        for (std::size_t word = 0; word < 9; ++word) words.push_back(word == 0 ? std::bit_cast<std::uint32_t>(0.5f) : 0u);
    } else {
        ReleaseMemory(words, Values.data(), std::bit_cast<std::uint32_t>(0.5f), 0u);
    }
    if (writeData) {
        const auto condition = reinterpret_cast<std::uintptr_t>(&Condition);
        words.insert(words.end(), {0xc0033700u, 0x500u, static_cast<std::uint32_t>(condition), static_cast<std::uint32_t>(static_cast<std::uint64_t>(condition) >> 32u), 0u});
        if (NopBeforeCondition) words.insert(words.end(), {0xc0001000u, 0u});
        words.insert(words.end(), {0xc0032200u, static_cast<std::uint32_t>(condition), static_cast<std::uint32_t>(static_cast<std::uint64_t>(condition) >> 32u), 0u, 9u});
    }
    if (TableWrite) {
        const auto value = reinterpret_cast<std::uintptr_t>(ValueTable.data());
        const auto table = reinterpret_cast<std::uintptr_t>(RegisterPairs.data());
        words.insert(words.end(), {0xc0063700u, 0x500u, static_cast<std::uint32_t>(table), static_cast<std::uint32_t>(static_cast<std::uint64_t>(table) >> 32u), 0x00cu, static_cast<std::uint32_t>(value), 0x00du, static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) >> 32u)});
        words.insert(words.end(), {0xc0036300u, static_cast<std::uint32_t>(table), static_cast<std::uint32_t>(static_cast<std::uint64_t>(table) >> 32u), 0x80000000u, 2u});
    } else {
        SetPixelTable(words, ValueTable.data());
    }
    if (PredicateValue >= 0) {
        const auto predicate = reinterpret_cast<std::uintptr_t>(&Predicate);
        words.insert(words.end(), {0xc0033700u, 0x500u, static_cast<std::uint32_t>(predicate), static_cast<std::uint32_t>(static_cast<std::uint64_t>(predicate) >> 32u), static_cast<std::uint32_t>(PredicateValue)});
        words.insert(words.end(), {0xc0022000u, (4u << 16u) | 0x100u, static_cast<std::uint32_t>(predicate), static_cast<std::uint32_t>(static_cast<std::uint64_t>(predicate) >> 32u)});
        words.insert(words.end(), {0xc0012d01u, 3u, 2u});
        words.insert(words.end(), {0xc0022000u, 0u, 0u, 0u});
    } else {
        words.insert(words.end(), {0xc0012d00u, 3u, 2u});
    }
    ReleaseMemory(words, &EndOfPipe, 1u, 2u);
    return words;
}

std::uint32_t CentreRed(const std::string& what, bool writeData = false) {
    std::fill_n(ColorMemory, ColorBytes, std::byte{0});
    EndOfPipe = 0;
    Values = {};
    Values[0] = 1.0f;
    Condition = 1;
    const auto full = reinterpret_cast<std::uintptr_t>(FullTable.data());
    RegisterPairs = {0x00cu, static_cast<std::uint32_t>(full), 0x00du, static_cast<std::uint32_t>(static_cast<std::uint64_t>(full) >> 32u)};
    auto words = Frame(writeData);
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, what + ": the submission failed");
    AgcDriverWaitIdle_nid_postfix();
    Require(EndOfPipe == 1u, what + ": the end-of-pipe label was not written");
    Require(Values[0] == 0.5f, what + ": the RELEASE_MEM label did not store its value");
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    const auto center = ((Height / 2u) * Width + Width / 2u) * 4u;
    return std::to_integer<std::uint32_t>(colors[center]);
}

void ParsesTheSwitch() {
    using AgcDriver::DriverDetail::ParseLabelRecordJobs;
    Require(ParseLabelRecordJobs(nullptr) && !ParseLabelRecordJobs("0") && !ParseLabelRecordJobs("off"), "APS5_LABEL_RECORD_JOBS unset does not record labels as jobs, or 0 or off does");
    Require(ParseLabelRecordJobs("1") && ParseLabelRecordJobs("on"), "APS5_LABEL_RECORD_JOBS=1 or on does not record labels as jobs");
    bool refused = false;
    try {
        static_cast<void>(ParseLabelRecordJobs("2"));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    Require(refused, "APS5_LABEL_RECORD_JOBS=2 was accepted");
}

void DrawsSeeEarlierLabels(bool jobs) {
    const auto before = AgcDriver::DriverDetail::LabelRecordJobTotals();
    constexpr std::uint64_t Frames = 4;
    for (std::uint64_t frame = 0; frame < Frames; ++frame) {
        const auto name = std::string(jobs ? "labels as record jobs" : "labels recorded by queue 0") + ", frame " + std::to_string(frame);
        const auto red = CentreRed(name);
        Require(red + 1u >= 128u && red <= 129u, name + ": the draw after a RELEASE_MEM label wrote red " + std::to_string(red) + ", expected 128 from the value the label stored");
    }
    const auto after = AgcDriver::DriverDetail::LabelRecordJobTotals();
    if (!jobs) {
        Require(after.packets == before.packets && after.jobs == before.jobs, "labels were recorded as jobs with APS5_LABEL_RECORD_JOBS unset");
        return;
    }
    Require(after.packets - before.packets == 2u * Frames, std::to_string(after.packets - before.packets) + " label packets were taken without a drain over " + std::to_string(Frames) + " frames, expected the deferred label and the end-of-pipe label of each frame");
    Require(after.jobs - before.jobs == Frames && after.labels - before.labels == Frames, std::to_string(after.labels - before.labels) + " deferred labels were recorded in " + std::to_string(after.jobs - before.jobs) + " jobs over " + std::to_string(Frames) + " frames, expected one label and one job per frame");
    Require(after.endOfPipe - before.endOfPipe <= Frames, std::to_string(after.endOfPipe - before.endOfPipe) + " end-of-pipe labels were recorded as jobs over " + std::to_string(Frames) + " frames, expected at most one per frame");
}

void DrawsSeeWideWriteData(bool jobs) {
    const auto before = AgcDriver::DriverDetail::LabelRecordJobTotals();
    WideStore = true;
    for (std::uint64_t frame = 0; frame < 4; ++frame) {
        const auto name = "a draw after a 36-byte WRITE_DATA of its value, frame " + std::to_string(frame);
        const auto red = CentreRed(name);
        Require(red + 1u >= 128u && red <= 129u, name + ": the draw wrote red " + std::to_string(red) + ", expected 128 from the value the WRITE_DATA stored");
    }
    WideStore = false;
    const auto after = AgcDriver::DriverDetail::LabelRecordJobTotals();
    if (!jobs) Require(after.memoryPackets == before.memoryPackets, "a WRITE_DATA was recorded as a memory job with the jobs off");
    else Require(after.memoryPackets > before.memoryPackets, "no wide WRITE_DATA was recorded as an ordered memory job while draws were in flight");
}

void IndirectRegistersSeeWriteData() {
    const auto before = AgcDriver::DriverDetail::Driver::DeferredRegisterLists();
    TableWrite = true;
    for (std::uint64_t frame = 0; frame < 4; ++frame) {
        const auto name = "a SET_SH_REG_INDIRECT after a WRITE_DATA of its register list in the same command buffer, frame " + std::to_string(frame);
        const auto red = CentreRed(name);
        Require(red + 1u >= 128u && red <= 129u, name + ": the draw wrote red " + std::to_string(red) + ", expected 128 through the list the WRITE_DATA stored");
    }
    TableWrite = false;
    const auto deferred = AgcDriver::DriverDetail::Driver::DeferredRegisterLists() - before;
    Require(deferred == 4u, std::to_string(deferred) + " register lists were left for their packet over 4 command buffers that write them first, expected 4");
}

void PredicatedDrawsSeeWriteData() {
    for (std::uint64_t frame = 0; frame < 4; ++frame) {
        PredicateValue = static_cast<int>(frame % 2);
        Predicate = PredicateValue == 0 ? 1u : 0u;
        const auto name = std::string("a draw predicated on a value a WRITE_DATA ") + (PredicateValue != 0 ? "set" : "cleared") + " just before, frame " + std::to_string(frame);
        const auto red = CentreRed(name);
        if (PredicateValue != 0) Require(red + 1u >= 128u && red <= 129u, name + ": the draw wrote red " + std::to_string(red) + ", expected 128 because the predicate passes");
        else Require(red >= 254u, name + ": the draw wrote red " + std::to_string(red) + ", expected 255 because the predicate skips the second draw");
    }
    PredicateValue = -1;
}

void ConditionsSeeWriteData() {
    for (std::uint64_t frame = 0; frame < 4; ++frame) {
        NopBeforeCondition = frame % 2 == 1;
        const auto name = std::string(NopBeforeCondition ? "a COND_EXEC after a NOP over a condition a WRITE_DATA cleared" : "a COND_EXEC over a condition a WRITE_DATA cleared just before") + ", frame " + std::to_string(frame);
        const auto red = CentreRed(name, true);
        Require(red >= 254u, name + ": the draw wrote red " + std::to_string(red) + ", expected 255 because the COND_EXEC skips the second draw");
        Require(Condition == 0u, name + ": the WRITE_DATA did not clear the condition");
    }
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    Require(_putenv_s(name, value) == 0, std::string("cannot set ") + name);
#else
    Require(setenv(name, value, 1) == 0, std::string("cannot set ") + name);
#endif
}

}

int main() {
    try {
        ParsesTheSwitch();
        const bool jobs = AgcDriver::DriverDetail::ParseLabelRecordJobs(std::getenv("APS5_LABEL_RECORD_JOBS"));
        SetEnvironment("APS5_RECORD_THREAD", "1");
        SetEnvironment("APS5_PREPARE_AHEAD", "4");
        SetEnvironment("APS5_PREPARE_WORKERS", "2");
        SetEnvironment("APS5_BINDING_PLAN", "1");
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        ColorMemory = AllocateWatched(Block);
        VertexTable = BufferDescriptor(Vertices.data(), sizeof(Vertices[0]), 3u, 0x01016facu);
        FullTable = BufferDescriptor(Full.data(), 0, 16u, 0x31016facu);
        ValueTable = BufferDescriptor(Values.data(), 0, 16u, 0x31016facu);
        Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(BufferPixelHeader, BufferPixelCode.data(), sizeof(BufferPixelCode), 1);
        DrawsSeeEarlierLabels(jobs);
        ConditionsSeeWriteData();
        IndirectRegistersSeeWriteData();
        PredicatedDrawsSeeWriteData();
        DrawsSeeWideWriteData(AgcDriver::DriverDetail::ParseMemoryPacketJobs(std::getenv("APS5_MEMORY_PACKET_JOBS")));
        AgcDriverShutdown_nid_postfix();
        std::cout << "Draws on the preparation workers see the values earlier RELEASE_MEM labels stored " << (jobs ? "with labels recorded as ordered jobs" : "with labels recorded by queue 0") << " (" << AgcDriver::DriverDetail::LabelRecordJobTotals().endOfPipe << " end-of-pipe labels recorded as jobs)" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}

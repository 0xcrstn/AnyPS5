#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
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
using AgcDriver::DriverDetail::BindingPlanCounters;

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
alignas(16) std::array<std::uint32_t, 4> PixelTable{};
alignas(16) std::array<std::uint32_t, 4> SecondPixelTable{};
alignas(16) std::array<float, 4> Other{1.0f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<float, 4> First{1.0f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<float, 4> Moved{0.5f, 0.0f, 0.0f, 0.0f};
alignas(16) std::array<std::uint32_t, 8> IndirectRecords{3u, 1u, 0u, 0u, 3u, 1u, 0u, 0u};
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

std::vector<std::uint32_t> Frame(const std::uint32_t* pixelCode, const void* pixelPointer, bool indirect = false) {
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
    SetRegister(words, SetShReg, 0x08b, (indirect ? 3u : 2u) << 1u);
    SetRegister(words, SetShReg, 0x08c, static_cast<std::uint32_t>(vertexTable));
    SetRegister(words, SetShReg, 0x08d, static_cast<std::uint32_t>(vertexTable >> 32u));
    if (indirect) {
        SetRegister(words, SetShReg, 0x08e, 0u);
        const auto records = reinterpret_cast<std::uintptr_t>(IndirectRecords.data());
        words.insert(words.end(), {0xc0021100u, 1u, static_cast<std::uint32_t>(records), static_cast<std::uint32_t>(static_cast<std::uint64_t>(records) >> 32u)});
        words.insert(words.end(), {0xc0082c00u, 0u, 0x280u, 0x280u, 0x8eu | (1u << 31u), 2u, 0u, 0u, 16u, 2u});
    } else {
        words.insert(words.end(), {0xc0012d00u, 3u, 2u});
    }
    const auto labelAddress = reinterpret_cast<std::uintptr_t>(&EndOfPipe);
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(labelAddress), static_cast<std::uint32_t>(static_cast<std::uint64_t>(labelAddress) >> 32u), 1u, 0u, 0u});
    return words;
}

void Draw(const std::uint32_t* pixelCode, const void* pixelPointer, std::uint32_t expectedRed, const std::string& what, bool indirect = false) {
    std::fill_n(ColorMemory, ColorBytes, std::byte{0});
    EndOfPipe = 0;
    auto words = Frame(pixelCode, pixelPointer, indirect);
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, what + ": the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    Require(EndOfPipe == 1u, what + ": the end-of-pipe label was not written");
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    const auto center = ((Height / 2u) * Width + Width / 2u) * 4u;
    const auto red = std::to_integer<std::uint32_t>(colors[center]);
    Require(red + 1u >= expectedRed && red <= expectedRed + 1u, what + ": the centre pixel's red is " + std::to_string(red) + ", expected " + std::to_string(expectedRed));
}

void SetVertexRecords(std::uint32_t records) {
    VertexTable = BufferDescriptor(Vertices.data(), sizeof(Vertices[0]), records, 0x01016facu);
}

void SetPixelBuffer(const void* data) {
    PixelTable = BufferDescriptor(data, 0, 16u, 0x31016facu);
}

void SetEnvironment(const char* name) {
#ifdef _WIN32
    Require(_putenv_s(name, "1") == 0, std::string("cannot set ") + name);
#else
    Require(setenv(name, "1", 1) == 0, std::string("cannot set ") + name);
#endif
}

BindingPlanCounters Counters() {
    return AgcDriver::DriverDetail::Driver::Get().PlanCounters();
}

void PlannedDrawsFollowTheirBuffers(bool libraries) {
    const auto mode = std::string(" ") + (libraries ? "with" : "without") + " graphics pipeline libraries";
    SetVertexRecords(3);
    SetPixelBuffer(First.data());
    Draw(BufferPixelCode.data(), PixelTable.data(), 255, "the first planned draw" + mode);
    SetPixelBuffer(Other.data());
    Draw(BufferPixelCode.data(), PixelTable.data(), 255, "a planned draw whose buffer moved" + mode);
    SetPixelBuffer(Moved.data());
    Draw(BufferPixelCode.data(), PixelTable.data(), 128, "a planned draw whose moved buffer holds another value" + mode);
    SetPixelBuffer(First.data());
    Draw(BufferPixelCode.data(), PixelTable.data(), 255, "the first planned draw again" + mode);
    auto plan = Counters();
    Require(AgcDriver::DriverDetail::Driver::Get().DrawCacheCounters().lookups == 0, "planned draws looked up the draw cache" + mode);
    Require(plan.draws == 4 && plan.decodeMisses == 1 && plan.decodeHits == 3 && plan.decodesPatched == 0, "planned draws kept " + std::to_string(plan.decodeHits) + " decodes (" + std::to_string(plan.decodesPatched) + " patched) and made " + std::to_string(plan.decodeMisses) + " over " + std::to_string(plan.draws) + " draws, expected 3 kept, 0 patched, 1 made over 4" + mode);
    Require(plan.stages == 8 && plan.templateHits == 6, "draws whose descriptors moved and changed refilled " + std::to_string(plan.templateHits) + " of " + std::to_string(plan.stages) + " stages from a template, expected 6 of 8" + mode);
    SecondPixelTable = PixelTable;
    Draw(BufferPixelCode.data(), SecondPixelTable.data(), 255, "a planned draw whose table moved" + mode);
    plan = Counters();
    Require(plan.draws == 5 && plan.decodeHits == 4 && plan.decodesPatched == 1 && plan.decodeMisses == 1, "a draw whose user-data pointer moved kept " + std::to_string(plan.decodeHits) + " decodes (" + std::to_string(plan.decodesPatched) + " patched), expected the decode kept and patched" + mode);
    Require(plan.stages == 10 && plan.templateHits == 8, "a draw whose user-data pointer moved refilled " + std::to_string(plan.templateHits) + " of " + std::to_string(plan.stages) + " stages from a template, expected 8 of 10" + mode);
    PixelTable = BufferDescriptor(First.data(), 16u, 16u, 0x31016facu);
    Draw(BufferPixelCode.data(), PixelTable.data(), 255, "a planned draw whose buffer stride changed" + mode);
    plan = Counters();
    Require(plan.stages == 12 && plan.templateHits == 9, "a draw whose descriptor specialization bits changed refilled " + std::to_string(plan.templateHits) + " of " + std::to_string(plan.stages) + " stages from a template, expected 9 of 12 (a full materialize for the changed stage)" + mode);
    Require(plan.verifiedStages == 12 && plan.stageMismatches == 0, std::to_string(plan.stageMismatches) + " of " + std::to_string(plan.verifiedStages) + " verified planned stages differ from the full capture and materialize" + mode);
    Require(plan.verifiedDecodes == 5 && plan.decodeMismatches == 0, std::to_string(plan.decodeMismatches) + " of " + std::to_string(plan.verifiedDecodes) + " kept decodes differ from a fresh decode" + mode);
}

void PlannedIndirectDrawsKeepOwnedResults() {
    SetVertexRecords(3);
    SetPixelBuffer(First.data());
    const auto before = Counters();
    const auto redosBefore = AgcDriver::DriverDetail::FrozenIndirectRedos();
    Draw(BufferPixelCode.data(), PixelTable.data(), 255, "the first CPU-expanded indirect planned draw", true);
    SetPixelBuffer(Moved.data());
    Draw(BufferPixelCode.data(), PixelTable.data(), 128, "a CPU-expanded indirect planned draw with moved pixel values", true);
    const auto after = Counters();
    const auto redos = AgcDriver::DriverDetail::FrozenIndirectRedos() - redosBefore;
    const char* workers = std::getenv("APS5_PREPARE_WORKERS");
    if (workers != nullptr && AgcDriver::DriverDetail::FrozenIndirectDraws()) Require(redos == 2u, std::to_string(redos) + " CPU-expanded indirect draws were prepared again in order on the record thread after a worker refused them, expected 2");
    Require(after.stages == before.stages + 8u + redos, "indirect planned draws did not materialize both initial stages and both patched vertex records (" + std::to_string(after.stages - before.stages) + " stages, " + std::to_string(redos) + " worker attempts refused)");
    Require(after.verifiedStages == before.verifiedStages + 8u + redos && after.stageMismatches == before.stageMismatches, "CPU-expanded planned stages differ from full materialization");
}

void IncrementalDrawsFollowWatchedTables() {
    auto* words = reinterpret_cast<std::uint32_t*>(AllocateWatched(Block));
    auto* table = words;
    auto* values = reinterpret_cast<float*>(words + 64);
    const auto describe = [&](const float* data) {
        const auto descriptor = BufferDescriptor(data, 0, 16u, 0x31016facu);
        std::copy(descriptor.begin(), descriptor.end(), table);
    };
    std::fill_n(values, 8, 0.0f);
    values[0] = 1.0f;
    values[4] = 0.5f;
    describe(values);
    SetVertexRecords(3);
    const auto before = Counters();
    Draw(BufferPixelCode.data(), table, 255, "the first incremental draw over a watched table");
    Draw(BufferPixelCode.data(), table, 255, "an incremental draw after the first collect of the watched table");
    const auto settled = Counters();
    Draw(BufferPixelCode.data(), table, 255, "an incremental draw over an unchanged watched table");
    const auto unchanged = Counters();
    Require(unchanged.incrementalStages == before.incrementalStages + 6u, "incremental draws ran " + std::to_string(unchanged.incrementalStages - before.incrementalStages) + " incremental stages, expected 6");
    Require(unchanged.keptReads > settled.keptReads, "an incremental draw over an unchanged watched table kept no guest word (" + std::to_string(unchanged.freshReads - settled.freshReads) + " read, " + std::to_string(unchanged.keptValues - settled.keptValues) + " values kept)");
    describe(values + 4);
    Draw(BufferPixelCode.data(), table, 128, "an incremental draw whose watched table a CPU store changed");
    describe(values);
    Draw(BufferPixelCode.data(), table, 255, "an incremental draw whose watched table a CPU store changed back");
    const auto after = Counters();
    Require(after.incrementalVerified - before.incrementalVerified == after.incrementalStages - before.incrementalStages, "incremental stages were not all verified");
    Require(after.incrementalStale == before.incrementalStale && after.stageMismatches == before.stageMismatches, "incremental draws served stale words or differed from the full capture");
}

std::uint64_t PlanOutcomes(const BindingPlanCounters& counters, AgcDriver::ShaderMemory::PlanOutcome outcome) {
    return counters.planOutcomes[static_cast<std::size_t>(outcome)];
}

std::uint64_t PlanFallbacks(const BindingPlanCounters& counters, AgcDriver::ShaderMemory::PlanFallback reason) {
    return counters.planFallbacks[static_cast<std::size_t>(reason)];
}

void IncrementalPlansFollowWatchedTables() {
    using Outcome = AgcDriver::ShaderMemory::PlanOutcome;
    using Fallback = AgcDriver::ShaderMemory::PlanFallback;
    auto* words = reinterpret_cast<std::uint32_t*>(AllocateWatched(Block));
    auto* table = words;
    auto* secondTable = words + 16;
    auto* values = reinterpret_cast<float*>(words + 64);
    const auto describe = [&](std::uint32_t* target, const float* data, std::uint32_t stride) {
        const auto descriptor = BufferDescriptor(data, stride, 16u, 0x31016facu);
        std::copy(descriptor.begin(), descriptor.end(), target);
    };
    std::fill_n(values, 8, 0.0f);
    values[0] = 1.0f;
    values[4] = 0.5f;
    describe(table, values, 0);
    describe(secondTable, values, 0);
    SetVertexRecords(3);
    const auto before = Counters();
    Draw(BufferPixelCode.data(), table, 255, "the first planned draw over a watched table");
    Draw(BufferPixelCode.data(), table, 255, "a planned draw after the first collect of the watched table");
    const auto settled = Counters();
    Draw(BufferPixelCode.data(), table, 255, "a planned draw over an unchanged watched table");
    const auto unchanged = Counters();
    Require(unchanged.planStages == settled.planStages + 2u, "a planned draw evaluated " + std::to_string(unchanged.planStages - settled.planStages) + " plan instances, expected 2");
    Require(PlanOutcomes(unchanged, Outcome::Reused) == PlanOutcomes(settled, Outcome::Reused) + 1u, "a planned draw over an unchanged watched table did not reuse its pixel stage's plan instance (" + std::to_string(unchanged.planNodesFresh - settled.planNodesFresh) + " nodes re-read)");
    describe(table, values + 4, 0);
    Draw(BufferPixelCode.data(), table, 128, "a planned draw whose watched table a CPU store changed");
    const auto written = Counters();
    Require(PlanOutcomes(written, Outcome::Reused) == PlanOutcomes(unchanged, Outcome::Reused) && PlanFallbacks(written, Fallback::Memory) > PlanFallbacks(unchanged, Fallback::Memory), "a planned draw whose read range a CPU store wrote reused its plan instance");
    describe(table, values, 0);
    Draw(BufferPixelCode.data(), table, 255, "a planned draw whose watched table a CPU store changed back");
    Draw(BufferPixelCode.data(), table, 255, "a planned draw over the restored watched table");
    const auto restored = Counters();
    Draw(BufferPixelCode.data(), secondTable, 255, "a planned draw whose table pointer user data changed");
    const auto moved = Counters();
    Require(PlanOutcomes(moved, Outcome::Reused) == PlanOutcomes(restored, Outcome::Reused) && PlanFallbacks(moved, Fallback::Inputs) > PlanFallbacks(restored, Fallback::Inputs), "a planned draw whose root user data changed reused its plan instance");
    describe(table, values, 16u);
    Draw(BufferPixelCode.data(), table, 255, "a planned draw whose buffer stride changed");
    const auto specialized = Counters();
    Require(PlanFallbacks(specialized, Fallback::Specialization) == PlanFallbacks(moved, Fallback::Specialization) + 1u && PlanOutcomes(specialized, Outcome::Full) == PlanOutcomes(moved, Outcome::Full) + 1u, "a planned draw whose specialization words changed did not take the full capture");
    Require(specialized.planVerified - before.planVerified == specialized.planStages - before.planStages, "planned stages were not all verified against the full capture");
    Require(specialized.planStale == before.planStale && specialized.stageMismatches == before.stageMismatches, "plan instances served stale words or differed from the full capture");
}

}

int main(int argc, char** argv) {
    try {
        const bool incremental = argc == 2 && std::string(argv[1]) == "incremental";
        const bool plans = argc == 2 && std::string(argv[1]) == "plans";
        SetEnvironment("APS5_BINDING_PLAN");
        SetEnvironment("APS5_VERIFY_BINDING_PLAN");
        if (incremental) {
            SetEnvironment("APS5_INCREMENTAL_BINDINGS");
            SetEnvironment("APS5_VERIFY_INCREMENTAL_BINDINGS");
        }
        if (plans) {
            SetEnvironment("APS5_INCREMENTAL_PLANS");
            SetEnvironment("APS5_VERIFY_INCREMENTAL_PLANS");
        }
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        ColorMemory = AllocateWatched(Block);
        Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(BufferPixelHeader, BufferPixelCode.data(), sizeof(BufferPixelCode), 1);
        PlannedDrawsFollowTheirBuffers(libraries);
        PlannedIndirectDrawsKeepOwnedResults();
        if (incremental) IncrementalDrawsFollowWatchedTables();
        if (plans) IncrementalPlansFollowWatchedTables();
        AgcDriverShutdown_nid_postfix();
        std::cout << "Planned draws read their buffers, reuse their decode and templates, and match the full capture " << (libraries ? "with" : "without") << " graphics pipeline libraries" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}

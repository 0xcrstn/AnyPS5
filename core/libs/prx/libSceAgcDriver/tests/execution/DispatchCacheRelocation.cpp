#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
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
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DispatchCacheTotals;

alignas(256) std::array<std::uint32_t, 12> CopyCode{0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xbf8cc07fu, 0xe0300000u, 0x80010000u, 0xbf8c3f70u, 0x4a000081u, 0xe0700000u, 0x80020000u, 0xbf810000u};

struct alignas(256) Table {
    std::array<std::uint32_t, 8> words{};
};

struct alignas(256) Buffer {
    std::array<std::uint32_t, 4> words{};
};

struct GuestBlock {
    alignas(256) std::array<std::byte, 256> reserved;
    std::array<Table, 6> tables;
    std::array<Buffer, 3> inputs;
    alignas(65536) Buffer output;
    alignas(256) volatile std::uint32_t done = 0;
};

constexpr std::size_t GuestBlockBytes = 2 * 65536;

GuestBlock* Guest = nullptr;
#ifdef _WIN32
HANDLE GuestSection = nullptr;
#endif

std::byte* AllocateBlock(bool watched) {
    if (!watched) return static_cast<std::byte*>(::operator new(GuestBlockBytes, std::align_val_t{GuestBlockBytes}));
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(GuestBlockBytes, GuestBlockBytes);
    GuestSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, static_cast<DWORD>(GuestBlockBytes), nullptr);
    if (GuestSection == nullptr) {
        GuestArena::GuestArenaRelease_nid_postfix(block, GuestBlockBytes);
        Require(false, "cannot create the watched dispatch backing");
    }
    try {
        GuestArena::GuestArenaMap_nid_postfix(block, GuestBlockBytes, GuestSection, 0, PAGE_READWRITE);
    } catch (...) {
        CloseHandle(GuestSection);
        GuestSection = nullptr;
        GuestArena::GuestArenaRelease_nid_postfix(block, GuestBlockBytes);
        throw;
    }
#else
    void* raw = mmap(nullptr, 2u * GuestBlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(raw != MAP_FAILED, "cannot map a watched dispatch block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + GuestBlockBytes - 1u) & ~static_cast<std::uintptr_t>(GuestBlockBytes - 1u);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned != begin + GuestBlockBytes) munmap(reinterpret_cast<void*>(aligned + GuestBlockBytes), begin + GuestBlockBytes - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, GuestBlockBytes);
#endif
    return static_cast<std::byte*>(block);
}

void ReleaseBlock(std::byte* block, bool watched) {
    if (!watched) {
        ::operator delete(block, std::align_val_t{GuestBlockBytes});
        return;
    }
#ifdef _WIN32
    GuestArena::GuestArenaReset_nid_postfix(block, GuestBlockBytes);
    GuestArena::GuestArenaForgetSection_nid_postfix(GuestSection);
    CloseHandle(GuestSection);
    GuestSection = nullptr;
    GuestArena::GuestArenaRelease_nid_postfix(block, GuestBlockBytes);
#else
    GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, GuestBlockBytes);
    munmap(block, GuestBlockBytes);
#endif
}

std::uint32_t Low(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

void Describe(Table& table, const Buffer& input, std::uint32_t inputBytes) {
    table.words = {Low(input.words.data()), High(input.words.data()) & 0xffffu, inputBytes, 0x31016facu, Low(Guest->output.words.data()), High(Guest->output.words.data()) & 0xffffu, sizeof(Guest->output.words), 0x31016facu};
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

std::uint32_t Dispatch(const Table& table, std::uint32_t threads) {
    Guest->output.words = {};
    std::vector<std::uint32_t> words;
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(CopyCode.data()));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    SetShaderRegister(words, 0x207, threads);
    SetShaderRegister(words, 0x208, 1);
    SetShaderRegister(words, 0x209, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, 2u << 1u);
    SetShaderRegister(words, 0x240, Low(table.words.data()));
    SetShaderRegister(words, 0x241, High(table.words.data()));
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
    Guest->done = 0;
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(const_cast<std::uint32_t*>(&Guest->done)), High(const_cast<std::uint32_t*>(&Guest->done)), 1});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "compute submit failed");
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Guest->output.words.data()), sizeof(Guest->output.words));
    return Guest->output.words[0];
}

std::string Describe(const DispatchCacheTotals& totals) {
    return std::to_string(totals.lookups) + " lookups, " + std::to_string(totals.exactHits) + " exact hits, " + std::to_string(totals.relocatedHits) + " relocated hits (" + std::to_string(totals.patchedHits) + " patched), " + std::to_string(totals.inserts) + " inserts";
}

void Expect(const char* what, std::uint32_t stored, std::uint32_t expected, std::uint64_t relocated, std::uint64_t patched, std::uint64_t inserts) {
    const auto totals = AgcDriver::DriverDetail::Driver::Get().DispatchCacheCounters();
    const std::string state = std::string(what) + ": " + Describe(totals) + ", stored " + std::to_string(stored);
    Require(stored == expected + 1, state + ", expected " + std::to_string(expected + 1));
    Require(totals.relocatedHits == relocated && totals.patchedHits == patched && totals.inserts == inserts, state + ", expected " + std::to_string(relocated) + " relocated hits (" + std::to_string(patched) + " patched) and " + std::to_string(inserts) + " inserts");
}

void RelocatedDispatchesBindTheCurrentBuffers(bool relocation) {
    Guest->inputs[0].words = {100, 0, 0, 0};
    Guest->inputs[1].words = {100, 0, 0, 0};
    Guest->inputs[2].words = {200, 0, 0, 0};
    for (std::size_t i = 0; i < 2; ++i) Describe(Guest->tables[i], Guest->inputs[0], sizeof(Guest->inputs[0].words));
    Describe(Guest->tables[2], Guest->inputs[1], sizeof(Guest->inputs[1].words));
    Describe(Guest->tables[3], Guest->inputs[2], sizeof(Guest->inputs[2].words));
    Describe(Guest->tables[4], Guest->inputs[2], 8);
    Describe(Guest->tables[5], Guest->inputs[0], sizeof(Guest->inputs[0].words));
    const std::uint64_t on = relocation ? 1 : 0;
    Expect("first sight", Dispatch(Guest->tables[0], 1), 100, 0, 0, 1);
    Expect("table moved, same content", Dispatch(Guest->tables[1], 1), 100, on, 0, 2 - on);
    Expect("table moved, input buffer moved to equal content", Dispatch(Guest->tables[2], 1), 100, 2 * on, on, 3 - 2 * on);
    Expect("table moved, input buffer moved to other content", Dispatch(Guest->tables[3], 1), 200, 3 * on, 2 * on, 4 - 3 * on);
    Expect("table moved, input descriptor size changed", Dispatch(Guest->tables[4], 1), 200, 3 * on, 2 * on, 5 - 3 * on);
    Expect("table moved, thread count changed", Dispatch(Guest->tables[5], 2), 100, 3 * on, 2 * on, 6 - 3 * on);
    Guest->inputs[2].words[0] = 300;
    Expect("buffer contents changed in place", Dispatch(Guest->tables[3], 1), 300, 3 * on, 2 * on, 6 - 3 * on);
    Describe(Guest->tables[0], Guest->inputs[2], sizeof(Guest->inputs[2].words));
    Expect("table rewritten in place to another input buffer", Dispatch(Guest->tables[0], 1), 300, 3 * on, 2 * on, 7 - 4 * on);
    Describe(Guest->tables[0], Guest->inputs[1], sizeof(Guest->inputs[1].words));
    Expect("table rewritten in place to a third input buffer", Dispatch(Guest->tables[0], 1), 100, 3 * on, 2 * on, 8 - 5 * on);
}

void IncrementalPlansFollowCurrentRootsAndDescriptors() {
    using Outcome = AgcDriver::ShaderMemory::PlanOutcome;
    using Fallback = AgcDriver::ShaderMemory::PlanFallback;
    auto& driver = AgcDriver::DriverDetail::Driver::Get();
    const auto outcome = [](const auto& counts, Outcome value) { return counts.planOutcomes[static_cast<std::size_t>(value)]; };
    const auto fallback = [](const auto& counts, Fallback value) { return counts.planFallbacks[static_cast<std::size_t>(value)]; };
    const auto run = [](const Table& table, std::uint32_t expected) {
        const auto stored = Dispatch(table, 1);
        Require(stored == expected + 1u, "an incremental dispatch stored " + std::to_string(stored) + ", expected " + std::to_string(expected + 1u));
    };
    Guest->inputs[0].words = {100, 0, 0, 0};
    Guest->inputs[1].words = {200, 0, 0, 0};
    Describe(Guest->tables[0], Guest->inputs[0], sizeof(Guest->inputs[0].words));
    Describe(Guest->tables[1], Guest->inputs[0], sizeof(Guest->inputs[0].words));
    const auto before = driver.PlanCounters();
    run(Guest->tables[0], 100);
    run(Guest->tables[0], 100);
    const auto settled = driver.PlanCounters();
    run(Guest->tables[0], 100);
    const auto unchanged = driver.PlanCounters();
    Require(unchanged.planStages == settled.planStages + 1u && outcome(unchanged, Outcome::Reused) == outcome(settled, Outcome::Reused) + 1u, "an unchanged dispatch did not reuse its plan instance: stages=" + std::to_string(unchanged.planStages - settled.planStages) + ", reused=" + std::to_string(outcome(unchanged, Outcome::Reused) - outcome(settled, Outcome::Reused)) + ", memory=" + std::to_string(fallback(unchanged, Fallback::Memory) - fallback(settled, Fallback::Memory)) + ", first=" + std::to_string(fallback(unchanged, Fallback::First) - fallback(settled, Fallback::First)) + ", unrecorded=" + std::to_string(fallback(unchanged, Fallback::Unrecorded) - fallback(settled, Fallback::Unrecorded)));
    Describe(Guest->tables[0], Guest->inputs[1], sizeof(Guest->inputs[1].words));
    run(Guest->tables[0], 200);
    const auto rewritten = driver.PlanCounters();
    Require(fallback(rewritten, Fallback::Memory) > fallback(unchanged, Fallback::Memory) && outcome(rewritten, Outcome::Reused) == outcome(unchanged, Outcome::Reused), "a descriptor rewritten by the CPU reused its old plan values");
    run(Guest->tables[1], 100);
    const auto moved = driver.PlanCounters();
    Require(fallback(moved, Fallback::Inputs) > fallback(rewritten, Fallback::Inputs) && outcome(moved, Outcome::Reused) == outcome(rewritten, Outcome::Reused), "a relocated table root reused values from the old table");
    run(Guest->tables[1], 100);
    run(Guest->tables[1], 100);
    Guest->inputs[0].words[0] = 300;
    run(Guest->tables[1], 300);
    Guest->tables[1].words[1] |= 16u << 16u;
    const auto priorSpecialization = driver.PlanCounters();
    run(Guest->tables[1], 300);
    const auto after = driver.PlanCounters();
    Require(fallback(after, Fallback::Specialization) == fallback(priorSpecialization, Fallback::Specialization) + 1u && outcome(after, Outcome::Full) == outcome(priorSpecialization, Outcome::Full) + 1u, "a changed descriptor stride did not invalidate specialization");
    Require(after.planVerified - before.planVerified == after.planStages - before.planStages && after.planStages - before.planStages == 9u, "incremental dispatches were not all verified against fresh captures and materialization");
    Require(after.planStale == before.planStale, "incremental dispatches served stale descriptor words");
    Require(driver.DispatchCacheCounters().lookups == 0, "the dispatch cache hid an incremental plan evaluation");
}

}

int main(int argc, char** argv) {
    try {
        const bool plans = argc == 2 && std::string(argv[1]) == "--plans";
        const bool tracedRelocation = argc == 2 && std::string(argv[1]) == "--plans-relocation";
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const bool relocation = std::getenv("APS5_NO_DISPATCH_RELOCATION") == nullptr;
        static_assert(sizeof(GuestBlock) <= GuestBlockBytes);
        auto* block = AllocateBlock(plans || tracedRelocation);
        Guest = new (block) GuestBlock{};
        GuestAllocations::Mutation().Add(block, GuestBlockBytes, true, true, true);
        if (plans) IncrementalPlansFollowCurrentRootsAndDescriptors();
        else RelocatedDispatchesBindTheCurrentBuffers(relocation);
        if (tracedRelocation) {
            const auto plan = AgcDriver::DriverDetail::Driver::Get().PlanCounters();
            Require(plan.planStages != 0u && plan.planVerified == plan.planStages && plan.planStale == 0u, "relocated dispatch captures did not verify traced plan snapshots and metadata");
        }
        AgcDriverShutdown_nid_postfix();
        GuestAllocations::Mutation().Remove(block);
        ReleaseBlock(block, plans || tracedRelocation);
        if (plans) std::cout << "Incremental dispatch plans followed moved roots, rewritten descriptors and changed specialization\n";
        else std::cout << "Dispatches with moved tables and buffers " << (relocation ? "reused" : "did not reuse") << " the cached dispatch and bound the current buffers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}

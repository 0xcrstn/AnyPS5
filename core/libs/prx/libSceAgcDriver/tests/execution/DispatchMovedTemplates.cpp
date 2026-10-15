#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using MovedCounters = AgcDriver::VulkanDevice::MovedTemplateStatistics;

alignas(256) std::array<std::uint32_t, 12> CopyCode{0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xbf8cc07fu, 0xe0300000u, 0x80010000u, 0xbf8c3f70u, 0x4a000081u, 0xe0700000u, 0x80020000u, 0xbf810000u};

struct alignas(256) Table {
    std::array<std::uint32_t, 8> words{};
};

struct alignas(256) Buffer {
    std::array<std::uint32_t, 4> words{};
};

struct GuestBlock {
    std::array<Table, 4> tables;
    std::array<Buffer, 3> inputs;
    std::array<Buffer, 2> outputs;
    alignas(256) volatile std::uint32_t done = 0;
};

constexpr std::size_t GuestBlockBytes = 65536;

GuestBlock* Guest = nullptr;

std::uint32_t Low(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

void Describe(Table& table, const Buffer& input, const Buffer& output) {
    table.words = {Low(input.words.data()), High(input.words.data()) & 0xffffu, sizeof(input.words), 0x31016facu, Low(output.words.data()), High(output.words.data()) & 0xffffu, sizeof(output.words), 0x31016facu};
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

std::uint32_t Dispatch(const Table& table, Buffer& output) {
    output.words = {};
    std::vector<std::uint32_t> words;
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(CopyCode.data()));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    SetShaderRegister(words, 0x207, 1);
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
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(output.words.data()), sizeof(output.words));
    return output.words[0];
}

std::string Describe(const MovedCounters& counters) {
    return std::to_string(counters.hits) + " moved-buffer template hits (" + std::to_string(counters.buffers) + " buffers), " + std::to_string(counters.refused) + " refused, " + std::to_string(counters.verified) + " verified";
}

void MovedReadOnlyBuffersHitTheirTemplate() {
    Guest->inputs[0].words = {100, 0, 0, 0};
    Guest->inputs[1].words = {200, 0, 0, 0};
    Guest->inputs[2].words = {300, 0, 0, 0};
    Describe(Guest->tables[0], Guest->inputs[0], Guest->outputs[0]);
    Describe(Guest->tables[1], Guest->inputs[1], Guest->outputs[0]);
    Describe(Guest->tables[2], Guest->inputs[2], Guest->outputs[1]);
    Describe(Guest->tables[3], Guest->inputs[0], Guest->outputs[0]);
    const auto counters = [] { return AgcDriver::VulkanDevice::MovedTemplateCounters(); };
    Require(Dispatch(Guest->tables[0], Guest->outputs[0]) == 101, "the first dispatch stored the wrong value");
    const auto first = counters();
    Require(Dispatch(Guest->tables[1], Guest->outputs[0]) == 201, "a dispatch whose read-only buffer moved stored the wrong value: " + Describe(counters()));
    const auto movedInput = counters();
    Require(movedInput.hits == first.hits + 1 && movedInput.buffers == first.buffers + 1, "a dispatch whose only change is a read-only buffer's address did not hit its template through one moved buffer: " + Describe(movedInput));
    Require(movedInput.verified == movedInput.hits, "a moved-buffer template hit was not verified against a fresh build: " + Describe(movedInput));
    Guest->outputs[0].words = {};
    Require(Dispatch(Guest->tables[2], Guest->outputs[1]) == 301, "a dispatch whose written buffer moved stored the wrong value: " + Describe(counters()));
    const auto movedOutput = counters();
    Require(movedOutput.hits == movedInput.hits, "a dispatch whose written buffer moved was served by a template: " + Describe(movedOutput));
    Require(Guest->outputs[0].words[0] == 0, "a dispatch whose written buffer moved stored into the template's written buffer");
    Require(Dispatch(Guest->tables[3], Guest->outputs[0]) == 101, "a dispatch back on the first buffers stored the wrong value: " + Describe(counters()));
    Guest->inputs[1].words[0] = 400;
    Require(Dispatch(Guest->tables[1], Guest->outputs[0]) == 401, "a moved-buffer template hit read stale bytes after a CPU store: " + Describe(counters()));
    const auto last = counters();
    Require(last.verified == last.hits, "moved-buffer template hits were not all verified: " + Describe(last));
    const char* prepared = std::getenv("APS5_PREPARED_MOVED_SETS");
    if (prepared == nullptr || std::strcmp(prepared, "0") != 0) Require(last.preparedAdopted != 0, "no moved-buffer set prepared before the device call was adopted: " + Describe(last));
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
        SetEnvironment("APS5_DISPATCH_MOVED_TEMPLATES", "1");
        SetEnvironment("APS5_VERIFY_DISPATCH_MOVED_TEMPLATES", "1");
        SetEnvironment("APS5_NO_DISPATCH_RELOCATION", "1");
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        Require(AgcDriver::VulkanDevice::MovedDispatchTemplates(), "APS5_DISPATCH_MOVED_TEMPLATES did not turn moved dispatch templates on");
        static_assert(sizeof(GuestBlock) <= GuestBlockBytes);
        auto* block = static_cast<std::byte*>(::operator new(GuestBlockBytes, std::align_val_t{GuestBlockBytes}));
        Guest = new (block) GuestBlock{};
        GuestAllocations::Mutation().Add(block, GuestBlockBytes, true, true, true);
        MovedReadOnlyBuffersHitTheirTemplate();
        AgcDriverShutdown_nid_postfix();
        GuestAllocations::Mutation().Remove(block);
        ::operator delete(block, std::align_val_t{GuestBlockBytes});
        std::cout << "Dispatches whose read-only buffers moved hit their template and bound the moved buffers; a moved written buffer built its own resources\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}

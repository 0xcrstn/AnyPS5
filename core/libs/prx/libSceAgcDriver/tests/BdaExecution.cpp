#include "BdaShader.hpp"
#include "BdaTranslationFixtures.hpp"
#include "BdaAbi.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ResidentMirrors.hpp"
#include "BdaPlacement.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

class Pipeline {
public:
    Pipeline(const Context& context, std::span<const std::uint32_t> code, const std::array<Buffer*, 3>& buffers) : context(context) {
        try {
            std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            setInfo.bindingCount = bindings.size();
            setInfo.pBindings = bindings.data();
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &size;
            Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &setLayout};
            Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &set), "vkAllocateDescriptorSets");
            for (std::uint32_t i = 0; i < buffers.size(); ++i) {
                const VkDescriptorBufferInfo buffer{buffers[i]->Handle(), 0, buffers[i]->Bytes().size()};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set;
                write.dstBinding = i;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &buffer;
                context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 1, &write, 0, nullptr);
            }
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &setLayout;
            Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = code.size_bytes();
            moduleInfo.pCode = code.data();
            Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            pipelineInfo.layout = layout;
            Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
        } catch (...) { release(); throw; }
    }

    ~Pipeline() { release(); }

    void Run(std::uint32_t groups) {
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &upload, 0, nullptr, 0, nullptr);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, groups, 1, 1);
        VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &download, 0, nullptr, 0, nullptr);
        batch.SubmitAndWait();
    }

private:
    void release() noexcept {
        if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (layout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        if (setLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }

    const Context& context;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

std::unique_ptr<Buffer> MakeTable(const Context& context, std::span<const ShaderRecompiler::BdaAbi::Range> ranges, std::size_t count) {
    namespace Table = ShaderRecompiler::BdaTable;
    const auto translation = BdaTestTranslation();
    auto words = Table::Build(ranges.first(std::min(count, ranges.size())), translation);
    if (translation == Table::Translation::Legacy) words[1] = static_cast<std::uint32_t>(count);
    else Table::Validate(words, translation);
    auto table = std::make_unique<Buffer>(context, words.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(table->Bytes().data(), words.data(), words.size() * sizeof(std::uint32_t));
    return table;
}

}

static void RunWrittenRangeDecodeTests() {
    namespace Abi = ShaderRecompiler::BdaAbi;
    std::vector<std::uint32_t> words(Abi::FaultBufferBytes / sizeof(std::uint32_t));
    std::vector<std::pair<std::uint64_t, std::size_t>> marked;
    const auto decode = [&] {
        marked.clear();
        return Abi::ForEachWrittenRange(words.data(), [&](std::uint64_t address, std::size_t bytes) { marked.emplace_back(address, bytes); });
    };
    const auto claim = [&](std::uint32_t index, std::uint32_t slot, std::uint64_t address, std::uint32_t mask) {
        words[Abi::WrittenListWord + index] = slot;
        words[Abi::WrittenSlotsWord + 2 * slot] = Abi::WrittenChunkKey(address);
        words[Abi::WrittenSlotsWord + 2 * slot + 1] = mask;
        words[Abi::WrittenCountWord] = std::max(words[Abi::WrittenCountWord], index + 1);
    };
    constexpr std::uint64_t page = std::uint64_t{1} << Abi::WrittenPageShift;
    Require(decode() && marked.empty(), "an empty written-page record marked pages");
    constexpr std::uint64_t top = 0x7ffffffe0000ULL;
    claim(0, 5, top, 0b1011u);
    Require(decode() && marked == std::vector<std::pair<std::uint64_t, std::size_t>>{{top, 2 * page}, {top + 3 * page, page}}, "written pages near the top of the address space were not decoded as written");
    std::fill(words.begin(), words.end(), 0u);
    constexpr std::uint64_t high = 0x200000001000ULL;
    claim(0, Abi::WrittenPageSlots - 1, high, 1u << ((high >> Abi::WrittenPageShift) & (Abi::WrittenChunkPages - 1u)));
    Require(decode() && marked == std::vector<std::pair<std::uint64_t, std::size_t>>{{high, page}}, "a written page above 16 TiB was decoded at another address");
    std::fill(words.begin(), words.end(), 0u);
    constexpr std::uint64_t chunk = 0x3e0000000ULL;
    claim(0, 7, chunk, 0xffffffffu);
    claim(1, 8, chunk + (std::uint64_t{1} << Abi::WrittenChunkShift), 1u);
    claim(2, 9, chunk - (std::uint64_t{1} << Abi::WrittenChunkShift), 0x80000000u);
    Require(decode() && marked == std::vector<std::pair<std::uint64_t, std::size_t>>{{chunk, 32 * page}, {chunk + 32 * page, page}, {chunk - page, page}}, "whole, first-page and last-page chunks were not decoded exactly");
    words[Abi::WrittenListWord + 1] = Abi::WrittenPageSlots;
    Require(!decode(), "a claim outside the slots was accepted");
    std::fill(words.begin(), words.end(), 0u);
    words[Abi::WrittenCountWord] = 1;
    Require(!decode(), "a claim of an empty slot was accepted");
    words[Abi::WrittenCountWord] = Abi::WrittenPageSlots + 1;
    Require(!decode(), "more claims than slots were accepted");
}

static void RunBdaDwordWriteTests(const Context& context) {
    RunWrittenRangeDecodeTests();
    namespace Abi = ShaderRecompiler::BdaAbi;
    constexpr std::uint64_t guest = 0x7fff12340f80ULL;
    constexpr std::size_t firstBytes = 160;
    Buffer first(context, firstBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer second(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer readOnly(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    const std::array<Abi::Range, 3> ranges{{
        {guest, guest + firstBytes, first.DeviceAddress(), Abi::Read | Abi::Write, 0},
        {guest + firstBytes, guest + firstBytes + 16, second.DeviceAddress(), Abi::Read | Abi::Write, 0},
        {guest + firstBytes + 16, guest + firstBytes + 32, readOnly.DeviceAddress(), Abi::Read, 0},
    }};
    std::unique_ptr<Buffer> table;
    Buffer fault(context, Abi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const std::array<std::uint32_t, 4> values{0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};
    const auto bytesAt = [&](std::uint64_t address) -> std::byte* {
        for (std::size_t index = 0; index < ranges.size(); ++index) {
            if (address < ranges[index].begin || address >= ranges[index].end) continue;
            auto& buffer = index == 0 ? first : index == 1 ? second : readOnly;
            return buffer.Bytes().data() + (address - ranges[index].begin);
        }
        return nullptr;
    };
    const auto notedBytes = [&](std::uint64_t first, std::uint64_t last) {
        std::uint64_t covered = 0, total = 0;
        const auto* faultWords = reinterpret_cast<const std::uint32_t*>(fault.Bytes().data());
        Require(Abi::ForEachWrittenRange(faultWords, [&](std::uint64_t begin, std::size_t bytes) {
            total += bytes;
            for (const auto address : {first, last}) covered += address >= begin && address < begin + bytes ? 1u : 0u;
        }), "a BDA dword write left an invalid written-page record");
        return std::pair{covered, total};
    };
    const auto write = [&](std::uint64_t address, std::uint32_t dwords, std::uint32_t storedBytes, Abi::FaultReason reason, std::uint32_t groups = 1) {
        table = MakeTable(context, ranges, ranges.size());
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        for (auto* buffer : {&first, &second, &readOnly}) std::memset(buffer->Bytes().data(), 0xa5, buffer->Bytes().size());
        Pipeline pipeline(context, MakeBdaDwordWriteTestShader(address, dwords, values.data()), {table.get(), &fault, &output});
        pipeline.Run(groups);
        Abi::Fault report{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        for (std::uint32_t byte = 0; byte < dwords * 4u; ++byte) {
            const auto* stored = bytesAt(address + byte);
            const auto expected = byte < storedBytes ? static_cast<std::byte>(values[byte / 4u] >> ((byte % 4u) * 8u)) : std::byte{0xa5};
            Require(stored != nullptr && *stored == expected, "BDA dword write stored incorrect bytes at offset " + std::to_string(byte) + " of guest + " + std::to_string(address - guest));
        }
        Require(*bytesAt(address - 1) == std::byte{0xa5}, "BDA dword write touched the byte before it");
        if (address + dwords * 4u < guest + firstBytes + 32) Require(*bytesAt(address + dwords * 4u) == std::byte{0xa5}, "BDA dword write touched the byte after it");
        if (static_cast<std::uint32_t>(reason) == 0) {
            Require(report.state == Abi::FaultState::Empty, "a mapped BDA dword write published a fault");
        } else {
            Require(report.state == Abi::FaultState::Ready && report.reason == reason && report.instruction == 0x1234, "a BDA dword write did not publish its fault");
        }
        const auto [covered, total] = notedBytes(address, address + (storedBytes != 0 ? storedBytes - 1u : 0u));
        if (storedBytes == 0) {
            Require(total == 0, "a BDA dword write that stored nothing noted pages");
        } else {
            const auto pages = ((address + storedBytes - 1u) >> Abi::WrittenPageShift) - (address >> Abi::WrittenPageShift) + 1u;
            Require(covered == 2 && total == pages << Abi::WrittenPageShift, "a BDA dword write did not note exactly its written pages");
            const auto chunks = ((address + storedBytes - 1u) >> Abi::WrittenChunkShift) - (address >> Abi::WrittenChunkShift) + 1u;
            std::uint32_t claims = 0;
            std::memcpy(&claims, fault.Bytes().data() + Abi::WrittenCountWord * sizeof(std::uint32_t), sizeof(claims));
            Require(claims == chunks, "a BDA dword write claimed a written-page slot more than once per chunk");
        }
    };
    write(guest + 16, 4, 16, static_cast<Abi::FaultReason>(0));
    write(guest + 20, 4, 16, static_cast<Abi::FaultReason>(0));
    write(guest + 8, 2, 8, static_cast<Abi::FaultReason>(0));
    write(guest + 12, 2, 8, static_cast<Abi::FaultReason>(0));
    write(guest + 32, 3, 12, static_cast<Abi::FaultReason>(0));
    write(guest + 0x78, 4, 16, static_cast<Abi::FaultReason>(0));
    write(guest + 0x7c, 2, 8, static_cast<Abi::FaultReason>(0));
    write(guest + 41, 4, 16, static_cast<Abi::FaultReason>(0));
    write(guest + firstBytes - 8, 4, 16, static_cast<Abi::FaultReason>(0));
    write(guest + firstBytes + 12, 4, 4, Abi::FaultReason::Permission);
    write(guest + 0x7c, 2, 8, static_cast<Abi::FaultReason>(0), 64);
}

void RunBdaExecutionTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    Buffer first(context, 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer second(context, 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    first.Bytes()[0] = std::byte{0x11};
    first.Bytes()[1] = std::byte{0x22};
    first.Bytes()[2] = std::byte{0x33};
    second.Bytes()[0] = std::byte{0x44};
    second.Bytes()[1] = std::byte{0x55};
    constexpr std::uint64_t guest = 0x7fff12340001ULL;
    std::array<Abi::Range, 2> ranges{{{guest, guest + 3, first.DeviceAddress(), Abi::Read, 0}, {guest + 3, guest + 5, second.DeviceAddress(), Abi::Read, 0}}};
    std::unique_ptr<Buffer> table;
    Buffer fault(context, sizeof(Abi::Fault), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto run = [&](std::uint64_t address, std::uint32_t bits, std::uint32_t expected, Abi::FaultReason reason, std::uint32_t count = 2, std::uint32_t groups = 1, std::int64_t offset = 0) {
        table = MakeTable(context, ranges, count);
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::uint32_t sentinel = 0xdeadbeef;
        std::memcpy(output.Bytes().data(), &sentinel, sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaTestShader(address, bits, offset), {table.get(), &fault, &output});
        pipeline.Run(groups);
        Abi::Fault report{};
        std::uint32_t result = 0;
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        std::memcpy(&result, output.Bytes().data(), sizeof(result));
        if (static_cast<std::uint32_t>(reason) == 0) {
            Require(report.state == Abi::FaultState::Empty && result == expected, "BDA GPU read produced incorrect data or a fault");
        } else {
            Require(report.state == Abi::FaultState::Ready && report.reason == reason && report.instruction == 0x1234, "BDA GPU fault was not published correctly");
            Require(result == sentinel, "faulting BDA shader continued to output a substitute value");
        }
    };
    run(guest, 8, 0x11, static_cast<Abi::FaultReason>(0));
    run(guest + 1, 16, 0x3322, static_cast<Abi::FaultReason>(0));
    run(guest + 1, 32, 0x55443322, static_cast<Abi::FaultReason>(0));
    run(guest + 5, 8, 0, Abi::FaultReason::Unmapped, 2, 64);
    run(guest - 1, 8, 0, Abi::FaultReason::Unmapped);
    run(std::numeric_limits<std::uint64_t>::max() - 1, 32, 0, Abi::FaultReason::Overflow);
    if (BdaTestTranslation() == ShaderRecompiler::BdaTable::Translation::Legacy) run(guest, 8, 0, Abi::FaultReason::InvalidTable, 3);
    run(std::numeric_limits<std::uint64_t>::max() - 2, 8, 0, Abi::FaultReason::Overflow, 2, 1, 4);
    run(1, 8, 0, Abi::FaultReason::Overflow, 2, 1, -4);
    Buffer words(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto read = [&](std::uint64_t address, std::uint32_t dwords, bool coherent, bool stops, std::uint32_t value, bool faults) {
        table = MakeTable(context, ranges, 2);
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::array<std::uint32_t, 4> sentinel{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
        std::memcpy(words.Bytes().data(), sentinel.data(), sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaDwordReadTestShader(address, dwords, coherent, stops), {table.get(), &fault, &words});
        pipeline.Run(1);
        Abi::Fault report{};
        std::array<std::uint32_t, 4> result{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        std::memcpy(result.data(), words.Bytes().data(), sizeof(result));
        auto expected = sentinel;
        if (!faults || !stops) {
            expected[0] = value;
            for (std::uint32_t dword = 1; dword < dwords; ++dword) expected[dword] = 0;
        }
        if (faults) {
            Require(report.state == Abi::FaultState::Ready && report.reason == Abi::FaultReason::Unmapped && report.address == guest + 5 && report.bytes == 1 && report.instruction == 0x1234, "BDA dword read did not publish its first fault");
        } else {
            Require(report.state == Abi::FaultState::Empty, "mapped BDA dword read published a fault");
        }
        Require(result == expected, faults && stops ? "faulting BDA dword read continued to output a substitute value" : "BDA dword read produced incorrect data");
    };
    for (const bool stops : {true, false}) {
        for (const bool coherent : {false, true}) {
            read(guest + 1, 1, coherent, stops, 0x55443322, false);
            read(guest + 3, 1, coherent, stops, 0x5544, true);
            read(guest + 1, 4, coherent, stops, 0x55443322, true);
        }
    }
    Buffer wide(context, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    for (std::uint32_t dword = 0; dword < 8u; ++dword) {
        const std::uint32_t value = 0xa0b0c000u + dword;
        std::memcpy(wide.Bytes().data() + dword * 4u, &value, sizeof(value));
    }
    constexpr std::uint64_t spanGuest = 0x7fff56780000ULL;
    const std::array<Abi::Range, 3> spanRanges{{ranges[0], ranges[1], {spanGuest, spanGuest + 32, wide.DeviceAddress(), Abi::Read, 0}}};
    std::unique_ptr<Buffer> spanTable;
    const auto span = [&](std::uint64_t address, std::int32_t offset, std::uint32_t extracted, bool coherent, bool stops, std::array<std::uint32_t, 4> values, Abi::FaultReason reason = {}, std::uint64_t faultAddress = 0, std::uint32_t faultBytes = 0) {
        spanTable = MakeTable(context, spanRanges, 3);
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::array<std::uint32_t, 4> sentinel{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
        std::memcpy(words.Bytes().data(), sentinel.data(), sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaSpanReadTestShader(address, static_cast<std::uint32_t>(offset), extracted, coherent, stops), {spanTable.get(), &fault, &words});
        pipeline.Run(1);
        Abi::Fault report{};
        std::array<std::uint32_t, 4> result{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        std::memcpy(result.data(), words.Bytes().data(), sizeof(result));
        const bool faults = static_cast<std::uint32_t>(reason) != 0u;
        if (faults) {
            Require(report.state == Abi::FaultState::Ready && report.reason == reason && report.address == faultAddress && report.bytes == faultBytes && report.instruction == 0x1234, "BDA span read did not publish its first fault");
        } else {
            Require(report.state == Abi::FaultState::Empty, "mapped BDA span read published a fault");
        }
        Require(result == (faults && stops ? sentinel : values), faults && stops ? "faulting BDA span read continued to output a substitute value" : "BDA span read produced incorrect data");
    };
    const auto at = [&](std::uint32_t dword) { return 0xa0b0c000u + dword; };
    for (const bool stops : {true, false}) {
        for (const bool coherent : {false, true}) {
            span(spanGuest + 16, -12, 0b1010u, coherent, stops, {0, at(2), 0, at(4)});
            span(spanGuest, 4, 0b0101u, coherent, stops, {at(1), 0, at(3), 0});
            span(spanGuest + 24, 0, 0b0011u, coherent, stops, {at(6), at(7), 0, 0});
            span(spanGuest + 20, 8, 0b0001u, coherent, stops, {at(7), 0, 0, 0});
            span(spanGuest + 28, 0, 0b0101u, coherent, stops, {at(7), 0, 0, 0}, Abi::FaultReason::Unmapped, spanGuest + 36, 1);
            span(guest, 1, 0b1001u, coherent, stops, {0x55443322, 0, 0, 0}, Abi::FaultReason::Unmapped, guest + 13, 1);
            span(4, -8, 0b0001u, coherent, stops, {0, 0, 0, 0}, Abi::FaultReason::Overflow, 4, 0);
        }
    }
    Buffer lower(context, 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer upper(context, 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    lower.Bytes()[0] = std::byte{0x61};
    lower.Bytes()[1] = std::byte{0x62};
    upper.Bytes()[0] = std::byte{0x71};
    upper.Bytes()[1] = std::byte{0x72};
    upper.Bytes()[2] = std::byte{0x73};
    const auto separate = ranges;
    constexpr std::uint64_t overlapping = 0x7fff9abc0001ULL;
    ranges = {{{overlapping, overlapping + 2, lower.DeviceAddress(), Abi::Read, 0}, {overlapping + 1, overlapping + 4, upper.DeviceAddress(), Abi::Read, 0}}};
    run(overlapping, 8, 0x61, static_cast<Abi::FaultReason>(0));
    run(overlapping + 1, 8, 0x71, static_cast<Abi::FaultReason>(0));
    run(overlapping, 32, 0x73727161, static_cast<Abi::FaultReason>(0));
    run(overlapping + 2, 16, 0x7372, static_cast<Abi::FaultReason>(0));
    run(overlapping + 4, 8, 0, Abi::FaultReason::Unmapped);
    ranges = separate;
    ranges[0].permissions = 0;
    run(guest, 8, 0, Abi::FaultReason::Permission);
    RunBdaDwordWriteTests(context);
}

void RunBdaStoreExecutionTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    Buffer first(context, 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer second(context, 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    std::unique_ptr<Buffer> table;
    Buffer fault(context, Abi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    constexpr std::uint64_t guest = 0x7fff1235fffdULL;
    constexpr std::uint32_t writable = Abi::Read | Abi::Write;
    std::array<Abi::Range, 2> ranges{};
    bool runtime = false;
    const auto run = [&](const char* name, std::uint64_t address, std::uint32_t bits, bool coherent, const std::array<std::uint8_t, 5>& expected, std::initializer_list<std::uint64_t> writtenAddresses, Abi::FaultReason reason = {}, std::uint64_t faultAddress = 0, std::uint32_t faultBytes = 0) {
        const std::string label = std::string(runtime ? "BDA runtime write " : "BDA store ") + name + (coherent ? " coherent: " : ": ");
        std::memset(first.Bytes().data(), 0xcd, first.Bytes().size());
        std::memset(second.Bytes().data(), 0xcd, second.Bytes().size());
        table = MakeTable(context, ranges, ranges.size());
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        std::memset(output.Bytes().data(), 0, output.Bytes().size());
        Pipeline pipeline(context, runtime ? MakeBdaWriteTestShader(address, 0xa1b2c3d4u, bits, coherent) : MakeBdaStoreTestShader(address, 0xa1b2c3d4u, bits, coherent), {table.get(), &fault, &output});
        pipeline.Run(1);
        std::array<std::uint8_t, 5> actual{};
        std::memcpy(actual.data(), first.Bytes().data(), 2);
        std::memcpy(actual.data() + 2, second.Bytes().data(), 3);
        Require(actual == expected, label + "incorrect stored bytes or writes outside the mapped span");
        Abi::Fault report{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        if (reason == Abi::FaultReason{}) {
            Require(report.state == Abi::FaultState::Empty && report.reason == Abi::FaultReason{} && report.address == 0 && report.bytes == 0 && report.stage == 0 && report.instruction == 0 && report.reserved == 0, label + "mapped store published a fault");
        } else {
            Require(report.state == Abi::FaultState::Ready && report.reason == reason && report.address == faultAddress && report.bytes == faultBytes && report.stage == static_cast<std::uint32_t>(ShaderRecompiler::IrShaderStage::Compute) && report.instruction == 0x1234 && report.reserved == 0, label + "incorrect first fault metadata");
        }
        std::array<std::uint32_t, Abi::FaultBufferBytes / sizeof(std::uint32_t)> words{};
        std::memcpy(words.data(), fault.Bytes().data(), sizeof(words));
        Require(words[Abi::WrittenOverflowWord] == 0, label + "written-page table overflowed");
        std::set<std::uint64_t> recordedPages;
        Require(Abi::ForEachWrittenRange(words.data(), [&](std::uint64_t address, std::size_t bytes) {
            for (std::size_t offset = 0; offset < bytes; offset += std::size_t{1} << Abi::WrittenPageShift) {
                Require(recordedPages.insert((address + offset) >> Abi::WrittenPageShift).second, label + "written-page table contains duplicate pages");
            }
        }), label + "invalid written-page record");
        std::set<std::uint64_t> expectedPages;
        std::set<std::uint32_t> expectedChunks;
        for (const auto writtenAddress : writtenAddresses) {
            expectedPages.insert(writtenAddress >> Abi::WrittenPageShift);
            expectedChunks.insert(Abi::WrittenChunkKey(writtenAddress));
        }
        Require(recordedPages == expectedPages, label + "incorrect written-page set");
        Require(words[Abi::WrittenCountWord] == expectedChunks.size(), label + "incorrect written-chunk claim count");
        std::uint32_t completed = 0;
        std::memcpy(&completed, output.Bytes().data(), sizeof(completed));
        Require(completed == 1, label + "store did not finish its per-byte accesses");
    };
    for (const auto& [coherent, writes] : std::initializer_list<std::pair<bool, bool>>{{false, false}, {false, true}, {true, false}, {true, true}}) {
        runtime = writes;
        ranges = {{{guest, guest + 2, first.DeviceAddress(), writable, 0}, {guest + 2, guest + 5, second.DeviceAddress(), writable, 0}}};
        run("byte", guest + 1, 8, coherent, {0xcd, 0xd4, 0xcd, 0xcd, 0xcd}, {guest + 1});
        run("split short", guest + 1, 16, coherent, {0xcd, 0xd4, 0xc3, 0xcd, 0xcd}, {guest + 1, guest + 2});
        run("split unaligned dword", guest, 32, coherent, {0xd4, 0xc3, 0xb2, 0xa1, 0xcd}, {guest, guest + 3});
        ranges[1].permissions = Abi::Read;
        run("read-only byte", guest + 2, 8, coherent, {0xcd, 0xcd, 0xcd, 0xcd, 0xcd}, {}, Abi::FaultReason::Permission, guest + 2, 1);
        run("read-only short suffix", guest + 1, 16, coherent, {0xcd, 0xd4, 0xcd, 0xcd, 0xcd}, {guest + 1}, Abi::FaultReason::Permission, guest + 2, 1);
        run("read-only suffix", guest, 32, coherent, {0xd4, 0xc3, 0xcd, 0xcd, 0xcd}, {guest}, Abi::FaultReason::Permission, guest + 2, 1);
        ranges[0].end = guest + 1;
        ranges[1].permissions = writable;
        run("unmapped middle byte", guest, 32, coherent, {0xd4, 0xcd, 0xb2, 0xa1, 0xcd}, {guest, guest + 3}, Abi::FaultReason::Unmapped, guest + 1, 1);
        ranges = {{{guest + 3, guest + 5, first.DeviceAddress(), writable, 0}, {guest + 5, guest + 8, second.DeviceAddress(), writable, 0}}};
        run("split aligned dword", guest + 3, 32, coherent, {0xcd, 0xcd, 0xcd, 0xcd, 0xcd}, {}, Abi::FaultReason::Unmapped, guest + 3, 4);
    }
}

void RunBdaTranslationTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    namespace Table = ShaderRecompiler::BdaTable;
    constexpr std::uint32_t stride = 12;
    Buffer unused(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::array<std::vector<std::uint32_t>, 4> shaders{};
    for (std::uint32_t permission = 1; permission < shaders.size(); ++permission) shaders[permission] = MakeBdaResolveTestShader(permission);
    const auto tables = BdaTableFixtures();
    for (std::size_t fixture = 0; fixture < tables.size(); ++fixture) {
        const auto& ranges = tables[fixture];
        const auto words = Table::Build(ranges, Table::Translation::Verify);
        Table::Validate(words, Table::Translation::Verify);
        Buffer table(context, words.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(table.Bytes().data(), words.data(), words.size() * sizeof(std::uint32_t));
        const auto queries = BdaFixtureQueries(ranges, fixture == 3);
        const auto groups = static_cast<std::uint32_t>((queries.size() + 63u) / 64u);
        Buffer io(context, static_cast<std::size_t>(groups) * 64u * stride * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        for (std::uint32_t permission = 1; permission < shaders.size(); ++permission) {
            auto* data = reinterpret_cast<std::uint32_t*>(io.Bytes().data());
            for (std::size_t index = 0; index < static_cast<std::size_t>(groups) * 64u; ++index) {
                const auto& query = queries[std::min(index, queries.size() - 1u)];
                auto* slot = data + index * stride;
                std::fill(slot, slot + stride, 0xffffffffu);
                slot[0] = static_cast<std::uint32_t>(query.address);
                slot[1] = static_cast<std::uint32_t>(query.address >> 32u);
                slot[2] = query.bytes;
            }
            Pipeline pipeline(context, shaders[permission], {&table, &io, &unused});
            pipeline.Run(groups);
            for (std::size_t index = 0; index < queries.size(); ++index) {
                const auto& query = queries[index];
                const auto* slot = data + index * stride;
                const auto expected = ReferenceBdaResolve(ranges, query.address, query.bytes, permission);
                const BdaResolved legacy{slot[4] | (static_cast<std::uint64_t>(slot[5]) << 32u), slot[6]};
                const BdaResolved radix{slot[8] | (static_cast<std::uint64_t>(slot[9]) << 32u), slot[10]};
                if (legacy == expected && radix == expected) continue;
                std::ostringstream message;
                message << std::hex << "BDA translation fixture " << fixture << " permission " << permission << " address 0x" << query.address << std::dec << " bytes " << query.bytes << std::hex << ": expected 0x" << expected.result << "/" << expected.reason << ", legacy 0x" << legacy.result << "/" << legacy.reason << ", radix 0x" << radix.result << "/" << radix.reason;
                Require(false, message.str());
            }
        }
    }
    const auto previous = BdaTestTranslation();
    const auto previousRelative = BdaTestRelative();
    SetBdaTestTranslation(Table::Translation::Verify);
    SetBdaTestRelative(Table::Relative::Off);
    Buffer first(context, 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer second(context, 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    second.Bytes()[0] = std::byte{0x44};
    constexpr std::uint64_t guest = 0x7fff12340001ULL;
    const std::array<Abi::Range, 2> ranges{{{guest, guest + 3, first.DeviceAddress(), Abi::Read, 0}, {guest + 3, guest + 5, second.DeviceAddress(), Abi::Read, 0}}};
    auto words = Table::Build(ranges, Table::Translation::Verify);
    const auto entry = words[3];
    Require(words[entry] == 1u && words[entry + 1] == 0u, "the injected-mismatch fixture no longer has a one-range root entry");
    words[entry] = 2u;
    bool rejected = false;
    try {
        Table::Validate(words, Table::Translation::Verify);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Require(rejected, "host validation accepted a radix entry that selects the wrong range");
    Buffer table(context, words.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(table.Bytes().data(), words.data(), words.size() * sizeof(std::uint32_t));
    Buffer fault(context, Abi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (const std::uint32_t bits : {8u, 32u}) {
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        Pipeline pipeline(context, MakeBdaTestShader(guest, bits), {&table, &fault, &output});
        pipeline.Run(1);
        Abi::Fault report{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        Require(report.state == Abi::FaultState::Ready && report.reason == Abi::FaultReason::TranslationMismatch && report.address == guest && report.instruction == 0x1234, "APS5_BDA_ABI=verify did not fail on an injected radix mismatch");
    }
    SetBdaTestTranslation(previous);
    const auto valid = Table::Build(ranges, previous);
    Buffer validTable(context, valid.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(validTable.Bytes().data(), valid.data(), valid.size() * sizeof(std::uint32_t));
    for (const auto relative : {Table::Relative::On, Table::Relative::Verify}) {
        SetBdaTestRelative(relative);
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::uint32_t sentinel = 0xdeadbeef;
        std::memcpy(output.Bytes().data(), &sentinel, sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaCapabilityTestShader(guest, second.DeviceAddress()), {&validTable, &fault, &output});
        pipeline.Run(1);
        Abi::Fault report{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        std::uint32_t result = 0;
        std::memcpy(&result, output.Bytes().data(), sizeof(result));
        if (relative == Table::Relative::On) {
            Require(report.state == Abi::FaultState::Empty && result == 0x44u, "APS5_BDA_RELATIVE=on did not translate through its capability");
        } else {
            Require(report.state == Abi::FaultState::Ready && report.reason == Abi::FaultReason::RelativeMismatch && report.address == guest && report.instruction == 0x1234, "APS5_BDA_RELATIVE=verify did not fail on an injected capability mismatch");
        }
    }
    SetBdaTestRelative(previousRelative);
}

void RunBdaSpanHelperTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    Buffer wide(context, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    for (std::uint32_t dword = 0; dword < 8u; ++dword) {
        const std::uint32_t value = 0xa0b0c000u + dword;
        std::memcpy(wide.Bytes().data() + dword * 4u, &value, sizeof(value));
    }
    constexpr std::uint64_t spanGuest = 0x7fff56780000ULL;
    const std::array<Abi::Range, 1> ranges{{{spanGuest, spanGuest + 32, wide.DeviceAddress(), Abi::Read, 0}}};
    const auto table = MakeTable(context, ranges, ranges.size());
    Buffer fault(context, Abi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer words(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (const auto stops : {true, false}) {
        SetBdaTestSpanHelpers(ShaderRecompiler::RewriteMode::Verify, 0x100u);
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        Pipeline pipeline(context, MakeBdaSpanReadTestShader(spanGuest, 4u, 0b0101u, false, stops), {table.get(), &fault, &words});
        pipeline.Run(1);
        Abi::Fault report{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        Require(report.state == Abi::FaultState::Ready && report.reason == Abi::FaultReason::SpanMismatch && report.address == spanGuest && report.bytes == 0b0101u && report.instruction == 0x1234, "APS5_BDA_SPAN_HELPERS=verify did not fail on an injected span mismatch");
    }
    SetBdaTestSpanHelpers(ShaderRecompiler::BdaSpanHelpersSetting());
}

void RunFloatRepairTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    using ShaderRecompiler::RewriteMode;
    Buffer unused(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer fault(context, Abi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer io(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    constexpr std::uint32_t groups = 16384;
    constexpr std::uint64_t perDispatch = std::uint64_t{groups} * 64u * 256u;
    for (std::uint32_t function = 1; function <= 3u; ++function) {
        Pipeline pipeline(context, MakeRepairTestShader(function, RewriteMode::Verify, 0u), {&unused, &fault, &io});
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        for (std::uint64_t base = 0; base < (std::uint64_t{1} << 32u); base += perDispatch) {
            const auto first = static_cast<std::uint32_t>(base);
            std::memcpy(io.Bytes().data(), &first, sizeof(first));
            pipeline.Run(groups);
            Abi::Fault report{};
            std::memcpy(&report, fault.Bytes().data(), sizeof(report));
            if (report.state == Abi::FaultState::Empty) continue;
            std::ostringstream message;
            message << std::hex << "lazy and eager float repair " << function << " differ for input 0x" << static_cast<std::uint32_t>(report.address) << ": eager 0x" << static_cast<std::uint32_t>(report.address >> 32u) << ", lazy 0x" << report.bytes;
            Require(false, message.str());
        }
        Pipeline injected(context, MakeRepairTestShader(function, RewriteMode::Verify, 1u), {&unused, &fault, &io});
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::uint32_t first = 0x3f800000u;
        std::memcpy(io.Bytes().data(), &first, sizeof(first));
        injected.Run(1);
        Abi::Fault report{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        Require(report.state == Abi::FaultState::Ready && report.reason == Abi::FaultReason::RepairMismatch && report.instruction == function && (static_cast<std::uint32_t>(report.address >> 32u) ^ report.bytes) == 1u, "float repair verify did not fail on an injected mismatch");
    }
}

void RunBdaPlacementTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    namespace Placement = ShaderRecompiler::BdaPlacement;
    constexpr std::uint64_t guest = 0x7fff12340000ULL;
    constexpr std::size_t pages = 3;
    constexpr std::size_t bytes = pages * Placement::PageBytes;
    constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    Buffer host(context, bytes, usage);
    Buffer work(context, bytes, usage);
    Buffer pristine(context, bytes, usage);
    Buffer record(context, Abi::FaultBufferBytes, usage);
    Buffer table(context, sizeof(Placement::Header) + sizeof(Placement::Entry), usage);
    const auto original = [](std::size_t offset) { return static_cast<std::uint8_t>(offset * 7u + 3u); };
    const auto writePages = [&](std::uint32_t mask) {
        std::memset(record.Bytes().data(), 0, record.Bytes().size());
        auto* words = reinterpret_cast<std::uint32_t*>(record.Bytes().data());
        words[Abi::WrittenCountWord] = 1;
        words[Abi::WrittenListWord] = 9;
        words[Abi::WrittenSlotsWord + 18] = Abi::WrittenChunkKey(guest);
        words[Abi::WrittenSlotsWord + 19] = mask;
    };
    const auto run = [&](std::size_t begin, std::size_t end, bool verify) {
        Placement::Header header{};
        header.count = 1;
        const Placement::Entry entry{guest + begin, guest + end, work.DeviceAddress() + begin, pristine.DeviceAddress() + begin, host.DeviceAddress() + begin};
        std::memcpy(table.Bytes().data(), &header, sizeof(header));
        std::memcpy(table.Bytes().data() + sizeof(header), &entry, sizeof(entry));
        CommandBatch batch(context);
        RecordPlacementCopyBack(context, batch.Handle(), record.DeviceAddress(), table.DeviceAddress(), verify);
        RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
        std::memcpy(&header, table.Bytes().data(), sizeof(header));
        return header;
    };
    for (const auto begin : {std::size_t{0}, std::size_t{1}, std::size_t{5}, std::size_t{4095}, std::size_t{4096}, std::size_t{4097}, std::size_t{8191}}) {
        for (const auto end : {begin + 1u, begin + 3u, std::size_t{4096}, std::size_t{8192}, std::size_t{8193}, bytes - 1u, bytes}) {
            if (end <= begin || end > bytes) continue;
            for (std::size_t offset = 0; offset < bytes; ++offset) {
                host.Bytes()[offset] = std::byte{original(offset)};
                pristine.Bytes()[offset] = std::byte{original(offset)};
                work.Bytes()[offset] = std::byte{static_cast<std::uint8_t>(original(offset) ^ 0xffu)};
            }
            writePages(0b101u);
            static_cast<void>(run(begin, end, false));
            for (std::size_t offset = 0; offset < bytes; ++offset) {
                const bool copied = offset >= begin && offset < end && (offset / Placement::PageBytes) != 1u;
                const auto expected = copied ? static_cast<std::uint8_t>(original(offset) ^ 0xffu) : original(offset);
                if (std::to_integer<std::uint8_t>(host.Bytes()[offset]) == expected && std::to_integer<std::uint8_t>(pristine.Bytes()[offset]) == expected) continue;
                Require(false, "BDA placement copy-back of [" + std::to_string(begin) + ", " + std::to_string(end) + ") left byte " + std::to_string(offset) + " wrong");
            }
        }
    }
    for (std::size_t offset = 0; offset < bytes; ++offset) {
        host.Bytes()[offset] = std::byte{original(offset)};
        pristine.Bytes()[offset] = std::byte{original(offset)};
        work.Bytes()[offset] = std::byte{original(offset)};
    }
    work.Bytes()[10] = std::byte{0x5a};
    host.Bytes()[20] = std::byte{0xa5};
    writePages(0b001u);
    const auto checked = run(0, bytes, true);
    Require(std::to_integer<std::uint8_t>(host.Bytes()[10]) == 0x5a, "BDA placement copy-back missed a byte the shader changed");
    Require(std::to_integer<std::uint8_t>(host.Bytes()[20]) == 0xa5, "BDA placement copy-back overwrote a byte written after the copy-in");
    Require(checked.mismatches == 1 && checked.firstMismatch == guest + 20, "APS5_BDA_PLACEMENT=verify did not report the injected difference between the placement and the import");
    std::memset(table.Bytes().data(), 0, sizeof(Placement::Header));
    work.Bytes()[20] = std::byte{0xa5};
    pristine.Bytes()[20] = std::byte{0xa5};
    Require(run(0, bytes, true).mismatches == 0, "APS5_BDA_PLACEMENT=verify reported a difference after a consistent copy-back");
    ClearMirrors(context.device);
}

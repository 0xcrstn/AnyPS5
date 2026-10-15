#include "prx/libSceAgcDriver/Graphics/include/CommandSink.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PassSignatures.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace AgcDriver::Graphics {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(ChunkCommand::Count)> ChunkCommandNames{"none", "bind pipeline", "set viewport", "set scissor", "set stencil reference", "set stencil compare mask", "set stencil write mask", "set stencil op", "set cull mode", "set front face", "set depth bias", "set depth bounds", "set depth test enable", "set depth write enable", "set depth compare op", "set depth bounds test enable", "set stencil test enable", "bind descriptor set", "push constants", "bind vertex buffers", "bind index buffer", "draw", "draw indexed", "draw indirect", "draw indexed indirect", "draw indirect count", "draw indexed indirect count", "draw mesh tasks", "draw mesh tasks indirect"};

template<typename TValue>
std::uint64_t Value(TValue value) {
    static_assert(std::is_integral_v<TValue> || std::is_enum_v<TValue>);
    if constexpr (std::is_enum_v<TValue>) {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(value));
    } else if constexpr (sizeof(TValue) == sizeof(std::uint64_t)) {
        return static_cast<std::uint64_t>(value);
    } else {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(value));
    }
}

template<typename THandle>
std::uint64_t Handle(THandle handle) {
    static_assert(sizeof(THandle) == sizeof(std::uint64_t));
    return std::bit_cast<std::uint64_t>(handle);
}

template<typename THandle>
THandle FromHandle(std::uint64_t word) {
    static_assert(sizeof(THandle) == sizeof(std::uint64_t));
    return std::bit_cast<THandle>(word);
}

std::uint64_t Bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

std::uint64_t Pair(std::uint32_t low, std::uint32_t high) {
    return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
}

std::uint32_t Low(std::uint64_t word) {
    return static_cast<std::uint32_t>(word);
}

std::uint32_t High(std::uint64_t word) {
    return static_cast<std::uint32_t>(word >> 32u);
}

float Float(std::uint32_t bits) {
    return std::bit_cast<float>(bits);
}

template<typename TEnum>
TEnum Enum(std::uint64_t word) {
    return static_cast<TEnum>(static_cast<std::uint32_t>(word));
}

std::string CommandText(std::uint64_t command) {
    if (command != 0 && command < ChunkCommandNames.size()) return ChunkCommandNames[command];
    return "unknown command " + std::to_string(command);
}

}

const char* ChunkCommandName(ChunkCommand command) {
    const auto index = static_cast<std::size_t>(command);
    return index < ChunkCommandNames.size() ? ChunkCommandNames[index] : "unknown";
}

void CommandList::Append(ChunkCommand command, std::initializer_list<std::uint64_t> arguments, std::span<const std::uint64_t> tail) {
    words.push_back((static_cast<std::uint64_t>(command) << 32u) | static_cast<std::uint64_t>(arguments.size() + tail.size()));
    words.insert(words.end(), arguments.begin(), arguments.end());
    words.insert(words.end(), tail.begin(), tail.end());
}

std::string ChunkDrawName(const ChunkDraw& draw) {
    char text[192];
    std::snprintf(text, sizeof(text), "draw of program 0x%llx (%u indices or vertices, %u instances) into target 0x%llx", static_cast<unsigned long long>(draw.program), draw.indexCount, draw.instanceCount, static_cast<unsigned long long>(draw.target));
    return text;
}

std::uintptr_t CommandSink::Identity() const {
    return list != nullptr ? identity : reinterpret_cast<std::uintptr_t>(commands);
}

bool CommandSink::emit(ChunkCommand command, std::initializer_list<std::uint64_t> arguments, std::span<const std::uint64_t> tail) const {
    if (list != nullptr) {
        list->Append(command, arguments, tail);
        return true;
    }
    PassSignatures::Note(static_cast<std::uint32_t>(command), arguments, tail);
    return false;
}

void CommandSink::BindPipeline(const Context& context, VkPipelineBindPoint bindPoint, VkPipeline pipeline) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::BindPipeline, {Value(bindPoint), Handle(pipeline)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdBindPipeline, "vkCmdBindPipeline")(commands, bindPoint, pipeline);
}

void CommandSink::SetViewport(const Context& context, const VkViewport& viewport) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetViewport, {Pair(Low(Bits(viewport.x)), Low(Bits(viewport.y))), Pair(Low(Bits(viewport.width)), Low(Bits(viewport.height))), Pair(Low(Bits(viewport.minDepth)), Low(Bits(viewport.maxDepth)))})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetViewport, "vkCmdSetViewport")(commands, 0, 1, &viewport);
}

void CommandSink::SetScissor(const Context& context, const VkRect2D& scissor) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetScissor, {Pair(static_cast<std::uint32_t>(scissor.offset.x), static_cast<std::uint32_t>(scissor.offset.y)), Pair(scissor.extent.width, scissor.extent.height)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetScissor, "vkCmdSetScissor")(commands, 0, 1, &scissor);
}

void CommandSink::SetStencilReference(const Context& context, VkStencilFaceFlags faces, std::uint32_t reference) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetStencilReference, {Value(faces), Value(reference)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetStencilReference, "vkCmdSetStencilReference")(commands, faces, reference);
}

void CommandSink::SetStencilCompareMask(const Context& context, VkStencilFaceFlags faces, std::uint32_t mask) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetStencilCompareMask, {Value(faces), Value(mask)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetStencilCompareMask, "vkCmdSetStencilCompareMask")(commands, faces, mask);
}

void CommandSink::SetStencilWriteMask(const Context& context, VkStencilFaceFlags faces, std::uint32_t mask) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetStencilWriteMask, {Value(faces), Value(mask)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetStencilWriteMask, "vkCmdSetStencilWriteMask")(commands, faces, mask);
}

void CommandSink::SetStencilOp(const Context& context, VkStencilFaceFlags faces, VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetStencilOp, {Value(faces), Value(failOp), Value(passOp), Value(depthFailOp), Value(compareOp)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetStencilOp, "vkCmdSetStencilOpEXT")(commands, faces, failOp, passOp, depthFailOp, compareOp);
}

void CommandSink::SetCullMode(const Context& context, VkCullModeFlags mode) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetCullMode, {Value(mode)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetCullMode, "vkCmdSetCullModeEXT")(commands, mode);
}

void CommandSink::SetFrontFace(const Context& context, VkFrontFace face) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetFrontFace, {Value(face)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetFrontFace, "vkCmdSetFrontFaceEXT")(commands, face);
}

void CommandSink::SetDepthBias(const Context& context, float constant, float clamp, float slope) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetDepthBias, {Bits(constant), Bits(clamp), Bits(slope)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetDepthBias, "vkCmdSetDepthBias")(commands, constant, clamp, slope);
}

void CommandSink::SetDepthBounds(const Context& context, float minimum, float maximum) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetDepthBounds, {Bits(minimum), Bits(maximum)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetDepthBounds, "vkCmdSetDepthBounds")(commands, minimum, maximum);
}

void CommandSink::SetDepthTestEnable(const Context& context, VkBool32 enable) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetDepthTestEnable, {Value(enable)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetDepthTestEnable, "vkCmdSetDepthTestEnableEXT")(commands, enable);
}

void CommandSink::SetDepthWriteEnable(const Context& context, VkBool32 enable) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetDepthWriteEnable, {Value(enable)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetDepthWriteEnable, "vkCmdSetDepthWriteEnableEXT")(commands, enable);
}

void CommandSink::SetDepthCompareOp(const Context& context, VkCompareOp compare) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetDepthCompareOp, {Value(compare)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetDepthCompareOp, "vkCmdSetDepthCompareOpEXT")(commands, compare);
}

void CommandSink::SetDepthBoundsTestEnable(const Context& context, VkBool32 enable) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetDepthBoundsTestEnable, {Value(enable)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetDepthBoundsTestEnable, "vkCmdSetDepthBoundsTestEnableEXT")(commands, enable);
}

void CommandSink::SetStencilTestEnable(const Context& context, VkBool32 enable) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::SetStencilTestEnable, {Value(enable)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdSetStencilTestEnable, "vkCmdSetStencilTestEnableEXT")(commands, enable);
}

void CommandSink::BindDescriptorSet(const Context& context, VkPipelineBindPoint bindPoint, VkPipelineLayout layout, std::uint32_t index, VkDescriptorSet set, std::span<const std::uint32_t> dynamicOffsets) const {
    if (list != nullptr || PassSignatures::Active()) {
        std::vector<std::uint64_t> offsets(dynamicOffsets.begin(), dynamicOffsets.end());
        if (emit(ChunkCommand::BindDescriptorSet, {Value(bindPoint), Handle(layout), Value(index), Handle(set), Value(static_cast<std::uint32_t>(offsets.size()))}, offsets)) return;
    }
    context.Resolved(&DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, bindPoint, layout, index, 1, &set, static_cast<std::uint32_t>(dynamicOffsets.size()), dynamicOffsets.empty() ? nullptr : dynamicOffsets.data());
}

void CommandSink::PushConstants(const Context& context, VkPipelineLayout layout, VkShaderStageFlags stages, std::uint32_t offset, std::uint32_t size, const void* values) const {
    if (list != nullptr || PassSignatures::Active()) {
        std::vector<std::uint64_t> data((static_cast<std::size_t>(size) + 7u) / 8u, 0);
        if (size != 0) std::memcpy(data.data(), values, size);
        if (emit(ChunkCommand::PushConstants, {Handle(layout), Value(stages), Value(offset), Value(size)}, data)) return;
    }
    context.Resolved(&DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, layout, stages, offset, size, values);
}

void CommandSink::BindVertexBuffers(const Context& context, std::uint32_t first, std::span<const VkBuffer> buffers, std::span<const VkDeviceSize> offsets) const {
    Require(offsets.size() >= buffers.size(), "a vertex buffer bind lacks offsets");
    if (list != nullptr || PassSignatures::Active()) {
        std::vector<std::uint64_t> tail;
        tail.reserve(buffers.size() * 2u);
        for (const auto buffer : buffers) tail.push_back(Handle(buffer));
        for (std::size_t index = 0; index < buffers.size(); ++index) tail.push_back(Value(offsets[index]));
        if (emit(ChunkCommand::BindVertexBuffers, {Value(first), Value(static_cast<std::uint32_t>(buffers.size()))}, tail)) return;
    }
    context.Resolved(&DeviceFunctions::cmdBindVertexBuffers, "vkCmdBindVertexBuffers")(commands, first, static_cast<std::uint32_t>(buffers.size()), buffers.data(), offsets.data());
}

void CommandSink::BindIndexBuffer(const Context& context, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::BindIndexBuffer, {Handle(buffer), Value(offset), Value(type)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdBindIndexBuffer, "vkCmdBindIndexBuffer")(commands, buffer, offset, type);
}

void CommandSink::Draw(const Context& context, std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::Draw, {Value(vertexCount), Value(instanceCount), Value(firstVertex), Value(firstInstance)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdDraw, "vkCmdDraw")(commands, vertexCount, instanceCount, firstVertex, firstInstance);
}

void CommandSink::DrawIndexed(const Context& context, std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::DrawIndexed, {Value(indexCount), Value(instanceCount), Value(firstIndex), Value(static_cast<std::uint32_t>(vertexOffset)), Value(firstInstance)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdDrawIndexed, "vkCmdDrawIndexed")(commands, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void CommandSink::DrawIndirect(const Context& context, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::DrawIndirect, {Handle(buffer), Value(offset), Value(count), Value(stride)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, buffer, offset, count, stride);
}

void CommandSink::DrawIndexedIndirect(const Context& context, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::DrawIndexedIndirect, {Handle(buffer), Value(offset), Value(count), Value(stride)})) return;
    }
    context.Resolved(&DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, buffer, offset, count, stride);
}

void CommandSink::DrawIndirectCount(const Context& context, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, std::uint32_t maxCount, std::uint32_t stride) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::DrawIndirectCount, {Handle(buffer), Value(offset), Handle(countBuffer), Value(countOffset), Value(maxCount), Value(stride)})) return;
    }
    context.Function<PFN_vkCmdDrawIndirectCountKHR>("vkCmdDrawIndirectCountKHR")(commands, buffer, offset, countBuffer, countOffset, maxCount, stride);
}

void CommandSink::DrawIndexedIndirectCount(const Context& context, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, std::uint32_t maxCount, std::uint32_t stride) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::DrawIndexedIndirectCount, {Handle(buffer), Value(offset), Handle(countBuffer), Value(countOffset), Value(maxCount), Value(stride)})) return;
    }
    context.Function<PFN_vkCmdDrawIndexedIndirectCountKHR>("vkCmdDrawIndexedIndirectCountKHR")(commands, buffer, offset, countBuffer, countOffset, maxCount, stride);
}

void CommandSink::DrawMeshTasks(const Context& context, std::uint32_t x, std::uint32_t y, std::uint32_t z) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::DrawMeshTasks, {Value(x), Value(y), Value(z)})) return;
    }
    context.Function<PFN_vkCmdDrawMeshTasksEXT>("vkCmdDrawMeshTasksEXT")(commands, x, y, z);
}

void CommandSink::DrawMeshTasksIndirect(const Context& context, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) const {
    if (list != nullptr || PassSignatures::Active()) {
        if (emit(ChunkCommand::DrawMeshTasksIndirect, {Handle(buffer), Value(offset), Value(count), Value(stride)})) return;
    }
    context.Function<PFN_vkCmdDrawMeshTasksIndirectEXT>("vkCmdDrawMeshTasksIndirectEXT")(commands, buffer, offset, count, stride);
}

std::size_t ReplayCommands(const Context& context, VkCommandBuffer commands, std::span<const std::uint64_t> words, CommandList* canonical) {
    const CommandSink direct(commands);
    std::optional<CommandSink> copy;
    if (canonical != nullptr) copy.emplace(*canonical, 0);
    const auto issue = [&](const auto& call) {
        if (commands != VK_NULL_HANDLE) call(direct);
        if (copy) call(*copy);
    };
    std::size_t position = 0;
    std::size_t replayed = 0;
    while (position < words.size()) {
        const auto header = words[position++];
        const auto command = header >> 32u;
        const auto count = static_cast<std::size_t>(header & 0xffffffffu);
        if (count > words.size() - position) throw std::runtime_error("chunk command list: " + CommandText(command) + " runs past the end of the list");
        const auto args = words.subspan(position, count);
        position += count;
        const auto expect = [&](std::size_t wanted) {
            if (args.size() != wanted) throw std::runtime_error("chunk command list: " + CommandText(command) + " carries " + std::to_string(args.size()) + " argument words, expected " + std::to_string(wanted));
        };
        switch (static_cast<ChunkCommand>(command)) {
        case ChunkCommand::BindPipeline:
            expect(2);
            issue([&](const CommandSink& sink) { sink.BindPipeline(context, Enum<VkPipelineBindPoint>(args[0]), FromHandle<VkPipeline>(args[1])); });
            break;
        case ChunkCommand::SetViewport: {
            expect(3);
            const VkViewport viewport{Float(Low(args[0])), Float(High(args[0])), Float(Low(args[1])), Float(High(args[1])), Float(Low(args[2])), Float(High(args[2]))};
            issue([&](const CommandSink& sink) { sink.SetViewport(context, viewport); });
            break;
        }
        case ChunkCommand::SetScissor: {
            expect(2);
            const VkRect2D scissor{{static_cast<std::int32_t>(Low(args[0])), static_cast<std::int32_t>(High(args[0]))}, {Low(args[1]), High(args[1])}};
            issue([&](const CommandSink& sink) { sink.SetScissor(context, scissor); });
            break;
        }
        case ChunkCommand::SetStencilReference:
            expect(2);
            issue([&](const CommandSink& sink) { sink.SetStencilReference(context, Low(args[0]), Low(args[1])); });
            break;
        case ChunkCommand::SetStencilCompareMask:
            expect(2);
            issue([&](const CommandSink& sink) { sink.SetStencilCompareMask(context, Low(args[0]), Low(args[1])); });
            break;
        case ChunkCommand::SetStencilWriteMask:
            expect(2);
            issue([&](const CommandSink& sink) { sink.SetStencilWriteMask(context, Low(args[0]), Low(args[1])); });
            break;
        case ChunkCommand::SetStencilOp:
            expect(5);
            issue([&](const CommandSink& sink) { sink.SetStencilOp(context, Low(args[0]), Enum<VkStencilOp>(args[1]), Enum<VkStencilOp>(args[2]), Enum<VkStencilOp>(args[3]), Enum<VkCompareOp>(args[4])); });
            break;
        case ChunkCommand::SetCullMode:
            expect(1);
            issue([&](const CommandSink& sink) { sink.SetCullMode(context, Low(args[0])); });
            break;
        case ChunkCommand::SetFrontFace:
            expect(1);
            issue([&](const CommandSink& sink) { sink.SetFrontFace(context, Enum<VkFrontFace>(args[0])); });
            break;
        case ChunkCommand::SetDepthBias:
            expect(3);
            issue([&](const CommandSink& sink) { sink.SetDepthBias(context, Float(Low(args[0])), Float(Low(args[1])), Float(Low(args[2]))); });
            break;
        case ChunkCommand::SetDepthBounds:
            expect(2);
            issue([&](const CommandSink& sink) { sink.SetDepthBounds(context, Float(Low(args[0])), Float(Low(args[1]))); });
            break;
        case ChunkCommand::SetDepthTestEnable:
            expect(1);
            issue([&](const CommandSink& sink) { sink.SetDepthTestEnable(context, Low(args[0])); });
            break;
        case ChunkCommand::SetDepthWriteEnable:
            expect(1);
            issue([&](const CommandSink& sink) { sink.SetDepthWriteEnable(context, Low(args[0])); });
            break;
        case ChunkCommand::SetDepthCompareOp:
            expect(1);
            issue([&](const CommandSink& sink) { sink.SetDepthCompareOp(context, Enum<VkCompareOp>(args[0])); });
            break;
        case ChunkCommand::SetDepthBoundsTestEnable:
            expect(1);
            issue([&](const CommandSink& sink) { sink.SetDepthBoundsTestEnable(context, Low(args[0])); });
            break;
        case ChunkCommand::SetStencilTestEnable:
            expect(1);
            issue([&](const CommandSink& sink) { sink.SetStencilTestEnable(context, Low(args[0])); });
            break;
        case ChunkCommand::BindDescriptorSet: {
            if (args.size() < 5) expect(5);
            const auto count = static_cast<std::size_t>(Low(args[4]));
            expect(5 + count);
            std::vector<std::uint32_t> offsets(count);
            for (std::size_t offset = 0; offset < count; ++offset) offsets[offset] = Low(args[5 + offset]);
            issue([&](const CommandSink& sink) { sink.BindDescriptorSet(context, Enum<VkPipelineBindPoint>(args[0]), FromHandle<VkPipelineLayout>(args[1]), Low(args[2]), FromHandle<VkDescriptorSet>(args[3]), offsets); });
            break;
        }
        case ChunkCommand::PushConstants: {
            if (args.size() < 4) expect(4);
            const auto size = Low(args[3]);
            expect(4 + (static_cast<std::size_t>(size) + 7u) / 8u);
            std::vector<std::byte> values(size);
            if (size != 0) std::memcpy(values.data(), args.data() + 4, size);
            issue([&](const CommandSink& sink) { sink.PushConstants(context, FromHandle<VkPipelineLayout>(args[0]), Low(args[1]), Low(args[2]), size, values.data()); });
            break;
        }
        case ChunkCommand::BindVertexBuffers: {
            if (args.size() < 2) expect(2);
            const auto bound = static_cast<std::size_t>(Low(args[1]));
            expect(2 + bound * 2u);
            std::vector<VkBuffer> buffers(bound);
            std::vector<VkDeviceSize> offsets(bound);
            for (std::size_t i = 0; i < bound; ++i) {
                buffers[i] = FromHandle<VkBuffer>(args[2 + i]);
                offsets[i] = args[2 + bound + i];
            }
            issue([&](const CommandSink& sink) { sink.BindVertexBuffers(context, Low(args[0]), buffers, offsets); });
            break;
        }
        case ChunkCommand::BindIndexBuffer:
            expect(3);
            issue([&](const CommandSink& sink) { sink.BindIndexBuffer(context, FromHandle<VkBuffer>(args[0]), args[1], Enum<VkIndexType>(args[2])); });
            break;
        case ChunkCommand::Draw:
            expect(4);
            issue([&](const CommandSink& sink) { sink.Draw(context, Low(args[0]), Low(args[1]), Low(args[2]), Low(args[3])); });
            break;
        case ChunkCommand::DrawIndexed:
            expect(5);
            issue([&](const CommandSink& sink) { sink.DrawIndexed(context, Low(args[0]), Low(args[1]), Low(args[2]), static_cast<std::int32_t>(Low(args[3])), Low(args[4])); });
            break;
        case ChunkCommand::DrawIndirect:
            expect(4);
            issue([&](const CommandSink& sink) { sink.DrawIndirect(context, FromHandle<VkBuffer>(args[0]), args[1], Low(args[2]), Low(args[3])); });
            break;
        case ChunkCommand::DrawIndexedIndirect:
            expect(4);
            issue([&](const CommandSink& sink) { sink.DrawIndexedIndirect(context, FromHandle<VkBuffer>(args[0]), args[1], Low(args[2]), Low(args[3])); });
            break;
        case ChunkCommand::DrawIndirectCount:
            expect(6);
            issue([&](const CommandSink& sink) { sink.DrawIndirectCount(context, FromHandle<VkBuffer>(args[0]), args[1], FromHandle<VkBuffer>(args[2]), args[3], Low(args[4]), Low(args[5])); });
            break;
        case ChunkCommand::DrawIndexedIndirectCount:
            expect(6);
            issue([&](const CommandSink& sink) { sink.DrawIndexedIndirectCount(context, FromHandle<VkBuffer>(args[0]), args[1], FromHandle<VkBuffer>(args[2]), args[3], Low(args[4]), Low(args[5])); });
            break;
        case ChunkCommand::DrawMeshTasks:
            expect(3);
            issue([&](const CommandSink& sink) { sink.DrawMeshTasks(context, Low(args[0]), Low(args[1]), Low(args[2])); });
            break;
        case ChunkCommand::DrawMeshTasksIndirect:
            expect(4);
            issue([&](const CommandSink& sink) { sink.DrawMeshTasksIndirect(context, FromHandle<VkBuffer>(args[0]), args[1], Low(args[2]), Low(args[3])); });
            break;
        default:
            throw std::runtime_error("chunk command list: " + CommandText(command));
        }
        ++replayed;
    }
    return replayed;
}

void CompareChunkCommands(std::span<const std::uint64_t> issued, std::span<const std::size_t> issuedDraws, std::span<const std::uint64_t> recorded, std::span<const std::size_t> recordedDraws, std::span<const ChunkDraw> draws) {
    if (issuedDraws.size() != draws.size()) throw std::logic_error("APS5_VERIFY_CHUNKED_RECORDING: the chunk's draw marks and draw identities differ in count");
    const auto segment = [](std::span<const std::uint64_t> words, std::span<const std::size_t> starts, std::size_t index) {
        if (index >= starts.size()) return std::span<const std::uint64_t>{};
        const auto end = index + 1 < starts.size() ? starts[index + 1] : words.size();
        if (starts[index] > end || end > words.size()) throw std::logic_error("APS5_VERIFY_CHUNKED_RECORDING: a draw mark lies outside its command list");
        return words.subspan(starts[index], end - starts[index]);
    };
    const auto drawCount = std::max(issuedDraws.size(), recordedDraws.size());
    for (std::size_t index = 0; index < drawCount; ++index) {
        const auto expected = segment(issued, issuedDraws, index);
        const auto actual = segment(recorded, recordedDraws, index);
        if (index < issuedDraws.size() && index < recordedDraws.size() && std::equal(expected.begin(), expected.end(), actual.begin(), actual.end())) continue;
        const auto name = index < draws.size() ? ChunkDrawName(draws[index]) : std::string("a draw encode never issued");
        std::size_t word = 0;
        while (word < expected.size() && word < actual.size() && expected[word] == actual[word]) ++word;
        char detail[160];
        std::snprintf(detail, sizeof(detail), " (draw %zu of the chunk: %zu words issued, %zu recorded, first difference at word %zu)", index, expected.size(), actual.size(), word);
        throw std::logic_error("APS5_VERIFY_CHUNKED_RECORDING: the recording worker recorded other commands than encode issued for the " + name + detail);
    }
}

}

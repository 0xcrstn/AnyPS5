#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDSINK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDSINK_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::Graphics {

enum class ChunkCommand : std::uint32_t {
    BindPipeline = 1,
    SetViewport,
    SetScissor,
    SetStencilReference,
    SetStencilCompareMask,
    SetStencilWriteMask,
    SetStencilOp,
    SetCullMode,
    SetFrontFace,
    SetDepthBias,
    SetDepthBounds,
    SetDepthTestEnable,
    SetDepthWriteEnable,
    SetDepthCompareOp,
    SetDepthBoundsTestEnable,
    SetStencilTestEnable,
    BindDescriptorSet,
    PushConstants,
    BindVertexBuffers,
    BindIndexBuffer,
    Draw,
    DrawIndexed,
    DrawIndirect,
    DrawIndexedIndirect,
    DrawIndirectCount,
    DrawIndexedIndirectCount,
    DrawMeshTasks,
    DrawMeshTasksIndirect,
    Count
};

const char* ChunkCommandName(ChunkCommand command);

class CommandList {
public:
    void Clear() { words.clear(); }
    bool Empty() const { return words.empty(); }
    std::span<const std::uint64_t> Words() const { return words; }
    void Append(ChunkCommand command, std::initializer_list<std::uint64_t> arguments, std::span<const std::uint64_t> tail = {});
    void AppendWords(std::span<const std::uint64_t> appended) { words.insert(words.end(), appended.begin(), appended.end()); }

private:
    std::vector<std::uint64_t> words;
};

struct ChunkDraw {
    std::uint64_t program = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint64_t target = 0;
};

std::string ChunkDrawName(const ChunkDraw& draw);

struct ChunkInheritance {
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    bool rendering = false;
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

class CommandSink {
public:
    CommandSink(VkCommandBuffer commands) : commands(commands) {}
    CommandSink(CommandList& list, std::uintptr_t identity) : list(&list), identity(identity) {}
    bool Chunked() const { return list != nullptr; }
    VkCommandBuffer Direct() const { return commands; }
    CommandList* List() const { return list; }
    std::uintptr_t Identity() const;
    void BindPipeline(const Context& context, VkPipelineBindPoint bindPoint, VkPipeline pipeline) const;
    void SetViewport(const Context& context, const VkViewport& viewport) const;
    void SetScissor(const Context& context, const VkRect2D& scissor) const;
    void SetStencilReference(const Context& context, VkStencilFaceFlags faces, std::uint32_t reference) const;
    void SetStencilCompareMask(const Context& context, VkStencilFaceFlags faces, std::uint32_t mask) const;
    void SetStencilWriteMask(const Context& context, VkStencilFaceFlags faces, std::uint32_t mask) const;
    void SetStencilOp(const Context& context, VkStencilFaceFlags faces, VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp) const;
    void SetCullMode(const Context& context, VkCullModeFlags mode) const;
    void SetFrontFace(const Context& context, VkFrontFace face) const;
    void SetDepthBias(const Context& context, float constant, float clamp, float slope) const;
    void SetDepthBounds(const Context& context, float minimum, float maximum) const;
    void SetDepthTestEnable(const Context& context, VkBool32 enable) const;
    void SetDepthWriteEnable(const Context& context, VkBool32 enable) const;
    void SetDepthCompareOp(const Context& context, VkCompareOp compare) const;
    void SetDepthBoundsTestEnable(const Context& context, VkBool32 enable) const;
    void SetStencilTestEnable(const Context& context, VkBool32 enable) const;
    void BindDescriptorSet(const Context& context, VkPipelineBindPoint bindPoint, VkPipelineLayout layout, std::uint32_t index, VkDescriptorSet set, std::span<const std::uint32_t> dynamicOffsets = {}) const;
    void PushConstants(const Context& context, VkPipelineLayout layout, VkShaderStageFlags stages, std::uint32_t offset, std::uint32_t size, const void* values) const;
    void BindVertexBuffers(const Context& context, std::uint32_t first, std::span<const VkBuffer> buffers, std::span<const VkDeviceSize> offsets) const;
    void BindIndexBuffer(const Context& context, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) const;
    void Draw(const Context& context, std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance) const;
    void DrawIndexed(const Context& context, std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance) const;
    void DrawIndirect(const Context& context, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) const;
    void DrawIndexedIndirect(const Context& context, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) const;
    void DrawIndirectCount(const Context& context, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, std::uint32_t maxCount, std::uint32_t stride) const;
    void DrawIndexedIndirectCount(const Context& context, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, std::uint32_t maxCount, std::uint32_t stride) const;
    void DrawMeshTasks(const Context& context, std::uint32_t x, std::uint32_t y, std::uint32_t z) const;
    void DrawMeshTasksIndirect(const Context& context, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) const;

private:
    VkCommandBuffer commands = VK_NULL_HANDLE;
    CommandList* list = nullptr;
    std::uintptr_t identity = 0;
};

std::size_t ReplayCommands(const Context& context, VkCommandBuffer commands, std::span<const std::uint64_t> words, CommandList* canonical);
void CompareChunkCommands(std::span<const std::uint64_t> issued, std::span<const std::size_t> issuedDraws, std::span<const std::uint64_t> recorded, std::span<const std::size_t> recordedDraws, std::span<const ChunkDraw> draws);

}

#endif

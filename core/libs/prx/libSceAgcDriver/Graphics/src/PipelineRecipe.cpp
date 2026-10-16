#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipe.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint32_t Magic = 0x52505041u;
constexpr std::size_t HeaderBytes = 44;

struct AnyField {
    template<typename TField>
    operator TField() const;
};

template<typename TStruct, std::size_t... Index>
constexpr bool bracedWith(std::index_sequence<Index...>) {
    return requires { TStruct{(static_cast<void>(Index), AnyField{})...}; };
}

template<typename TStruct, std::size_t Count = 0>
constexpr std::size_t fieldCount() {
    if constexpr (bracedWith<TStruct>(std::make_index_sequence<Count + 1>{})) return fieldCount<TStruct, Count + 1>();
    else return Count;
}

template<typename TState>
constexpr std::size_t expectedStateFields() {
    if constexpr (requires(const TState& state) { state.reversedBlend; }) return 51;
    else return 50;
}

static_assert(fieldCount<State>() == expectedStateFields<State>(), "State changed: update the canonical pipeline recipe codec");
static_assert(fieldCount<ShaderStages>() == 8, "ShaderStages changed: update the pipeline recipe codec");
static_assert(fieldCount<ShaderRecompiler::MeshConfiguration>() == 10, "MeshConfiguration changed: update the pipeline recipe codec");
static_assert(fieldCount<ShaderRecompiler::TessellationConfiguration>() == 10, "TessellationConfiguration changed: update the pipeline recipe codec");
static_assert(fieldCount<VkStencilOpState>() == 7 && fieldCount<VkPipelineColorBlendAttachmentState>() == 8, "Vulkan state changed: update the pipeline recipe codec");
static_assert(fieldCount<VkVertexInputBindingDescription>() == 3 && fieldCount<VkVertexInputAttributeDescription>() == 4, "Vulkan vertex input changed: update the pipeline recipe codec");

class PreflightBudget {
public:
    void Encoded(std::size_t count, std::size_t elementBytes) {
        if (count > (PipelineRecipeMaxBytes - minimumBytes) / elementBytes) throw std::length_error("pipeline recipe minimum size exceeds byte limit");
        minimumBytes += count * elementBytes;
    }

    template<typename TValue>
    void Metadata(const std::vector<TValue>& values, std::size_t maximum = 65536u) {
        if (values.size() > maximum || values.size() > metadataRemaining / sizeof(TValue)) throw std::length_error("pipeline recipe metadata exceeds limit");
        metadataRemaining -= values.size() * sizeof(TValue);
    }

    [[nodiscard]] std::size_t Minimum() const { return minimumBytes; }

private:
    std::size_t minimumBytes = HeaderBytes;
    std::size_t metadataRemaining = PipelineRecipeMaxBytes;
};

template<typename TSpirv>
std::size_t wordCount(const TSpirv& spirv) {
    if constexpr (requires { spirv.WordCount(); }) return spirv.WordCount();
    else return spirv.size();
}

template<typename TBinding>
void bindingMetadata(PreflightBudget& budget, const TBinding& binding) {
    if (binding.count > 1048576u) throw std::length_error("pipeline recipe descriptor count exceeds limit");
    budget.Metadata(binding.guestDescriptor, 1048576u);
    budget.Metadata(binding.samplerDepthCompare);
    budget.Metadata(binding.imageWritten);
    budget.Metadata(binding.imageDepthCompare);
    budget.Metadata(binding.imageAtomic);
    budget.Metadata(binding.imageAtomic64);
    budget.Metadata(binding.imageShapes);
    budget.Metadata(binding.bufferAtomic);
    budget.Metadata(binding.bufferWritten);
    budget.Metadata(binding.samplerUnnormalized);
    budget.Metadata(binding.samplerIntegerBorder);
    budget.Metadata(binding.imageUnnormalized);
    budget.Metadata(binding.imageUnnormalizedBaseLevel);
    budget.Metadata(binding.imageSamplers);
    budget.Metadata(binding.bufferRead);
    if constexpr (requires { binding.bufferReadRanges; }) {
        budget.Metadata(binding.bufferReadRanges);
        for (const auto& ranges : binding.bufferReadRanges) {
            budget.Metadata(ranges.ranges);
            for (const auto& range : ranges.ranges) budget.Metadata(range.indexExpression);
        }
    }
}

template<typename TResult>
void shaderMetadata(PreflightBudget& budget, const TResult& shader) {
    budget.Metadata(shader.vertexInputPatches, 4096u);
    budget.Metadata(shader.runtimeImageResources);
    budget.Metadata(shader.vertexInputs, 4096u);
    budget.Metadata(shader.parameterExports, 4096u);
    budget.Metadata(shader.fragmentParameters, 4096u);
    budget.Metadata(shader.stencilReferences, 4096u);
    budget.Metadata(shader.specialization, 4096u);
    budget.Metadata(shader.bindings, 4096u);
    budget.Metadata(shader.pushConstants, PipelinePushConstantBytes);
    budget.Metadata(shader.vertexAttributes, 4096u);
    budget.Metadata(shader.imageTablePoison, 4096u);
    budget.Metadata(shader.imageTableRanges);
    budget.Metadata(shader.imageTableReadRanges);
    for (const auto& ranges : shader.imageTableReadRanges) budget.Metadata(ranges);
    for (const auto& binding : shader.bindings) bindingMetadata(budget, binding);
    if constexpr (requires { shader.shaderDataUserWords; }) budget.Metadata(shader.shaderDataUserWords);
    if constexpr (requires { shader.bufferBaseSlots; }) budget.Metadata(shader.bufferBaseSlots);
    budget.Encoded(shader.specialization.size(), 8u);
    budget.Encoded(shader.bindings.size(), 20u);
    budget.Encoded(shader.pushConstants.size(), 1u);
}

void stateBounds(const State& state, const VertexInputLayout& vertexInput, std::size_t mainWords, std::size_t fragmentWords) {
    if (state.colors.size() > 8 || state.blends.size() > 8 || vertexInput.bindings.size() > 64 || vertexInput.attributes.size() > 64 || mainWords > 4096 || fragmentWords > 4096) throw std::length_error("pipeline recipe state or layout count exceeds limit");
}

class Writer {
public:
    template<typename TValue>
    void Value(TValue& value) {
        if constexpr (std::is_enum_v<TValue>) {
            auto raw = static_cast<std::uint32_t>(value);
            Value(raw);
        } else if constexpr (std::is_same_v<TValue, bool>) {
            auto raw = static_cast<std::uint8_t>(value);
            Value(raw);
        } else if constexpr (std::is_same_v<TValue, float>) {
            auto raw = std::bit_cast<std::uint32_t>(value);
            Value(raw);
        } else {
            static_assert(std::is_unsigned_v<TValue>);
            if (out.size() > PipelineRecipeMaxBytes - sizeof(TValue)) throw std::length_error("pipeline recipe exceeds byte limit");
            for (std::size_t index = 0; index < sizeof(TValue); ++index) out.push_back(static_cast<std::byte>((value >> (index * 8u)) & 0xffu));
        }
    }

    template<typename TValue>
    void List(std::vector<TValue>& values, std::size_t maximum) {
        if (values.size() > maximum) throw std::length_error("pipeline recipe count exceeds limit");
        auto count = static_cast<std::uint32_t>(values.size());
        Value(count);
    }

    void Bytes(std::vector<std::byte>& bytes) {
        List(bytes, PipelineRecipeMaxBytes);
        if (bytes.size() > PipelineRecipeMaxBytes - out.size()) throw std::length_error("pipeline recipe exceeds byte limit");
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    void Text(std::string& text) {
        std::vector<std::byte> bytes;
        bytes.reserve(text.size());
        for (const auto character : text) bytes.push_back(static_cast<std::byte>(character));
        Bytes(bytes);
    }

    std::vector<std::byte> out;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes(bytes) {}

    template<typename TValue>
    void Value(TValue& value) {
        if constexpr (std::is_enum_v<TValue>) {
            std::uint32_t raw = 0;
            Value(raw);
            value = static_cast<TValue>(raw);
        } else if constexpr (std::is_same_v<TValue, bool>) {
            std::uint8_t raw = 0;
            Value(raw);
            if (raw > 1u) throw std::runtime_error("invalid pipeline recipe boolean");
            value = raw != 0;
        } else if constexpr (std::is_same_v<TValue, float>) {
            std::uint32_t raw = 0;
            Value(raw);
            value = std::bit_cast<float>(raw);
        } else {
            static_assert(std::is_unsigned_v<TValue>);
            if (sizeof(TValue) > bytes.size() - cursor) throw std::runtime_error("truncated pipeline recipe");
            value = 0;
            for (std::size_t index = 0; index < sizeof(TValue); ++index) value |= static_cast<TValue>(std::to_integer<std::uint8_t>(bytes[cursor++])) << (index * 8u);
        }
    }

    template<typename TValue>
    void List(std::vector<TValue>& values, std::size_t maximum) {
        std::uint32_t count = 0;
        Value(count);
        if (count > maximum || count > bytes.size() - cursor) throw std::runtime_error("invalid pipeline recipe count");
        values.resize(count);
    }

    void Bytes(std::vector<std::byte>& out) {
        std::uint32_t count = 0;
        Value(count);
        if (count > bytes.size() - cursor) throw std::runtime_error("truncated pipeline shader");
        out.assign(bytes.begin() + cursor, bytes.begin() + cursor + count);
        cursor += count;
    }

    void SkipBytes() {
        std::uint32_t count = 0;
        Value(count);
        if (count > bytes.size() - cursor) throw std::runtime_error("truncated pipeline shader");
        cursor += count;
    }

    void Text(std::string& text) {
        std::uint32_t count = 0;
        Value(count);
        if (count > 64 || count > bytes.size() - cursor) throw std::runtime_error("invalid pipeline input identity length");
        text.clear();
        for (std::uint32_t index = 0; index < count; ++index) text.push_back(static_cast<char>(std::to_integer<std::uint8_t>(bytes[cursor++])));
    }

    [[nodiscard]] bool Done() const { return cursor == bytes.size(); }

private:
    std::span<const std::byte> bytes;
    std::size_t cursor = 0;
};

std::uint64_t hashBytes(std::span<const std::byte> bytes, std::uint64_t seed) {
    auto hash = seed;
    for (const auto byte : bytes) hash = (hash ^ std::to_integer<std::uint8_t>(byte)) * 0x100000001b3ull;
    return hash;
}

template<typename TCodec, typename TValue, typename TTransfer>
void optional(TCodec& codec, std::optional<TValue>& value, TTransfer transfer) {
    bool present = value.has_value();
    codec.Value(present);
    if (present) {
        if (!value) value.emplace();
        transfer(codec, *value);
    } else value.reset();
}

template<typename TCodec>
void mesh(TCodec& codec, ShaderRecompiler::MeshConfiguration& value) {
    for (auto* field : {&value.inputPrimitive, &value.primitivesPerGroup, &value.verticesPerGroup, &value.maxVertices, &value.maxPrimitives, &value.threadsPerGroup, &value.ldsSizeDwords, &value.provokingVertex, &value.esgsItemSize, &value.outputPrimitiveSize}) codec.Value(*field);
}

template<typename TCodec>
void tessellation(TCodec& codec, ShaderRecompiler::TessellationConfiguration& value) {
    for (auto* field : {&value.inputControlPoints, &value.outputControlPoints, &value.domain, &value.partitioning, &value.outputTopology, &value.patchesPerGroup, &value.primitiveGroupSize, &value.instancePatches}) codec.Value(*field);
    codec.Value(value.breakAtInstance);
    codec.Value(value.drawParameters);
}

template<typename TCodec>
void stencil(TCodec& codec, VkStencilOpState& value) {
    codec.Value(value.failOp);
    codec.Value(value.passOp);
    codec.Value(value.depthFailOp);
    codec.Value(value.compareOp);
    codec.Value(value.compareMask);
    codec.Value(value.writeMask);
    codec.Value(value.reference);
}

template<typename TCodec>
void blend(TCodec& codec, VkPipelineColorBlendAttachmentState& value) {
    codec.Value(value.blendEnable);
    codec.Value(value.srcColorBlendFactor);
    codec.Value(value.dstColorBlendFactor);
    codec.Value(value.colorBlendOp);
    codec.Value(value.srcAlphaBlendFactor);
    codec.Value(value.dstAlphaBlendFactor);
    codec.Value(value.alphaBlendOp);
    codec.Value(value.colorWriteMask);
}

template<typename TCodec, typename TState>
void reversedBlend(TCodec& codec, TState& state) {
    if constexpr (requires { state.reversedBlend; }) codec.Value(state.reversedBlend);
    else {
        std::uint32_t value = 0;
        codec.Value(value);
        if (value != 0) throw std::runtime_error("pipeline recipe uses unsupported reversed blend state");
    }
}

template<typename TCodec>
void stateFields(TCodec& codec, State& state) {
    codec.Value(state.stages.path);
    codec.Value(state.stages.vertexWaveSize);
    codec.Value(state.stages.fragmentWaveSize);
    optional(codec, state.stages.mesh, mesh<TCodec>);
    optional(codec, state.stages.tessellation, tessellation<TCodec>);
    optional(codec, state.depth, [](TCodec& io, DepthTarget& value) { io.Value(value.format); io.Value(value.samples); });
    codec.Value(state.depthTest);
    codec.Value(state.depthWrite);
    codec.Value(state.depthCompare);
    codec.Value(state.depthBoundsTest);
    codec.Value(state.depthBias);
    codec.Value(state.depthBiasClamp);
    codec.Value(state.depthBiasPerFace);
    codec.Value(state.stencilTest);
    stencil(codec, state.stencilFront);
    stencil(codec, state.stencilBack);
    optional(codec, state.stencilReplayReference, [](TCodec& io, std::uint32_t& value) { io.Value(value); });
    codec.List(state.colors, 8u);
    for (auto& color : state.colors) {
        codec.Value(color.format);
        codec.Value(color.exportIndex);
        codec.Value(color.componentMapping);
    }
    codec.List(state.blends, 8u);
    for (auto& value : state.blends) blend(codec, value);
    for (auto& value : state.blendConstants) codec.Value(value);
    codec.Value(state.hasColorTarget);
    codec.Value(state.rectList);
    codec.Value(state.dualSourceBlend);
    reversedBlend(codec, state);
    codec.Value(state.topology);
    codec.Value(state.primitiveRestart);
    codec.Value(state.negativeOneToOne);
    codec.Value(state.depthClamp);
    codec.Value(state.conservativeRasterization);
    codec.Value(state.cullMode);
    codec.Value(state.frontFace);
    codec.Value(state.provokingVertexMode);
    codec.Value(state.samples);
    for (auto& value : state.sampleLocations) codec.Value(value);
    codec.Value(state.customSampleLocations);
    codec.Value(state.sampleLocationGrid);
    codec.Value(state.perSampleShading);
    for (auto& value : state.quadSampleMask) codec.Value(value);
    codec.Value(state.pixelEarlyZ);
}

template<typename TSpirv>
TSpirv resolveSpirv(const TSpirv& spirv) {
    if constexpr (requires { spirv.Resolved(); }) return spirv.Resolved();
    else return spirv;
}

ShaderRecompiler::RecompileResult canonicalShader(const ShaderRecompiler::RecompileResult& shader) {
    auto result = shader;
    result.spirv = resolveSpirv(shader.spirv);
    result.variantId = 0;
    result.specializationId = 0;
    result.cacheHit = false;
    std::sort(result.specialization.begin(), result.specialization.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    for (std::size_t index = 1; index < result.specialization.size(); ++index) {
        if (result.specialization[index - 1].id == result.specialization[index].id) throw std::runtime_error("duplicate pipeline specialization ID");
    }
    std::fill(result.pushConstants.begin(), result.pushConstants.end(), std::byte{0});
    for (auto& binding : result.bindings) binding.guestDescriptor.clear();
    result.vertexAttributes.clear();
    result.poisonedSrtReads = 0;
    result.imageTablePoison.clear();
    result.imageTableRanges.clear();
    result.imageTableReadRanges.clear();
    result.imageTableShader = 0;
    result.imageTableFaults = 0;
    return result;
}

template<typename TShader>
std::size_t pushBytes(const TShader& shader) {
    if constexpr (requires { shader.pushConstants; }) {
        if (!shader.pushConstants.empty()) {
            if (shader.pushConstants.size() != shader.program->pushConstants.size()) throw std::runtime_error("pipeline push override size differs from the compiled program");
            return shader.pushConstants.size();
        }
    }
    return shader.program->pushConstants.size();
}

template<typename TShader>
void setPushBytes(TShader& shader, std::span<const std::byte> bytes) {
    if constexpr (requires { shader.pushConstants = bytes; }) shader.pushConstants = bytes;
}

bool emptyTableBinding(const ShaderRecompiler::DescriptorBinding& binding) {
    const bool table = (binding.role == ShaderRecompiler::DescriptorRole::ImageTable && binding.kind == ShaderRecompiler::DescriptorKind::SampledImage) || (binding.role == ShaderRecompiler::DescriptorRole::SamplerTable && binding.kind == ShaderRecompiler::DescriptorKind::Sampler);
    return table && !binding.imageShape && binding.samplerDepthCompare.empty() && binding.imageWritten.empty() && binding.imageDepthCompare.empty() && binding.imageAtomic.empty() && binding.imageAtomic64.empty() && binding.imageShapes.empty() && binding.bufferAtomic.empty() && binding.bufferWritten.empty() && binding.samplerUnnormalized.empty() && binding.samplerIntegerBorder.empty() && binding.imageUnnormalized.empty() && binding.imageUnnormalizedBaseLevel.empty() && binding.imageSamplers.empty() && binding.bufferRead.empty() && binding.bufferReadRanges.empty();
}

bool validShader(const ShaderRecompiler::RecompileResult& result) {
    const auto words = result.spirv.Words();
    if (result.runtimeAbiVersion != ShaderRecompiler::RuntimeAbi::Version || words.size() < 5 || words[0] != 0x07230203u || words[3] == 0 || words[4] != 0) return false;
    if (result.pushConstants.size() > PipelinePushConstantBytes || result.bindings.size() > 4096 || result.specialization.size() > 4096) return false;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        if (count == 0 || count > words.size() - cursor) return false;
        cursor += count;
    }
    for (std::size_t index = 1; index < result.specialization.size(); ++index) {
        if (result.specialization[index - 1].id >= result.specialization[index].id) return false;
    }
    for (const auto& binding : result.bindings) {
        if (binding.descriptorSet > 1 || (binding.count == 0 && !emptyTableBinding(binding)) || binding.count > 1048576u || !binding.guestDescriptor.empty()) return false;
        if (static_cast<std::uint32_t>(binding.kind) > static_cast<std::uint32_t>(ShaderRecompiler::DescriptorKind::Sampler) || static_cast<std::uint32_t>(binding.role) > static_cast<std::uint32_t>(ShaderRecompiler::DescriptorRole::ImageTableMap)) return false;
    }
    return true;
}

void validate(const PipelineRecipe& recipe, bool native = true) {
    if (std::endian::native != std::endian::little) throw std::runtime_error("pipeline recipe shader codec requires little endian");
    if (recipe.stages.empty() || recipe.stages.size() > 8 || recipe.vertexInput.bindings.size() > 64 || recipe.vertexInput.attributes.size() > 64 || recipe.mainLayoutKey.size() > 4096 || recipe.fragmentLayoutKey.size() > 4096) throw std::runtime_error("invalid pipeline recipe layout");
    std::set<VkShaderStageFlagBits> stages;
    for (const auto& stage : recipe.stages) {
        if (!stage.inputIdentity.empty() && (stage.inputIdentity.size() != 64 || !std::ranges::all_of(stage.inputIdentity, [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }))) throw std::runtime_error("invalid pipeline input identity");
        if (stage.stage == ShaderRecompiler::ShaderStage::Compute || (native && (stage.program == nullptr || !validShader(*stage.program)))) throw std::runtime_error("invalid pipeline recipe shader");
        if (!stages.insert(VulkanStage(stage.stage)).second || stage.pushConstantOffset > PipelinePushConstantBytes || (native && stage.program->pushConstants.size() > PipelinePushConstantBytes - stage.pushConstantOffset) || stage.pushConstantOffset % 4 != 0) throw std::runtime_error("invalid pipeline recipe push layout");
    }
    const auto& state = recipe.state;
    if (static_cast<std::uint32_t>(state.stages.path) > static_cast<std::uint32_t>(ShaderPath::TessellationGeometry) || state.colors.size() > 8 || state.blends.size() > 8 || state.samples == 0 || state.samples > 8 || (state.samples & (state.samples - 1u)) != 0) throw std::runtime_error("invalid pipeline recipe state");
    const auto above = [](auto value, auto maximum) { return static_cast<std::uint32_t>(value) > static_cast<std::uint32_t>(maximum); };
    if (above(state.topology, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) || state.cullMode > VK_CULL_MODE_FRONT_AND_BACK || above(state.frontFace, VK_FRONT_FACE_CLOCKWISE) || above(state.depthCompare, VK_COMPARE_OP_ALWAYS) || above(state.provokingVertexMode, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT) || above(state.conservativeRasterization, VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT)) throw std::runtime_error("invalid pipeline recipe raster state");
    if (recipe.attachmentLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && recipe.attachmentLayout != VK_IMAGE_LAYOUT_GENERAL) throw std::runtime_error("invalid pipeline recipe attachment layout");
    std::set<std::uint32_t> exports;
    for (const auto& color : state.colors) if (color.exportIndex >= state.blends.size() || !exports.insert(color.exportIndex).second || color.format == VK_FORMAT_UNDEFINED) throw std::runtime_error("invalid pipeline recipe attachments");
    if (state.blends.size() != (state.colors.empty() ? 0u : state.colors.back().exportIndex + 1u)) throw std::runtime_error("invalid pipeline recipe blend count");
    for (const auto& value : state.blends) if (value.blendEnable > VK_TRUE || above(value.colorBlendOp, VK_BLEND_OP_MAX) || above(value.alphaBlendOp, VK_BLEND_OP_MAX) || above(value.srcColorBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) || above(value.dstColorBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) || above(value.srcAlphaBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) || above(value.dstAlphaBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) || (value.colorWriteMask & ~15u) != 0) throw std::runtime_error("invalid pipeline recipe blend state");
    for (const auto* value : {&state.stencilFront, &state.stencilBack}) if (above(value->failOp, VK_STENCIL_OP_DECREMENT_AND_WRAP) || above(value->passOp, VK_STENCIL_OP_DECREMENT_AND_WRAP) || above(value->depthFailOp, VK_STENCIL_OP_DECREMENT_AND_WRAP) || above(value->compareOp, VK_COMPARE_OP_ALWAYS)) throw std::runtime_error("invalid pipeline recipe stencil state");
    std::set<std::uint32_t> bindings;
    for (const auto& value : recipe.vertexInput.bindings) if (!bindings.insert(value.binding).second || above(value.inputRate, VK_VERTEX_INPUT_RATE_INSTANCE)) throw std::runtime_error("invalid pipeline recipe vertex binding");
    std::set<std::uint32_t> locations;
    for (const auto& value : recipe.vertexInput.attributes) if (!bindings.contains(value.binding) || !locations.insert(value.location).second || value.format == VK_FORMAT_UNDEFINED) throw std::runtime_error("invalid pipeline recipe vertex attribute");
}

template<typename TCodec>
void layoutFields(TCodec& codec, PipelineRecipe& recipe) {
    codec.Value(recipe.attachmentLayout);
    codec.List(recipe.vertexInput.bindings, 64u);
    for (auto& value : recipe.vertexInput.bindings) { codec.Value(value.binding); codec.Value(value.stride); codec.Value(value.inputRate); }
    codec.List(recipe.vertexInput.attributes, 64u);
    for (auto& value : recipe.vertexInput.attributes) { codec.Value(value.location); codec.Value(value.binding); codec.Value(value.format); codec.Value(value.offset); }
    codec.List(recipe.mainLayoutKey, 4096u);
    for (auto& value : recipe.mainLayoutKey) codec.Value(value);
    codec.List(recipe.fragmentLayoutKey, 4096u);
    for (auto& value : recipe.fragmentLayoutKey) codec.Value(value);
    codec.Value(recipe.splitSets);
    stateFields(codec, recipe.state);
}

}

State CanonicalPipelineState(const State& state) {
    if (state.colors.size() > 8 || state.blends.size() > 8) throw std::length_error("pipeline recipe state count exceeds limit");
    Writer writer;
    auto input = state;
    stateFields(writer, input);
    State result{};
    Reader reader(writer.out);
    stateFields(reader, result);
    if (result.stencilReplayReference) {
        result.stencilReplayReference = 0u;
        result.stencilFront.reference = result.stencilBack.reference = 0u;
    }
    return result;
}

PipelineRecipe MakePipelineRecipe(const State& state, const VertexInputLayout& vertexInput, const PipelineSetLayouts& resources, std::span<const CompiledShader> shaders, VkImageLayout attachmentLayout) {
    stateBounds(state, vertexInput, resources.mainKey.size(), resources.fragmentKey.size());
    static_cast<void>(MinimumPipelineRecipeBytes(shaders));
    PipelineRecipe recipe;
    recipe.state = CanonicalPipelineState(state);
    recipe.vertexInput = vertexInput;
    recipe.mainLayoutKey.assign(resources.mainKey.begin(), resources.mainKey.end());
    recipe.fragmentLayoutKey.assign(resources.fragmentKey.begin(), resources.fragmentKey.end());
    recipe.splitSets = resources.split;
    recipe.attachmentLayout = attachmentLayout;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) throw std::runtime_error("missing compiled shader");
        auto result = canonicalShader(*shader.program);
        if (pushBytes(shader) > PipelinePushConstantBytes) throw std::runtime_error("invalid pipeline shader push size");
        result.pushConstants.assign(pushBytes(shader), std::byte{0});
        recipe.stages.push_back({shader.stage, shader.pushConstantOffset, std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(result))});
    }
    validate(recipe);
    return recipe;
}

std::vector<CompiledShader> PipelineRecipeShaders(const PipelineRecipe& recipe) {
    if (recipe.stages.size() > 8) throw std::length_error("pipeline recipe stage count exceeds limit");
    std::vector<CompiledShader> result;
    result.reserve(recipe.stages.size());
    for (const auto& stage : recipe.stages) {
        CompiledShader shader{stage.stage, stage.program.get(), stage.pushConstantOffset};
        if (stage.program != nullptr) setPushBytes(shader, stage.program->pushConstants);
        result.push_back(shader);
    }
    return result;
}

std::size_t MinimumPipelineRecipeBytes(std::span<const CompiledShader> shaders) {
    if (shaders.empty() || shaders.size() > 8) throw std::length_error("pipeline recipe stage count exceeds limit");
    PreflightBudget budget;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) throw std::runtime_error("missing compiled shader");
        budget.Encoded(wordCount(shader.program->spirv), sizeof(std::uint32_t));
    }
    for (const auto& shader : shaders) shaderMetadata(budget, *shader.program);
    return budget.Minimum();
}

std::size_t MinimumPipelineRecipeBytes(const PipelineRecipe& recipe) {
    stateBounds(recipe.state, recipe.vertexInput, recipe.mainLayoutKey.size(), recipe.fragmentLayoutKey.size());
    if (recipe.stages.size() > 8) throw std::length_error("pipeline recipe stage count exceeds limit");
    for (const auto& stage : recipe.stages) if (stage.inputIdentity.size() > 64) throw std::length_error("pipeline input identity exceeds limit");
    const auto shaders = PipelineRecipeShaders(recipe);
    return MinimumPipelineRecipeBytes(shaders);
}

std::vector<std::byte> PipelineShaderIdentity(const CompiledShader& shader) {
    if (shader.program == nullptr) throw std::runtime_error("missing compiled shader");
    static_cast<void>(MinimumPipelineRecipeBytes(std::span(&shader, 1)));
    Writer writer;
    auto version = PipelineRecipeVersion;
    auto shaderVersion = ShaderRecompiler::ShaderDiskCache::FormatVersion;
    auto source = ShaderRecompiler::ShaderDiskCache::SourceVersion();
    writer.Value(version);
    writer.Value(shaderVersion);
    writer.Value(source);
    auto stage = shader.stage;
    auto offset = shader.pushConstantOffset;
    writer.Value(stage);
    writer.Value(offset);
    auto result = canonicalShader(*shader.program);
    if (pushBytes(shader) > PipelinePushConstantBytes) throw std::runtime_error("invalid pipeline shader push size");
    result.pushConstants.assign(pushBytes(shader), std::byte{0});
    std::vector<std::byte> encoded;
    ShaderRecompiler::ShaderDiskCache::EncodeResult(result, encoded);
    writer.Bytes(encoded);
    return std::move(writer.out);
}

std::vector<std::byte> PipelineRecipeStateIdentity(const State& state, const VertexInputLayout& vertexInput, const PipelineSetLayouts& resources, VkImageLayout attachmentLayout) {
    stateBounds(state, vertexInput, resources.mainKey.size(), resources.fragmentKey.size());
    PipelineRecipe recipe;
    recipe.state = CanonicalPipelineState(state);
    recipe.vertexInput = vertexInput;
    recipe.mainLayoutKey.assign(resources.mainKey.begin(), resources.mainKey.end());
    recipe.fragmentLayoutKey.assign(resources.fragmentKey.begin(), resources.fragmentKey.end());
    recipe.splitSets = resources.split;
    recipe.attachmentLayout = attachmentLayout;
    Writer writer;
    layoutFields(writer, recipe);
    return std::move(writer.out);
}

std::vector<std::byte> EncodePipelineRecipe(const PipelineRecipe& recipe) {
    static_cast<void>(MinimumPipelineRecipeBytes(recipe));
    auto canonical = MakePipelineRecipe(recipe.state, recipe.vertexInput, {VK_NULL_HANDLE, VK_NULL_HANDLE, recipe.splitSets, recipe.mainLayoutKey, recipe.fragmentLayoutKey}, PipelineRecipeShaders(recipe), recipe.attachmentLayout);
    for (std::size_t index = 0; index < canonical.stages.size(); ++index) canonical.stages[index].inputIdentity = recipe.stages[index].inputIdentity;
    validate(canonical);
    Writer payload;
    layoutFields(payload, canonical);
    payload.List(canonical.stages, 8u);
    for (const auto& stage : canonical.stages) {
        auto stageType = stage.stage;
        auto offset = stage.pushConstantOffset;
        payload.Value(stageType);
        payload.Value(offset);
        auto inputIdentity = stage.inputIdentity;
        payload.Text(inputIdentity);
        std::vector<std::byte> encoded;
        ShaderRecompiler::ShaderDiskCache::EncodeResult(*stage.program, encoded);
        payload.Bytes(encoded);
    }
    if (payload.out.size() > PipelineRecipeMaxBytes - HeaderBytes) throw std::length_error("pipeline recipe exceeds byte limit");
    Writer file;
    auto magic = Magic;
    auto version = PipelineRecipeVersion;
    auto shaderVersion = ShaderRecompiler::ShaderDiskCache::FormatVersion;
    auto abi = ShaderRecompiler::RuntimeAbi::Version;
    auto source = ShaderRecompiler::ShaderDiskCache::SourceVersion();
    auto count = static_cast<std::uint32_t>(payload.out.size());
    auto checksum = hashBytes(payload.out, 0xcbf29ce484222325ull);
    auto checksumHigh = hashBytes(payload.out, 0x9e3779b97f4a7c15ull);
    file.Value(magic); file.Value(version); file.Value(shaderVersion); file.Value(abi); file.Value(source); file.Value(count); file.Value(checksum); file.Value(checksumHigh);
    file.out.insert(file.out.end(), payload.out.begin(), payload.out.end());
    return std::move(file.out);
}

namespace {

bool decode(std::span<const std::byte> bytes, PipelineRecipe& recipe, bool inputsOnly) {
    if (bytes.size() < HeaderBytes || bytes.size() > PipelineRecipeMaxBytes || std::endian::native != std::endian::little) return false;
    try {
        Reader header(bytes.first(HeaderBytes));
        std::uint32_t magic = 0, version = 0, shaderVersion = 0, abi = 0, count = 0;
        std::uint64_t source = 0, checksum = 0, checksumHigh = 0;
        header.Value(magic); header.Value(version); header.Value(shaderVersion); header.Value(abi); header.Value(source); header.Value(count); header.Value(checksum); header.Value(checksumHigh);
        if (magic != Magic || version != PipelineRecipeVersion || abi != ShaderRecompiler::RuntimeAbi::Version || count != bytes.size() - HeaderBytes) return false;
        if (!inputsOnly && (shaderVersion != ShaderRecompiler::ShaderDiskCache::FormatVersion || source != ShaderRecompiler::ShaderDiskCache::SourceVersion())) return false;
        const auto payload = bytes.subspan(HeaderBytes);
        if (checksum != hashBytes(payload, 0xcbf29ce484222325ull) || checksumHigh != hashBytes(payload, 0x9e3779b97f4a7c15ull)) return false;
        PipelineRecipe decoded;
        Reader reader(payload);
        layoutFields(reader, decoded);
        reader.List(decoded.stages, 8u);
        for (auto& stage : decoded.stages) {
            reader.Value(stage.stage);
            reader.Value(stage.pushConstantOffset);
            reader.Text(stage.inputIdentity);
            if (!stage.inputIdentity.empty() && (stage.inputIdentity.size() != 64 || !std::ranges::all_of(stage.inputIdentity, [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }))) return false;
            if (inputsOnly) {
                if (stage.stage == ShaderRecompiler::ShaderStage::Compute || stage.pushConstantOffset > PipelinePushConstantBytes || stage.pushConstantOffset % 4 != 0) return false;
                static_cast<void>(VulkanStage(stage.stage));
                reader.SkipBytes();
                continue;
            }
            std::vector<std::byte> encoded;
            reader.Bytes(encoded);
            auto result = std::make_shared<ShaderRecompiler::RecompileResult>();
            if (!ShaderRecompiler::ShaderDiskCache::DecodeResult(encoded, *result)) return false;
            stage.program = std::move(result);
        }
        if (!reader.Done()) return false;
        if (inputsOnly) {
            validate(decoded, false);
            decoded.state = CanonicalPipelineState(decoded.state);
        } else {
            validate(decoded);
            if (EncodePipelineRecipe(decoded) != std::vector<std::byte>(bytes.begin(), bytes.end())) return false;
        }
        recipe = std::move(decoded);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}

bool DecodePipelineRecipe(std::span<const std::byte> bytes, PipelineRecipe& recipe) { return decode(bytes, recipe, false); }

bool DecodePipelineRecipeInputs(std::span<const std::byte> bytes, PipelineRecipe& recipe) { return decode(bytes, recipe, true); }

std::string PipelineRecipeInputIdentity(const PipelineRecipe& recipe) {
    stateBounds(recipe.state, recipe.vertexInput, recipe.mainLayoutKey.size(), recipe.fragmentLayoutKey.size());
    if (recipe.stages.size() > 8) throw std::length_error("pipeline recipe stage count exceeds limit");
    for (const auto& stage : recipe.stages) if (stage.inputIdentity.size() > 64) throw std::length_error("pipeline input identity exceeds limit");
    auto canonical = recipe;
    canonical.state = CanonicalPipelineState(recipe.state);
    canonical.mainLayoutKey.clear();
    canonical.fragmentLayoutKey.clear();
    canonical.splitSets = false;
    Writer writer;
    auto version = PipelineRecipeVersion;
    auto abi = ShaderRecompiler::RuntimeAbi::Version;
    writer.Value(version);
    writer.Value(abi);
    layoutFields(writer, canonical);
    writer.List(canonical.stages, 8u);
    for (auto& stage : canonical.stages) {
        writer.Value(stage.stage);
        writer.Text(stage.inputIdentity);
    }
    return PipelineRecipeIdentity(writer.out);
}

std::string PipelineRecipeIdentity(std::span<const std::byte> encoded) {
    constexpr char digits[] = "0123456789abcdef";
    std::string identity;
    identity.reserve(32);
    for (const auto hash : {hashBytes(encoded, 0xcbf29ce484222325ull), hashBytes(encoded, 0x9e3779b97f4a7c15ull)}) {
        for (int shift = 60; shift >= 0; shift -= 4) identity.push_back(digits[(hash >> shift) & 15u]);
    }
    return identity;
}

std::string PipelineRecipeIdentity(const PipelineRecipe& recipe) { return PipelineRecipeIdentity(EncodePipelineRecipe(recipe)); }

}

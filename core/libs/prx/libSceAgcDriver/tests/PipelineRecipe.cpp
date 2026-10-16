#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipe.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {

bool rejectLargeAllocation = false;

}

void* operator new(std::size_t bytes) {
    if (rejectLargeAllocation && bytes > 1024u) throw std::bad_alloc();
    if (auto* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {

using namespace AgcDriver::Graphics;
using namespace ShaderRecompiler;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TShader>
void setPushBytes(TShader& shader, std::span<const std::byte> bytes) {
    if constexpr (requires { shader.pushConstants = bytes; }) shader.pushConstants = bytes;
}

template<typename TShader>
void pushOverrides(TShader shader) {
    const auto identity = PipelineShaderIdentity(shader);
    if constexpr (requires { shader.pushConstants; }) {
        shader.pushConstants = {};
        require(PipelineShaderIdentity(shader) == identity, "empty optional push override lost the compiled program push layout");
        const std::array<std::byte, 1> invalid{std::byte{0}};
        shader.pushConstants = invalid;
        bool threw = false;
        try { static_cast<void>(PipelineShaderIdentity(shader)); } catch (const std::exception&) { threw = true; }
        require(threw, "mismatched optional push override was accepted");
    }
}

PipelineRecipe fixture() {
    RecompileResult shader;
    shader.spirv = std::vector<std::uint32_t>{0x07230203u, 0x00010000u, 0u, 2u, 0u, 0x00020011u, 1u};
    shader.hostSubgroupSize = 32;
    shader.specialization = {{7u, 11u}, {2u, 13u}};
    shader.pushConstants.assign(16, std::byte{0x55});
    DescriptorBinding binding{};
    binding.kind = DescriptorKind::StorageBuffer;
    binding.role = DescriptorRole::ShaderData;
    binding.count = 1;
    binding.guestDescriptor = {1u, 2u, 3u, 4u};
    shader.bindings.push_back(binding);
    State state{};
    state.stages.path = ShaderPath::Vertex;
    state.stages.vertexWaveSize = 32;
    state.stages.fragmentWaveSize = 32;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.hasColorTarget = false;
    VertexInputLayout vertex;
    vertex.bindings.push_back({0u, 16u, VK_VERTEX_INPUT_RATE_VERTEX});
    vertex.attributes.push_back({0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 0u});
    const std::array<std::uint32_t, 4> layout{0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, VK_SHADER_STAGE_VERTEX_BIT};
    std::array<CompiledShader, 1> stages{{{ShaderStage::Vertex, &shader, 0u}}};
    setPushBytes(stages[0], shader.pushConstants);
    auto recipe = MakePipelineRecipe(state, vertex, {VK_NULL_HANDLE, VK_NULL_HANDLE, false, layout, {}}, stages, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    recipe.stages[0].inputIdentity.assign(64, 'a');
    return recipe;
}

void codec() {
    auto recipe = fixture();
    pushOverrides(PipelineRecipeShaders(recipe).front());
    auto encoded = EncodePipelineRecipe(recipe);
    PipelineRecipe decoded;
    require(DecodePipelineRecipe(encoded, decoded), "recipe round trip failed");
    require(EncodePipelineRecipe(decoded) == encoded, "recipe round trip changed canonical bytes");
    require(decoded.stages[0].program->specialization[0].id == 2u && decoded.stages[0].program->specialization[1].value == 11u, "materialized specialization was lost");
    require(decoded.stages[0].program->bindings[0].guestDescriptor.empty(), "guest descriptors persisted");
    require(decoded.stages[0].program->pushConstants.size() == 16 && decoded.stages[0].program->pushConstants[0] == std::byte{0}, "push layout was lost or draw values persisted");
    require(decoded.mainLayoutKey == recipe.mainLayoutKey && decoded.vertexInput.attributes[0].format == VK_FORMAT_R32G32B32A32_SFLOAT, "pipeline layouts changed");
    const auto initialIdentity = PipelineRecipeIdentity(recipe);
    auto different = recipe;
    auto shader = std::make_shared<RecompileResult>(*recipe.stages[0].program);
    different.stages[0].program = shader;
    shader->variantId = 999;
    shader->specializationId = 123;
    shader->pushConstants.assign(16, std::byte{0xaa});
    shader->bindings[0].guestDescriptor = {555u, 666u, 777u, 888u};
    shader->imageTableShader = 123456u;
    std::reverse(shader->specialization.begin(), shader->specialization.end());
    different.state.viewport = {100, 100, 500, 500, 0, 1};
    different.state.scissor = {{77, 88}, {123, 456}};
    require(PipelineRecipeIdentity(different) == initialIdentity, "transient IDs, addresses or dynamic draw state changed native identity");
    shader->specialization[0].value ^= 1u;
    require(PipelineRecipeIdentity(different) != initialIdentity, "specialization values are missing from native identity");
    require(PipelineRecipeInputIdentity(different) == PipelineRecipeInputIdentity(recipe), "compiled specialization changed input identity");
    shader->specialization[0].value ^= 1u;
    shader->bdaAbiVersion = 99;
    require(PipelineRecipeIdentity(different) != initialIdentity, "BDA ABI is missing from native identity");
    different = recipe;
    different.mainLayoutKey[2] = 2;
    require(PipelineRecipeIdentity(different) != initialIdentity, "descriptor layout is missing from native identity");
    require(PipelineRecipeInputIdentity(different) == PipelineRecipeInputIdentity(recipe), "compiler-generated descriptor layout changed input identity");
    different = recipe;
    different.stages[0].pushConstantOffset = 16;
    require(PipelineRecipeIdentity(different) != initialIdentity, "push offset is missing from native identity");
    require(PipelineRecipeInputIdentity(different) == PipelineRecipeInputIdentity(recipe), "compiler-generated push offset changed input identity");
    different = recipe;
    different.state.depthBiasClamp = 1.0f;
    require(PipelineRecipeIdentity(different) != initialIdentity, "pipeline state is missing from native identity");
    const auto identity = PipelineRecipeIdentity(encoded);
    require(identity.size() == 32 && identity == PipelineRecipeIdentity(recipe), "content filename is unstable");
    encoded.clear();
    require(decoded.stages[0].program->spirv.Words()[0] == 0x07230203u, "decoded SPIR-V borrows destroyed entry storage");
}

void runtimeIdentityAndEmptyTables() {
    auto recipe = fixture();
    auto shader = std::make_shared<RecompileResult>(*recipe.stages[0].program);
    recipe.stages[0].program = shader;
    shader->bindings[0].descriptorSet = 2u;
    require(!PipelineShaderIdentity(PipelineRecipeShaders(recipe).front()).empty(), "runtime identity rejected metadata before runtime semantic validation");
    bool rejected = false;
    try { static_cast<void>(EncodePipelineRecipe(recipe)); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "invalid runtime descriptor set entered a persisted recipe");
    shader->bindings.clear();
    for (const auto role : {DescriptorRole::ImageTable, DescriptorRole::SamplerTable}) {
        DescriptorBinding table{};
        table.kind = role == DescriptorRole::ImageTable ? DescriptorKind::SampledImage : DescriptorKind::Sampler;
        table.role = role;
        shader->bindings.push_back(table);
    }
    const auto stages = PipelineRecipeShaders(recipe);
    auto made = MakePipelineRecipe(recipe.state, recipe.vertexInput, {}, stages, recipe.attachmentLayout);
    const auto encoded = EncodePipelineRecipe(made);
    PipelineRecipe decoded;
    require(DecodePipelineRecipe(encoded, decoded), "empty image and sampler tables did not persist");
    require(decoded.stages[0].program->bindings.size() == 2u && decoded.stages[0].program->bindings[0].count == 0u && decoded.stages[0].program->bindings[1].count == 0u, "empty table counts changed on decode");
    const auto refuses = [&] {
        bool refused = false;
        try { static_cast<void>(EncodePipelineRecipe(recipe)); } catch (const std::runtime_error&) { refused = true; }
        require(refused, "invalid empty table metadata entered a persisted recipe");
    };
    shader->bindings[0].imageDepthCompare.push_back(false);
    refuses();
    shader->bindings[0].imageDepthCompare.clear();
    shader->bindings[0].kind = DescriptorKind::StorageBuffer;
    refuses();
    shader->bindings[0].kind = DescriptorKind::SampledImage;
    shader->bindings[1].role = DescriptorRole::GuestSamplers;
    refuses();
}

void rejection() {
    const auto recipe = fixture();
    const auto bytes = EncodePipelineRecipe(recipe);
    auto sentinel = fixture();
    sentinel.state.cullMode = VK_CULL_MODE_BACK_BIT;
    const auto sentinelBytes = EncodePipelineRecipe(sentinel);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        require(!DecodePipelineRecipe(std::span(bytes).first(length), sentinel), "truncated recipe accepted");
    }
    require(EncodePipelineRecipe(sentinel) == sentinelBytes, "failed decode modified caller output");
    for (const auto offset : {0u, 4u, 8u, 12u, 16u, 24u, 28u, 36u, 44u}) {
        auto corrupt = bytes;
        corrupt[offset] ^= std::byte{1};
        require(!DecodePipelineRecipe(corrupt, sentinel), "corrupt or incompatible recipe accepted");
    }
    auto oldSource = bytes;
    oldSource[16] ^= std::byte{1};
    PipelineRecipe inputs;
    require(DecodePipelineRecipeInputs(oldSource, inputs), "compiler change discarded durable pipeline state");
    require(inputs.stages[0].program == nullptr && inputs.stages[0].inputIdentity == recipe.stages[0].inputIdentity, "inputs-only decode loaded stale artifacts or lost input identity");
    require(PipelineRecipeInputIdentity(inputs) == PipelineRecipeInputIdentity(recipe), "compiler change altered durable recipe identity");
    auto oldCodec = bytes;
    oldCodec[8] ^= std::byte{1};
    require(DecodePipelineRecipeInputs(oldCodec, inputs), "shader codec change discarded source-independent state");
    auto oldAbi = bytes;
    oldAbi[12] ^= std::byte{1};
    require(!DecodePipelineRecipeInputs(oldAbi, inputs), "unsupported runtime ABI accepted for regeneration");
    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    require(!DecodePipelineRecipe(trailing, sentinel), "trailing bytes accepted");
    std::vector<std::byte> oversized(PipelineRecipeMaxBytes + 1u);
    require(!DecodePipelineRecipe(oversized, sentinel), "oversized recipe accepted");
    auto invalid = recipe;
    auto shader = std::make_shared<RecompileResult>(*recipe.stages[0].program);
    invalid.stages[0].program = shader;
    shader->specialization.push_back(shader->specialization.front());
    bool threw = false;
    try { static_cast<void>(EncodePipelineRecipe(invalid)); } catch (const std::exception&) { threw = true; }
    require(threw, "duplicate specialization IDs accepted");
}

template<typename TWork>
void refusesBeforeCopy(TWork work) {
    rejectLargeAllocation = true;
    bool refused = false;
    try { work(); } catch (const std::length_error&) { refused = true; } catch (...) {}
    rejectLargeAllocation = false;
    require(refused, "oversized recipe was not refused before copying or encoding payloads");
}

void preflight() {
    auto recipe = fixture();
    const auto minimum = MinimumPipelineRecipeBytes(recipe);
    require(minimum <= EncodePipelineRecipe(recipe).size(), "preflight size is not a lower bound");
    const auto count = PipelineRecipeMaxBytes / (2u * sizeof(std::uint32_t)) + 1u;
    std::vector<std::uint32_t> words(count, 0x00010000u);
    std::copy_n(std::array<std::uint32_t, 5>{0x07230203u, 0x00010000u, 0u, 1u, 0u}.begin(), 5, words.begin());
    auto large = std::make_shared<RecompileResult>();
    large->spirv = std::move(words);
    recipe.stages = {{ShaderStage::Vertex, 0u, large, {}}, {ShaderStage::Fragment, 0u, large, {}}};
    const auto shaders = PipelineRecipeShaders(recipe);
    refusesBeforeCopy([&] { static_cast<void>(MinimumPipelineRecipeBytes(recipe)); });
    refusesBeforeCopy([&] { static_cast<void>(EncodePipelineRecipe(recipe)); });
    refusesBeforeCopy([&] { static_cast<void>(MakePipelineRecipe(recipe.state, recipe.vertexInput, {}, shaders, recipe.attachmentLayout)); });
    auto metadata = std::make_shared<RecompileResult>(*fixture().stages[0].program);
    metadata->bindings[0].guestDescriptor.resize(1048577u);
    auto invalidMetadata = fixture();
    invalidMetadata.stages[0].program = metadata;
    refusesBeforeCopy([&] { static_cast<void>(EncodePipelineRecipe(invalidMetadata)); });
    refusesBeforeCopy([&] { static_cast<void>(PipelineShaderIdentity(PipelineRecipeShaders(invalidMetadata).front())); });
}

}

int main() {
    try {
        codec();
        runtimeIdentityAndEmptyTables();
        rejection();
        preflight();
        std::cout << "pipeline recipe codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

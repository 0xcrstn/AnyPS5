#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeGenerators.hpp"
#include <array>
#include <iostream>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>

namespace {

using namespace AgcDriver::Graphics;
using namespace ShaderRecompiler;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TWork>
void rejects(TWork work, const char* message) {
    bool threw = false;
    try { work(); } catch (const std::exception&) { threw = true; }
    require(threw, message);
}

PipelineRecipe fixture(bool rectangle) {
    PipelineRecipe recipe;
    recipe.state.stages.path = ShaderPath::Vertex;
    recipe.state.rectList = rectangle;
    recipe.state.topology = rectangle ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    recipe.state.cullMode = VK_CULL_MODE_NONE;
    auto vertex = std::make_shared<RecompileResult>();
    vertex->parameterExports = {0u};
    auto fragment = std::make_shared<RecompileResult>();
    fragment->fragmentParameters.push_back({0u, 0u, false, false});
    fragment->barycentricEmulation.active = !rectangle;
    fragment->barycentricEmulation.smooth = !rectangle;
    recipe.stages.push_back({ShaderStage::Vertex, 0u, std::move(vertex), std::string(64, 'a')});
    if (rectangle) {
        recipe.stages.push_back({ShaderStage::TessellationControl, 0u, nullptr, {}});
        recipe.stages.push_back({ShaderStage::TessellationEvaluation, 0u, nullptr, {}});
    } else recipe.stages.push_back({ShaderStage::Geometry, 0u, nullptr, {}});
    recipe.stages.push_back({ShaderStage::Fragment, PipelinePushSlotBytes, std::move(fragment), std::string(64, 'b')});
    return recipe;
}

SpirvTarget target() {
    static constexpr std::array<std::uint32_t, 3> capabilities{spv::CapabilityShader, spv::CapabilityTessellation, spv::CapabilityGeometry};
    SpirvTarget result{};
    result.vulkanVersion = VK_API_VERSION_1_1;
    result.spirvVersion = 0x00010300u;
    result.supportedCapabilities = capabilities;
    result.tessellation = TessellationTargetLimits{32u, 128u, 128u, 120u, 4096u, 128u, 128u};
    return result;
}

bool entryPoint(const RecompileResult& result, spv::ExecutionModel model) {
    const auto words = result.spirv.Words();
    require(words.size() >= 5 && words[0] == spv::MagicNumber, "generated shader lacks SPIR-V header");
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        require(count != 0 && count <= words.size() - cursor, "generated shader instruction is truncated");
        if ((words[cursor] & 0xffffu) == spv::OpEntryPoint && count >= 3 && words[cursor + 1] == static_cast<std::uint32_t>(model)) return true;
        cursor += count;
    }
    return false;
}

void regeneration() {
    const auto current = target();
    const GeometryStageLimits limits{64u, 128u, 256u, 1024u, 128u};
    auto rectangle = fixture(true);
    require(IsDerivedPipelineStage(rectangle, 1) && IsDerivedPipelineStage(rectangle, 2) && !IsDerivedPipelineStage(rectangle, 0) && !IsDerivedPipelineStage(rectangle, 3) && !IsDerivedPipelineStage(rectangle, 4), "rect-list derived stage classification is wrong");
    const auto vertex = rectangle.stages.front().program;
    const auto fragment = rectangle.stages.back().program;
    RegeneratePipelineRecipeStages(rectangle, current, limits);
    require(rectangle.stages.front().program == vertex && rectangle.stages.back().program == fragment, "regeneration replaced captured source stages");
    require(entryPoint(*rectangle.stages[1].program, spv::ExecutionModelTessellationControl) && entryPoint(*rectangle.stages[2].program, spv::ExecutionModelTessellationEvaluation), "rect-list regeneration did not emit the expected stages");
    require(rectangle.stages[1].program->bindings.size() == 1 && rectangle.stages[1].program->bindings[0].role == DescriptorRole::FaultBuffer, "rect-list regeneration lost its fault buffer layout");
    require(rectangle.stages[1].inputIdentity.empty() && rectangle.stages[2].inputIdentity.empty() && rectangle.stages[1].pushConstantOffset == 0 && rectangle.stages[2].pushConstantOffset == 0, "generated stages acquired false source provenance or push offsets");
    require(!IsDerivedPipelineStage(rectangle, 1), "resolved stage is still an inventory exemption");
    const auto control = rectangle.stages[1].program;
    RegeneratePipelineRecipeStages(rectangle, current, limits);
    require(rectangle.stages[1].program == control, "complete recipe was unnecessarily regenerated");
    auto barycentric = fixture(false);
    require(IsDerivedPipelineStage(barycentric, 1), "barycentric generated stage was not recognized");
    RegeneratePipelineRecipeStages(barycentric, current, limits);
    require(entryPoint(*barycentric.stages[1].program, spv::ExecutionModelGeometry), "barycentric regeneration did not emit a geometry shader");
}

void rejection() {
    const auto current = target();
    const GeometryStageLimits limits{64u, 128u, 256u, 1024u, 128u};
    auto unknown = fixture(true);
    unknown.stages.front().inputIdentity.clear();
    require(!IsDerivedPipelineStage(unknown, 1), "recipe without source provenance was exempted");
    rejects([&] { RegeneratePipelineRecipeStages(unknown, current, limits); }, "recipe without source references regenerated");
    unknown = fixture(true);
    unknown.stages[1].inputIdentity.assign(64, 'c');
    require(!IsDerivedPipelineStage(unknown, 1), "wrong generated-stage source ID was ignored");
    rejects([&] { RegeneratePipelineRecipeStages(unknown, current, limits); }, "guest-stage reference was replaced by a generated stage");
    unknown = fixture(true);
    unknown.stages[1].pushConstantOffset = 4;
    require(!IsDerivedPipelineStage(unknown, 1), "unknown generated-stage push layout was accepted");
    unknown = fixture(true);
    unknown.state.rectList = false;
    unknown.state.stages.path = ShaderPath::Tessellation;
    require(!IsDerivedPipelineStage(unknown, 1), "real guest tessellation stage was treated as generated");
    rejects([&] { RegeneratePipelineRecipeStages(unknown, current, limits); }, "unknown missing guest stage regenerated");
    auto rectangle = fixture(true);
    auto unsupported = current;
    unsupported.tessellation.reset();
    rejects([&] { RegeneratePipelineRecipeStages(rectangle, unsupported, limits); }, "rect-list regenerated without tessellation target support");
    require(rectangle.stages[1].program == nullptr && rectangle.stages[2].program == nullptr, "failed rectangle generation partially modified the recipe");
    auto barycentric = fixture(false);
    auto fragment = std::make_shared<RecompileResult>(*barycentric.stages.back().program);
    fragment->barycentricEmulation.active = false;
    barycentric.stages.back().program = fragment;
    rejects([&] { RegeneratePipelineRecipeStages(barycentric, current, limits); }, "changed barycentric execution path was guessed");
    barycentric = fixture(false);
    rejects([&] { RegeneratePipelineRecipeStages(barycentric, current, std::nullopt); }, "geometry stage regenerated without device limits");
    barycentric.state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    require(!IsDerivedPipelineStage(barycentric, 1), "non-triangle barycentric shape accepted");
}

}

int main() {
    try {
        regeneration();
        rejection();
        std::cout << "pipeline recipe generator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

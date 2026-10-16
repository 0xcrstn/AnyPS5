#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeGenerators.hpp"
#include <algorithm>
#include <stdexcept>

namespace AgcDriver::Graphics {

namespace {

bool inputReference(const PipelineRecipe::Stage& stage) noexcept {
    return stage.inputIdentity.size() == 64 && std::ranges::all_of(stage.inputIdentity, [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

bool derivedHole(const PipelineRecipe::Stage& stage, ShaderRecompiler::ShaderStage expected) noexcept {
    return stage.stage == expected && stage.program == nullptr && stage.inputIdentity.empty() && stage.pushConstantOffset == 0;
}

bool sourceEndpoints(const PipelineRecipe& recipe) noexcept {
    using Stage = ShaderRecompiler::ShaderStage;
    return recipe.stages.size() >= 2 && recipe.stages.front().stage == Stage::Vertex && recipe.stages.back().stage == Stage::Fragment && inputReference(recipe.stages.front()) && inputReference(recipe.stages.back());
}

bool vertexPath(const PipelineRecipe& recipe) noexcept {
    return recipe.state.stages.path == ShaderPath::Vertex && !recipe.state.stages.mesh && !recipe.state.stages.tessellation;
}

bool rectangle(const PipelineRecipe& recipe) noexcept {
    using Stage = ShaderRecompiler::ShaderStage;
    return recipe.stages.size() == 4 && recipe.state.rectList && vertexPath(recipe) && recipe.state.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST && recipe.state.cullMode == VK_CULL_MODE_NONE && sourceEndpoints(recipe) && derivedHole(recipe.stages[1], Stage::TessellationControl) && derivedHole(recipe.stages[2], Stage::TessellationEvaluation);
}

bool barycentric(const PipelineRecipe& recipe) noexcept {
    const auto topology = recipe.state.topology;
    const bool triangles = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    return recipe.stages.size() == 3 && !recipe.state.rectList && vertexPath(recipe) && triangles && sourceEndpoints(recipe) && derivedHole(recipe.stages[1], ShaderRecompiler::ShaderStage::Geometry);
}

}

bool IsDerivedPipelineStage(const PipelineRecipe& recipe, std::size_t index) noexcept {
    if (index >= recipe.stages.size()) return false;
    return (rectangle(recipe) && (index == 1 || index == 2)) || (barycentric(recipe) && index == 1);
}

void RegeneratePipelineRecipeStages(PipelineRecipe& recipe, const ShaderRecompiler::SpirvTarget& target, const std::optional<ShaderRecompiler::GeometryStageLimits>& geometryLimits) {
    const bool rectangles = rectangle(recipe);
    const bool barycentrics = barycentric(recipe);
    bool missing = false;
    for (std::size_t index = 0; index < recipe.stages.size(); ++index) {
        if (recipe.stages[index].program != nullptr) continue;
        missing = true;
        if (!IsDerivedPipelineStage(recipe, index)) throw std::runtime_error("pipeline recipe has an unresolved source stage or unsupported generated stage");
    }
    if (!missing) return;
    const auto& vertex = *recipe.stages.front().program;
    const auto& fragment = *recipe.stages.back().program;
    ShaderRecompiler::RuntimeAbi::RequireVersion(vertex.runtimeAbiVersion);
    ShaderRecompiler::RuntimeAbi::RequireVersion(fragment.runtimeAbiVersion);
    if (rectangles) {
        auto generated = ShaderRecompiler::BuildRectListShaders(vertex, fragment, target);
        auto control = std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(generated.control));
        auto evaluation = std::make_shared<const ShaderRecompiler::RecompileResult>(std::move(generated.evaluation));
        recipe.stages[1].program = std::move(control);
        recipe.stages[2].program = std::move(evaluation);
    } else if (barycentrics) {
        if (!fragment.barycentricEmulation.active) throw std::runtime_error("pipeline recipe no longer uses emulated barycentrics");
        recipe.stages[1].program = std::make_shared<const ShaderRecompiler::RecompileResult>(ShaderRecompiler::BuildBarycentricGeometryShader(vertex, fragment, target, geometryLimits));
    }
}

}

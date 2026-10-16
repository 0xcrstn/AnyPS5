#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINERECIPEGENERATORS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINERECIPEGENERATORS_HPP

#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipe.hpp"

namespace AgcDriver::Graphics {

[[nodiscard]] bool IsDerivedPipelineStage(const PipelineRecipe& recipe, std::size_t index) noexcept;
void RegeneratePipelineRecipeStages(PipelineRecipe& recipe, const ShaderRecompiler::SpirvTarget& target, const std::optional<ShaderRecompiler::GeometryStageLimits>& geometryLimits);

}

#endif

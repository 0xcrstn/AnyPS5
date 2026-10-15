#ifndef CORE_SHADER_RECOMPILER_SHADERMANIFEST_HPP
#define CORE_SHADER_RECOMPILER_SHADERMANIFEST_HPP

#include "CompiledVariant.hpp"
#include "Recompiler.hpp"
#include <cstdint>
#include <string>
#include <string_view>

namespace ShaderRecompiler {

bool ShaderManifestEnabled();
std::string ShaderManifestLine(const RecompileRequest& request, const CompiledVariant& variant, std::string_view source);
void NoteShaderManifest(const RecompileRequest& request, const CompiledVariant& variant, std::string_view source);

}

#endif

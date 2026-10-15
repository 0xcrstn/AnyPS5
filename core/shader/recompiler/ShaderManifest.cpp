#include "ShaderManifest.hpp"
#include "BdaTable.hpp"
#include "SpirvBackend/SpirvStatistics.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ShaderRecompiler {

namespace {

const char* StageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Compute: return "compute";
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::TessellationControl: return "tessellation-control";
    case ShaderStage::TessellationEvaluation: return "tessellation-evaluation";
    case ShaderStage::Geometry: return "geometry";
    case ShaderStage::Fragment: return "fragment";
    case ShaderStage::Local: return "local";
    case ShaderStage::Mesh: return "mesh";
    }
    throw std::runtime_error("unknown shader stage in the shader manifest");
}

const char* TranslationName(BdaTable::Translation translation) {
    switch (translation) {
    case BdaTable::Translation::Legacy: return "legacy";
    case BdaTable::Translation::Radix: return "radix";
    case BdaTable::Translation::Verify: return "verify";
    case BdaTable::Translation::Linear: return "linear";
    case BdaTable::Translation::LinearVerify: return "linear_verify";
    }
    throw std::runtime_error("unknown BDA translation in the shader manifest");
}

std::uint64_t HashWords(std::span<const std::uint32_t> words) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : words) {
        hash ^= word;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::string Hex(std::uint64_t value) {
    char text[19];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

const std::filesystem::path& ManifestDirectory() {
    static const std::filesystem::path directory = [] {
        const char* text = std::getenv("APS5_SHADER_MANIFEST");
        return text != nullptr ? std::filesystem::path(text) : std::filesystem::path();
    }();
    return directory;
}

bool WritesSpirv(std::uint64_t address) {
    static const std::vector<std::uint64_t> addresses = [] {
        std::vector<std::uint64_t> parsed;
        const char* text = std::getenv("APS5_SHADER_MANIFEST_SPIRV");
        if (text == nullptr) return parsed;
        std::string list(text);
        if (list == "all") {
            parsed.push_back(0);
            return parsed;
        }
        std::stringstream stream(list);
        std::string item;
        while (std::getline(stream, item, ',')) {
            if (item.empty()) continue;
            char* end = nullptr;
            const auto value = std::strtoull(item.c_str(), &end, 16);
            if (end == item.c_str() || *end != '\0' || value == 0) throw std::runtime_error("APS5_SHADER_MANIFEST_SPIRV must be \"all\" or comma-separated hexadecimal shader addresses, not \"" + list + "\"");
            parsed.push_back(value);
        }
        return parsed;
    }();
    return std::find(addresses.begin(), addresses.end(), std::uint64_t{0}) != addresses.end() || std::find(addresses.begin(), addresses.end(), address) != addresses.end();
}

unsigned long ProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

}

bool ShaderManifestEnabled() {
    return !ManifestDirectory().empty();
}

std::string ShaderManifestLine(const RecompileRequest& request, const CompiledVariant& variant, std::string_view source) {
    const auto spirv = variant.artifact.spirv.Resolved();
    const auto words = spirv.Words();
    std::ostringstream line;
    line << "{\"address\":\"" << Hex(request.shader.codeAddress) << "\",\"stage\":\"" << StageName(request.shader.stage) << "\",\"source\":\"" << source << "\",\"variant\":" << variant.artifact.variantId
         << ",\"codeWords\":" << request.shader.code.size() << ",\"waveSize\":" << request.context.waveSize << ",\"hostSubgroupSize\":" << variant.artifact.hostSubgroupSize;
    if (request.context.compute.has_value()) {
        const auto& threads = request.context.compute->numThreads;
        line << ",\"workgroup\":[" << threads[0] << ',' << threads[1] << ',' << threads[2] << "],\"ldsDwords\":" << request.context.compute->ldsSizeDwords;
    }
    if (request.context.floatMode.has_value()) {
        const auto& mode = *request.context.floatMode;
        line << ",\"floatMode\":" << mode.floatMode << ",\"dx10Clamp\":" << (mode.dx10Clamp ? "true" : "false") << ",\"ieeeMode\":" << (mode.ieeeMode ? "true" : "false") << ",\"fp16Overflow\":" << (mode.fp16Overflow ? "true" : "false");
    }
    line << ",\"bdaAbi\":" << variant.artifact.bdaAbiVersion << ",\"bdaTranslation\":\"" << TranslationName(BdaTable::TranslationSetting()) << "\",\"registration\":" << (variant.artifact.registrationModule ? "true" : "false")
         << ",\"spirvHash\":\"" << Hex(HashWords(words)) << "\",\"statistics\":" << SpirvStatisticsJson(AnalyzeSpirv(words)) << '}';
    return line.str();
}

void NoteShaderManifest(const RecompileRequest& request, const CompiledVariant& variant, std::string_view source) {
    if (!ShaderManifestEnabled()) return;
    static std::mutex mutex;
    const auto line = ShaderManifestLine(request, variant, source);
    const auto& directory = ManifestDirectory();
    std::lock_guard lock(mutex);
    static const bool created = [&] {
        std::filesystem::create_directories(directory);
        return true;
    }();
    static_cast<void>(created);
    std::ofstream manifest(directory / ("manifest-" + std::to_string(ProcessId()) + ".jsonl"), std::ios::app);
    manifest << line << '\n';
    if (!manifest) throw std::runtime_error("cannot append to the shader manifest in " + directory.string());
    if (!WritesSpirv(request.shader.codeAddress)) return;
    const auto spirv = variant.artifact.spirv.Resolved();
    const auto words = spirv.Words();
    const auto name = std::string(StageName(request.shader.stage)) + "-" + Hex(request.shader.codeAddress) + "-" + Hex(HashWords(words)) + ".spv";
    std::ofstream file(directory / name, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size_bytes()));
    if (!file) throw std::runtime_error("cannot write " + (directory / name).string());
}

}

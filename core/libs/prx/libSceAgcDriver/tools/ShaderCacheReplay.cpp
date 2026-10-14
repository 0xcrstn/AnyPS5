#include "ShaderCacheEntry.hpp"
#include "Optimization/ResourceProgram.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open the file");
    const std::vector<char> text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) bytes[index] = static_cast<std::byte>(text[index]);
    return bytes;
}

std::uint64_t Hash(std::uint64_t hash, std::uint64_t value) {
    return (hash ^ value) * 0x100000001b3ull;
}

double Milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

template<typename TSpirv>
std::vector<std::uint32_t> PreparedWords(const TSpirv& spirv) {
    if constexpr (requires { spirv.Resolved(); }) return spirv.Resolved().Copy();
    else return spirv.Copy();
}

void WriteSpirv(const std::filesystem::path& directory, const char* path, std::span<const std::uint32_t> spirv) {
    std::filesystem::create_directories(directory);
    const auto target = directory / std::filesystem::path(path).filename().replace_extension(".spv");
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * sizeof(std::uint32_t)));
    if (!file) throw std::runtime_error("cannot write " + target.string());
}

void Replay(const char* path, bool frontEnd, bool graphics, const std::filesystem::path& spirvOut) {
    const auto bytes = ReadFile(path);
    const auto entry = AgcDriver::Tools::ReadCacheEntry(bytes, graphics);
    if (entry == nullptr) {
        std::printf("%s: skipped, not a compute%s shader variant\n", path, graphics ? ", pixel or vertex" : "");
        return;
    }
    if (frontEnd) {
        const auto started = std::chrono::steady_clock::now();
        static_cast<void>(ShaderRecompiler::PrepareResourceProgram(entry->request));
        std::printf("%s: front end %.1f ms\n", path, Milliseconds(started));
    }
    const auto cpuStarted = std::clock();
    const auto started = std::chrono::steady_clock::now();
    const auto handle = ShaderRecompiler::PrepareShader(entry->request);
    const auto wallMs = Milliseconds(started);
    const auto cpuMs = 1000.0 * static_cast<double>(std::clock() - cpuStarted) / CLOCKS_PER_SEC;
    const auto spirv = PreparedWords(ShaderRecompiler::GetPreparedArtifact(*handle).spirv);
    std::uint64_t spirvHash = 0xcbf29ce484222325ull;
    for (const auto word : spirv) spirvHash = Hash(spirvHash, word);
    if (!spirvOut.empty()) WriteSpirv(spirvOut, path, spirv);
    std::vector<std::byte> plan;
    if (!ShaderRecompiler::ShaderDiskCache::EncodePlan(*ShaderRecompiler::GetResourcePlan(entry->request), plan)) throw std::runtime_error("the resource plan cannot be encoded");
    std::uint64_t planHash = 0xcbf29ce484222325ull;
    for (const auto byte : plan) planHash = Hash(planHash, static_cast<std::uint8_t>(byte));
    const auto compute = entry->request.context.compute.value_or(ShaderRecompiler::ShaderComputeStageInfo{{0u, 0u, 0u}, 0u, {false, false, false}, false, 0u, {}, 0u});
    std::printf("%s: %zu code dwords, wave%u, %ux%ux%u threads%s, %zu SPIR-V words (%zu stored), prepared in %.1f ms (%.1f ms CPU), SPIR-V hash %016llx, plan hash %016llx, stored hash %016llx\n",
        path, entry->code.size(), entry->request.context.waveSize, compute.numThreads[0], compute.numThreads[1], compute.numThreads[2], compute.PartialGroups() ? " with partial groups" : "",
        spirv.size(), entry->storedWords, wallMs, cpuMs, static_cast<unsigned long long>(spirvHash), static_cast<unsigned long long>(planHash), static_cast<unsigned long long>(entry->storedHash));
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: agc_shader_cache_replay [--front-end] [--graphics] [--spirv-out DIRECTORY] <shader cache entry>...\n");
        return 2;
    }
    bool frontEnd = false;
    bool graphics = false;
    std::filesystem::path spirvOut;
    int failures = 0;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--front-end") {
            frontEnd = true;
            continue;
        }
        if (std::string(argv[index]) == "--graphics") {
            graphics = true;
            continue;
        }
        if (std::string(argv[index]) == "--spirv-out" && index + 1 < argc) {
            spirvOut = argv[++index];
            continue;
        }
        try {
            Replay(argv[index], frontEnd, graphics, spirvOut);
        } catch (const std::exception& error) {
            std::printf("%s: failed: %.2000s\n", argv[index], error.what());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

#include "CacheKey.hpp"
#include "Recompiler.hpp"
#include "RdnaDecoder/RdnaDescriptorFormat.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <map>
#include <windows.h>
#include <psapi.h>

namespace {

using namespace ShaderRecompiler;

class Bytes {
public:
    explicit Bytes(std::span<const std::byte> data) : data(data) {}
    template<typename T>
    T Read() {
        if (offset + sizeof(T) > data.size()) throw std::runtime_error("truncated key");
        T value{};
        std::memcpy(&value, data.data() + offset, sizeof(T));
        offset += sizeof(T);
        return value;
    }
    std::span<const std::byte> Take(std::size_t bytes) {
        if (offset + bytes > data.size()) throw std::runtime_error("truncated key");
        const auto taken = data.subspan(offset, bytes);
        offset += bytes;
        return taken;
    }
    [[nodiscard]] bool Done() const { return offset == data.size(); }

private:
    std::span<const std::byte> data;
    std::size_t offset = 0;
};

class Words {
public:
    explicit Words(std::span<const std::uint64_t> words) : words(words) {}
    std::uint64_t Next() {
        if (index >= words.size()) throw std::runtime_error("truncated request key");
        return words[index++];
    }
    template<typename T>
    T As() { return static_cast<T>(Next()); }
    bool Flag() { return Next() != 0u; }
    [[nodiscard]] bool Done() const { return index == words.size(); }

private:
    std::span<const std::uint64_t> words;
    std::size_t index = 0;
};

struct Decoded {
    std::vector<std::uint32_t> code;
    std::vector<std::uint32_t> userData;
    std::vector<std::uint32_t> fallbacks;
    std::vector<std::uint32_t> capabilities;
    std::vector<std::string> extensionText;
    std::vector<std::string_view> extensions;
    std::vector<std::uint64_t> storedKey;
    std::uint32_t hostSubgroupSize = 0;
    RecompileRequest request{};
};

template<typename T, std::size_t N>
void ReadArray(Words& words, std::array<T, N>& values) {
    for (auto& value : values) value = static_cast<T>(words.Next());
}

std::unique_ptr<Decoded> Decode(std::span<const std::byte> key, std::vector<std::pair<std::string, std::string>>* switches) {
    auto decoded = std::make_unique<Decoded>();
    Bytes bytes(key);
    static_cast<void>(bytes.Read<std::uint32_t>());
    static_cast<void>(bytes.Read<std::uint32_t>());
    static_cast<void>(bytes.Read<std::uint64_t>());
    const auto keyWords = bytes.Read<std::uint64_t>();
    if (keyWords > key.size() / 8u) throw std::runtime_error("not a variant key");
    const auto keySpan = bytes.Take(keyWords * 8u);
    decoded->storedKey.resize(keyWords);
    std::memcpy(decoded->storedKey.data(), keySpan.data(), keySpan.size());
    const auto codeWords = bytes.Read<std::uint64_t>();
    if (codeWords > key.size() / 4u) throw std::runtime_error("not a variant key");
    const auto codeSpan = bytes.Take(codeWords * 4u);
    decoded->code.resize(codeWords);
    std::memcpy(decoded->code.data(), codeSpan.data(), codeSpan.size());
    decoded->hostSubgroupSize = bytes.Read<std::uint32_t>();
    auto& request = decoded->request;
    request.layout.descriptorSet = bytes.Read<std::uint32_t>();
    request.layout.firstBinding = bytes.Read<std::uint32_t>();
    request.layout.pushConstantOffsetBytes = bytes.Read<std::uint32_t>();
    request.layout.pushConstantSizeBytes = bytes.Read<std::uint32_t>();
    while (!bytes.Done()) {
        const auto nameBytes = bytes.Read<std::uint64_t>();
        if (nameBytes > key.size()) throw std::runtime_error("not a variant key");
        const auto name = bytes.Take(nameBytes);
        const auto valueBytes = bytes.Read<std::uint64_t>();
        if (valueBytes > key.size()) throw std::runtime_error("not a variant key");
        const auto value = bytes.Take(valueBytes);
        if (switches != nullptr) switches->emplace_back(std::string(reinterpret_cast<const char*>(name.data()), name.size()), std::string(reinterpret_cast<const char*>(value.data()), value.size()));
    }

    Words words(decoded->storedKey);
    static_cast<void>(words.Next());
    request.shader.stage = words.As<ShaderStage>();
    const auto size = words.Next();
    static_cast<void>(words.Next());
    if (size != decoded->code.size()) throw std::runtime_error("code size differs from the key");
    request.shader.codeAddress = 0x1000000000ull + (std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(decoded->code.data()), decoded->code.size() * 4u)) & 0xffffff00ull);
    request.shader.code = decoded->code;
    auto& context = request.context;
    context.waveSize = words.As<std::uint32_t>();
    context.userDataBaseRegister = words.As<std::uint32_t>();
    const auto userWords = words.Next();
    if (userWords > 64u) throw std::runtime_error("user data count");
    decoded->userData.assign(userWords, 0u);
    context.userData = decoded->userData;
    if (words.Flag()) {
        ShaderComputeStageInfo compute{};
        ReadArray(words, compute.numThreads);
        compute.ldsSizeDwords = words.As<std::uint32_t>();
        ReadArray(words, compute.groupIdEnable);
        compute.tgSizeEnable = words.Flag();
        compute.threadIdComponentCount = words.As<std::uint32_t>();
        if (words.Flag()) compute.partialThreads = {1u, 1u, 1u};
        compute.scratchDwords = words.As<std::uint32_t>();
        context.compute = compute;
    }
    if (words.Flag()) {
        ShaderPixelStageInfo pixel{};
        pixel.interpolatorCount = words.As<std::uint32_t>();
        if (pixel.interpolatorCount > pixel.interpolatorSettings.size()) throw std::runtime_error("interpolator count");
        for (std::uint32_t index = 0; index < pixel.interpolatorCount; ++index) pixel.interpolatorSettings[index] = words.As<std::uint32_t>();
        pixel.wave32 = words.Flag();
        pixel.inputAddr = words.As<std::uint32_t>();
        pixel.hasPerspectiveCenterVgpr = words.Flag();
        pixel.perspectiveCentroid = words.Flag();
        pixel.posX = words.Flag();
        pixel.posY = words.Flag();
        pixel.posZ = words.Flag();
        pixel.posW = words.Flag();
        pixel.frontFace = words.Flag();
        pixel.frontFaceAllBits = words.Flag();
        pixel.ancillary = words.Flag();
        pixel.sampleShading = words.Flag();
        pixel.noPerspective = words.Flag();
        pixel.linearCentroid = words.Flag();
        pixel.pixelKillEnable = words.Flag();
        pixel.depthExportEnable = words.Flag();
        pixel.sampleMaskExportEnable = words.Flag();
        pixel.earlyZ = words.Flag();
        pixel.executeOnNoop = words.Flag();
        pixel.conservativeZExport = words.As<ConservativeZExport>();
        pixel.orderedPixelShader = words.Flag();
        pixel.alphaToCoverage = words.Flag();
        pixel.stencilRefExportEnable = words.Flag();
        pixel.ditheredAlphaToCoverage = words.Flag();
        pixel.quadPixelMask = words.As<std::uint8_t>();
        ReadArray(words, pixel.quadSampleMask);
        pixel.extraLdsDwords = words.As<std::uint32_t>();
        ReadArray(words, pixel.targetOutputMode);
        pixel.targetExportMapping.fill(0xe4u);
        context.pixel = pixel;
    }
    if (words.Flag()) {
        ShaderVertexStageInfo vertex{};
        vertex.fetchAttribReg = words.As<std::uint32_t>();
        vertex.fetchBufferReg = words.As<std::uint32_t>();
        vertex.fetchEmbedded = words.Flag();
        vertex.paClVsOutCntl = words.As<std::uint32_t>();
        vertex.resourcesNum = words.As<std::uint32_t>();
        if (vertex.resourcesNum > vertex.resources.size()) throw std::runtime_error("vertex resource count");
        for (std::uint32_t index = 0; index < vertex.resourcesNum; ++index) {
            if (!vertex.fetchEmbedded) {
                const auto wanted = words.Next();
                std::uint32_t format = 0;
                for (std::uint32_t candidate = 1; candidate < 0x80u; ++candidate) {
                    try {
                        if (static_cast<std::uint64_t>(VertexInputNumericClass(static_cast<IrBufferFormat>(candidate))) == wanted) {
                            format = candidate;
                            break;
                        }
                    } catch (const std::exception&) {
                    }
                }
                vertex.resources[index].fields = {0u, 0u, 0u, format << 12u};
            }
            auto& destination = vertex.resourcesDst[index];
            destination.registerStart = words.As<std::int32_t>();
            destination.registersNum = words.As<std::int32_t>();
            destination.attrId = words.As<std::int32_t>();
            destination.fetchIndex = words.As<std::uint32_t>();
        }
        context.vertex = vertex;
    }
    if (words.Flag()) {
        ShaderFloatMode mode;
        mode.floatMode = words.As<std::uint32_t>();
        mode.dx10Clamp = words.Flag();
        mode.ieeeMode = words.Flag();
        mode.fp16Overflow = words.Flag();
        context.floatMode = mode;
    }
    const auto fallbackCount = words.Next();
    if (fallbackCount > 4096u) throw std::runtime_error("fallback count");
    decoded->fallbacks.resize(fallbackCount);
    for (auto& fallback : decoded->fallbacks) fallback = words.As<std::uint32_t>();
    context.linkedCallFallbacks = decoded->fallbacks;
    if (request.shader.stage == ShaderStage::Mesh && words.Flag()) {
        MeshConfiguration mesh{};
        for (auto* field : {&mesh.inputPrimitive, &mesh.primitivesPerGroup, &mesh.verticesPerGroup, &mesh.maxVertices, &mesh.maxPrimitives, &mesh.threadsPerGroup, &mesh.ldsSizeDwords, &mesh.provokingVertex, &mesh.esgsItemSize}) *field = words.As<std::uint32_t>();
        request.graphics.emplace();
        request.graphics->mesh = mesh;
    }
    const bool tessellationStage = request.shader.stage == ShaderStage::Local || request.shader.stage == ShaderStage::TessellationControl || request.shader.stage == ShaderStage::TessellationEvaluation;
    if (tessellationStage && words.Flag()) {
        TessellationConfiguration tessellation{};
        for (auto* field : {&tessellation.inputControlPoints, &tessellation.outputControlPoints, &tessellation.domain, &tessellation.partitioning, &tessellation.outputTopology}) *field = words.As<std::uint32_t>();
        if (request.shader.stage != ShaderStage::Local) {
            tessellation.patchesPerGroup = words.As<std::uint32_t>();
            tessellation.primitiveGroupSize = words.As<std::uint32_t>();
            const auto instancePatches = words.As<std::uint32_t>();
            if (instancePatches != 0u) throw std::runtime_error("instanced tessellation patches are not replayed");
        }
        if (!request.graphics) request.graphics.emplace();
        request.graphics->tessellation = tessellation;
    }
    auto& target = request.target;
    target.vulkanVersion = words.As<std::uint32_t>();
    target.spirvVersion = words.As<std::uint32_t>();
    target.subgroupSize = words.As<std::uint32_t>();
    target.bdaAbiVersion = words.As<std::uint32_t>();
    const auto capabilityCount = words.Next();
    if (capabilityCount > 4096u) throw std::runtime_error("capability count");
    decoded->capabilities.resize(capabilityCount);
    for (auto& capability : decoded->capabilities) capability = words.As<std::uint32_t>();
    const auto extensionCount = words.Next();
    if (extensionCount > 4096u) throw std::runtime_error("extension count");
    decoded->extensionText.resize(extensionCount);
    for (auto& text : decoded->extensionText) {
        const auto length = words.Next();
        if (length > 4096u) throw std::runtime_error("extension length");
        text.resize(length);
        for (auto& character : text) character = static_cast<char>(words.Next());
    }
    for (const auto& text : decoded->extensionText) decoded->extensions.emplace_back(text);
    target.supportedCapabilities = decoded->capabilities;
    target.supportedExtensions = decoded->extensions;
    target.fragmentShaderBarycentricEnabled = words.Flag();
    ReadArray(words, target.maxWorkgroupSize);
    target.maxWorkgroupInvocations = words.As<std::uint32_t>();
    target.maxWorkgroupSharedMemoryBytes = words.As<std::uint32_t>();
    if (words.Flag()) {
        MeshTargetLimits mesh{};
        ReadArray(words, mesh.maxWorkgroupSize);
        for (auto* field : {&mesh.maxWorkgroupInvocations, &mesh.maxSharedMemoryBytes, &mesh.maxOutputVertices, &mesh.maxOutputPrimitives, &mesh.maxOutputComponents, &mesh.maxOutputMemoryBytes, &mesh.outputPerVertexGranularity, &mesh.outputPerPrimitiveGranularity}) *field = words.As<std::uint32_t>();
        target.mesh = mesh;
    }
    if (words.Flag()) {
        TessellationTargetLimits tessellation{};
        for (auto* field : {&tessellation.maxPatchSize, &tessellation.maxControlPerVertexInputComponents, &tessellation.maxControlPerVertexOutputComponents, &tessellation.maxControlPerPatchOutputComponents, &tessellation.maxControlTotalOutputComponents, &tessellation.maxEvaluationInputComponents, &tessellation.maxEvaluationOutputComponents}) *field = words.As<std::uint32_t>();
        target.tessellation = tessellation;
    }
    target.nonConstantImageOffsets = words.Flag();
    target.srgbDecodeFormats = words.As<std::uint32_t>();
    target.narrowSubgroupClock = words.Flag();
    target.float64DenormPreserve = words.Flag();
    target.registrationModules = words.Flag();
    target.registrationWordLimit = words.As<std::uint32_t>();
    target.vertexSubgroups = words.Flag();
    target.float16DenormPreserve = words.Flag();
    if (words.Flag()) throw std::runtime_error("debug probe entries are not replayed");
    static_cast<void>(words.Next());
    static_cast<void>(words.Next());
    if (!words.Done()) throw std::runtime_error("request key has unread words");
    request.useCache = false;
    std::vector<std::uint64_t> rebuilt;
    RecompileCacheKey::Build(request, rebuilt);
    if (rebuilt != decoded->storedKey) throw std::runtime_error("the decoded request does not rebuild its key");
    return decoded;
}

std::uint64_t Fnv(std::span<const std::uint32_t> words) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : words) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (word >> shift) & 0xffu;
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: shader_corpus <entry directory> <output> [every N] [threads]\n");
        return 2;
    }
    const std::filesystem::path directory = argv[1];
    const std::size_t every = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1u;
    const unsigned threads = argc > 4 ? static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10)) : 4u;
    std::vector<std::filesystem::path> entries;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() == ".bin") entries.push_back(file.path());
    }
    std::sort(entries.begin(), entries.end());
    std::vector<std::filesystem::path> picked;
    for (std::size_t index = 0; index < entries.size(); index += std::max<std::size_t>(every, 1u)) picked.push_back(entries[index]);
    std::vector<std::pair<std::string, std::string>> switches;
    for (const auto& path : picked) {
        std::ifstream file(path, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (raw.size() < 48u) continue;
        std::uint64_t keyBytes = 0;
        std::memcpy(&keyBytes, raw.data() + 16, 8);
        try {
            static_cast<void>(Decode(std::as_bytes(std::span(raw)).subspan(48, keyBytes), &switches));
            break;
        } catch (const std::exception&) {
            switches.clear();
        }
    }
    for (const auto& [name, value] : switches) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
        std::fprintf(stderr, "switch %s=%s\n", name.c_str(), value.c_str());
    }
    std::vector<std::string> lines(picked.size());
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> compiled{0}, skipped{0}, failed{0};
    std::atomic<std::uint64_t> totalMicros{0};
    std::vector<HANDLE> workerHandles(threads, nullptr);
    std::mutex handleMutex;
    std::atomic<bool> stopSampling{false};
    std::map<std::uint64_t, std::uint64_t> samples;
    const char* profilePath = std::getenv("CORPUS_PROFILE");
    std::thread sampler([&] {
        if (profilePath == nullptr) return;
        while (!stopSampling.load()) {
            std::vector<HANDLE> handles;
            {
                std::lock_guard lock(handleMutex);
                for (auto handle : workerHandles) if (handle != nullptr) handles.push_back(handle);
            }
            for (auto handle : handles) {
                if (SuspendThread(handle) == static_cast<DWORD>(-1)) continue;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(handle, &context)) ++samples[context.Rip];
                ResumeThread(handle);
            }
            Sleep(1);
        }
    });
    std::vector<std::thread> pool;
    for (unsigned worker = 0; worker < threads; ++worker) {
        pool.emplace_back([&, worker] {
            {
                HANDLE self = nullptr;
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
                std::lock_guard lock(handleMutex);
                workerHandles[worker] = self;
            }
            for (;;) {
                const auto index = next.fetch_add(1);
                if (index >= picked.size()) return;
                const auto& path = picked[index];
                std::ifstream file(path, std::ios::binary);
                std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                std::uint64_t keyBytes = 0;
                if (raw.size() >= 48u) std::memcpy(&keyBytes, raw.data() + 16, 8);
                std::unique_ptr<Decoded> decoded;
                try {
                    decoded = Decode(std::as_bytes(std::span(raw)).subspan(48, keyBytes), nullptr);
                } catch (const std::exception& error) {
                    ++skipped;
                    lines[index] = path.filename().string() + " skip " + error.what();
                    continue;
                }
                const auto start = std::chrono::steady_clock::now();
                try {
                    const auto result = Recompile(decoded->request);
                    const auto micros = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                    totalMicros += micros;
                    char text[160];
                    std::snprintf(text, sizeof(text), " ok %016llx %zu %llu", static_cast<unsigned long long>(Fnv(result.spirv.Words())), result.spirv.size(), static_cast<unsigned long long>(micros));
                    lines[index] = path.filename().string() + text;
                    ++compiled;
                } catch (const std::exception& error) {
                    ++failed;
                    lines[index] = path.filename().string() + " error " + error.what();
                }
            }
        });
    }
    for (auto& thread : pool) thread.join();
    stopSampling = true;
    sampler.join();
    if (profilePath != nullptr) {
        FILE* profile = std::fopen(profilePath, "w");
        for (const auto& [rip, count] : samples) std::fprintf(profile, "%llx %llu\n", static_cast<unsigned long long>(rip), static_cast<unsigned long long>(count));
        HMODULE modules[512];
        DWORD needed = 0;
        EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed);
        for (DWORD index = 0; index < needed / sizeof(HMODULE); ++index) {
            MODULEINFO info{};
            char name[MAX_PATH]{};
            GetModuleInformation(GetCurrentProcess(), modules[index], &info, sizeof(info));
            GetModuleFileNameA(modules[index], name, MAX_PATH);
            std::fprintf(profile, "module %llx %lx %s\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll)), static_cast<unsigned long>(info.SizeOfImage), name);
        }
        std::fclose(profile);
    }
    std::ofstream out(argv[2]);
    for (const auto& line : lines) out << line << '\n';
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto seconds = [](const FILETIME& time) { return static_cast<double>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime) / 1e7; };
    std::fprintf(stderr, "%zu picked, %zu compiled, %zu skipped, %zu failed, %.1f s compile wall summed, %.1f s process cpu\n", picked.size(), compiled.load(), skipped.load(), failed.load(), static_cast<double>(totalMicros.load()) / 1e6, seconds(user) + seconds(kernel));
    return 0;
}

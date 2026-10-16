#include "prx/libSceAgcDriver/Execution/include/DrawTraceIdentity.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "TraceIdentity.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

using namespace ShaderRecompiler;
using namespace AgcDriver;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string RawDigest(std::string_view text) {
    TraceIdentityBuilder digest;
    digest.Bytes(std::as_bytes(std::span(text.data(), text.size())));
    return digest.Finish();
}

void KnownSha256Vectors() {
    Check(RawDigest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA256 vector failed");
    Check(RawDigest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc SHA256 vector failed");
    constexpr std::string_view multi = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    constexpr std::string_view expected = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    Check(RawDigest(multi) == expected, "multi-block SHA256 vector failed");
    TraceIdentityBuilder incremental;
    for (const auto item : multi) incremental.Bytes(std::as_bytes(std::span(&item, 1)));
    Check(incremental.Finish() == expected && incremental.Finish() == expected, "incremental SHA256 or repeat Finish changed input");
}

void CanonicalEncoding() {
    TraceIdentityBuilder integer;
    integer.Integer(0x0807060504030201ull);
    const std::array<std::byte, 8> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    TraceIdentityBuilder raw;
    raw.Bytes(bytes);
    Check(integer.Finish() == raw.Finish(), "integer encoding is not little endian");
    TraceIdentityBuilder words;
    const std::array<std::uint32_t, 2> values{0x04030201u, 0x08070605u};
    words.Words(values);
    TraceIdentityBuilder explicitWords;
    explicitWords.Integer(2);
    explicitWords.Bytes(bytes);
    Check(words.Finish() == explicitWords.Finish(), "word encoding lost count or little endian bytes");
    TraceIdentityBuilder left, right;
    left.Text("ab"); left.Text("c");
    right.Text("a"); right.Text("bc");
    Check(left.Finish() != right.Finish(), "text framing permits concatenation ambiguity");
}

void SharedModuleIdentity() {
    SharedSpirv original(std::vector<std::uint32_t>{0x07230203u, 0x00010000u, 4u});
    const auto digest = original.TraceIdentity();
    auto copy = original;
    copy[2] = 9u;
    Check(original.TraceIdentity() == digest && copy.TraceIdentity() != digest, "copy-on-write reused the original module digest");
    auto owner = std::make_shared<const std::vector<std::uint32_t>>(original.Copy());
    SharedSpirv mapped(owner, *owner);
    Check(mapped.TraceIdentity() == digest, "mapped and owned modules have different content identities");
    mapped[1] = 7u;
    Check(mapped.TraceIdentity() != digest && original.TraceIdentity() == digest, "mapped mutation retained a stale identity");
    SharedSpirv building(std::vector<std::uint32_t>{0u, 0u, 0u});
    const auto* cache = building.TraceCache().get();
    Check(cache != nullptr, "detail-mode digest cache was not allocated");
    for (std::size_t index = 0; index < building.size(); ++index) building[index] = static_cast<std::uint32_t>(index);
    Check(building.TraceCache().get() == cache, "unhashed mutable access allocated a cache per word");
    const auto oldDigest = building.TraceIdentity();
    building[0] = 20u;
    Check(building.TraceIdentity() != oldDigest, "owned mutation retained a stale identity");
    std::array<std::string, 4> results;
    std::array<std::thread, 4> readers;
    for (std::size_t index = 0; index < readers.size(); ++index) readers[index] = std::thread([&, index] { results[index] = original.TraceIdentity(); });
    for (auto& reader : readers) reader.join();
    for (const auto& result : results) Check(result == digest, "concurrent module identity changed");
}

void SourceAndCompilerIdentity() {
    const std::array<std::uint32_t, 2> code{1u, 2u};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x1000u, code, 0u, {}};
    const auto original = MakeShaderTraceSourceIdentity(request, 32u);
    Check(original != nullptr && original->guestIdentity.size() == 64u && original->recompileKeyIdentity.size() == 64u, "cold source identities missing");
    request.shader.codeAddress = 0x9000u;
    const auto relocated = MakeShaderTraceSourceIdentity(request, 32u);
    Check(relocated->guestIdentity == original->guestIdentity && relocated->recompileKeyIdentity == original->recompileKeyIdentity, "guest relocation entered compiler content identity");
    const auto subgroup = MakeShaderTraceSourceIdentity(request, 64u);
    Check(subgroup->guestIdentity == original->guestIdentity && subgroup->recompileKeyIdentity != original->recompileKeyIdentity, "compiler subgroup configuration was not separated from guest identity");
    request.shader.stage = ShaderStage::Vertex;
    Check(MakeShaderTraceSourceIdentity(request, 32u)->guestIdentity != original->guestIdentity, "source identity omitted guest stage");
}

RecompileResult Shader() {
    RecompileResult shader;
    shader.spirv = std::vector<std::uint32_t>{0x07230203u, 0x00010000u, 0u, 2u, 0u};
    shader.specialization = {{7u, 11u}, {3u, 9u}};
    shader.bindings.push_back({DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 0u, 1u, 1u});
    shader.pushConstants.resize(16u);
    shader.hostSubgroupSize = 32u;
    FinalizeShaderTraceIdentity(shader);
    return shader;
}

DrawTrace::Options Configuration() {
    DrawTrace::Options options;
    options.runId = "11111111-1111-4111-8111-111111111111";
    options.captureId = "22222222-2222-4222-8222-222222222222";
    return options;
}

void ShaderVariantSemanticsAndEpochs() {
    auto shader = Shader();
    const auto options = Configuration();
    DrawTrace::Session first(options), second(options);
    DrawTrace::Id initial = 0;
    std::string stable;
    {
        DrawTrace::SessionScope scope(first);
        const std::array<std::uint32_t, 2> guest{1u, 2u};
        ShaderBinary binary{ShaderStage::Vertex, 0x1000u, guest, 0u, {}};
        const auto guestId = DrawTrace::RegisterGuestShaderIdentity(binary);
        binary.codeAddress = 0x9000u;
        Check(DrawTrace::RegisterGuestShaderIdentity(binary) == guestId, "guest relocation changed content identity");
        binary.stage = ShaderStage::Fragment;
        Check(DrawTrace::RegisterGuestShaderIdentity(binary) != guestId, "guest stage was excluded from identity");
        initial = DrawTrace::RegisterShaderIdentity(ShaderStage::Vertex, shader);
        Check(initial != 0 && DrawTrace::RegisterShaderIdentity(ShaderStage::Vertex, shader) == initial, "shader metadata cache did not deduplicate");
        stable = DrawTrace::ComputePipelineIdentity(shader, true, {});
        std::reverse(shader.specialization.begin(), shader.specialization.end());
        FinalizeShaderTraceIdentity(shader);
        Check(DrawTrace::RegisterShaderIdentity(ShaderStage::Vertex, shader) == initial, "specialization ordering changed stable identity");
        shader.pushConstants[0] = std::byte{42};
        shader.bindings[0].guestDescriptor = {4u, 8u, 12u};
        shader.variantId = 555u;
        FinalizeShaderTraceIdentity(shader);
        Check(DrawTrace::RegisterShaderIdentity(ShaderStage::Vertex, shader) == initial, "invocation payload or process ID changed stable variant identity");
        Check(DrawTrace::ComputePipelineIdentity(shader, true, {}) == stable, "invocation payload changed compute pipeline identity");
    }
    {
        DrawTrace::SessionScope scope(second);
        Check(DrawTrace::RegisterMetadata("synthetic", "reserve-first-id") != 0, "synthetic metadata reservation failed");
        const auto renewed = DrawTrace::RegisterShaderIdentity(ShaderStage::Vertex, shader);
        Check(renewed != 0, "session reuse failed to register metadata");
        Check(shader.traceMetadata->epoch == second.Identity(), "shader metadata cache retained a foreign session epoch");
        shader.specialization[0].value += 1u;
        FinalizeShaderTraceIdentity(shader);
        Check(DrawTrace::RegisterShaderIdentity(ShaderStage::Vertex, shader) != renewed, "changed specialization reused variant identity");
        const auto changed = DrawTrace::ComputePipelineIdentity(shader, true, {});
        Check(changed != stable, "changed specialization reused compute pipeline identity");
        const auto changedStage = DrawTrace::RegisterShaderIdentity(ShaderStage::Vertex, shader);
        shader.spirv[2] = 99u;
        Check(DrawTrace::ComputePipelineIdentity(shader, true, {}) != changed, "patched final SPIRV reused pipeline identity");
        Check(DrawTrace::RegisterShaderIdentity(ShaderStage::Vertex, shader) != changedStage, "module copy-on-write failed to invalidate cached shader metadata");
        const std::array<std::uint32_t, 1> layout{1u};
        Check(DrawTrace::ComputePipelineIdentity(shader, true, layout) != DrawTrace::ComputePipelineIdentity(shader, true, {}), "descriptor layout omitted from compute identity");
        Check(DrawTrace::ComputePipelineIdentity(shader, true, {}, 32u) != DrawTrace::ComputePipelineIdentity(shader, true, {}, 64u), "actual subgroup requirement omitted from compute identity");
        Check(DrawTrace::ComputePipelineIdentity(shader, true, {}, 32u, 1u) != DrawTrace::ComputePipelineIdentity(shader, true, {}, 32u, 0u), "actual pipeline flags omitted from compute identity");
        shader.specialization.push_back(shader.specialization.front());
        bool rejected = false;
        try { FinalizeShaderTraceIdentity(shader); } catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "duplicate specialization constants were accepted");
        second.RequestStop("synthetic completion", DrawTrace::Clock::now());
        second.Freeze(DrawTrace::Clock::now());
        Check(second.Json().find("shader_variant") != std::string::npos, "session reuse returned a foreign cached metadata ID");
    }
}

void BuiltinPipelineSemantics() {
    DrawTrace::Session session(Configuration());
    DrawTrace::SessionScope scope(session);
    std::array<std::uint32_t, 5> code{0x07230203u, 0x00010000u, 0u, 2u, 0u};
    std::array<VkPushConstantRange, 1> pushes{{{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}}};
    std::array<std::uint32_t, 3> layout{0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u};
    std::array<std::uint32_t, 2> values{7u, 11u};
    std::array<VkSpecializationMapEntry, 2> entries{{{9u, 0u, 4u}, {3u, 4u, 4u}}};
    DrawTrace::BuiltinComputePipelineState state;
    state.pushes = pushes;
    state.descriptorLayout = layout;
    state.specializationEntries = entries;
    state.specializationData = std::as_bytes(std::span(values));
    const auto identity = [&] { return DrawTrace::RegisterBuiltinComputePipelineIdentity("synthetic_helper", code, state); };
    const auto initial = identity();
    Check(initial != 0 && identity() == initial, "builtin identity was absent or unstable");
    std::swap(entries[0], entries[1]);
    Check(identity() == initial, "specialization map ordering changed semantic builtin identity");
    values[0] = 8u;
    Check(identity() != initial, "builtin specialization value was omitted");
    values[0] = 7u;
    pushes[0].size = 32u;
    Check(identity() != initial, "builtin push layout was omitted");
    pushes[0].size = 16u;
    layout[2] = 2u;
    Check(identity() != initial, "builtin descriptor layout was omitted");
    layout[2] = 1u;
    state.pipelineFlags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    Check(identity() != initial, "builtin pipeline flags were omitted");
    state.pipelineFlags = 0;
    state.stageFlags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    Check(identity() != initial, "builtin stage flags were omitted");
    state.stageFlags = 0;
    state.requiredSubgroupSize = 32u;
    Check(identity() != initial, "builtin subgroup size was omitted");
    state.requiredSubgroupSize = 0;
    state.entryPoint = "other";
    Check(identity() != initial, "builtin entry point was omitted");
    state.entryPoint = "main";
    code[3] = 3u;
    Check(identity() != initial, "selected builtin SPIR-V was omitted");
    entries[0].offset = 1000u;
    bool rejected = false;
    try { static_cast<void>(identity()); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "invalid builtin specialization range was accepted");
    entries[0].offset = entries[1].offset;
    entries[0].constantID = entries[1].constantID;
    rejected = false;
    try { static_cast<void>(identity()); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "duplicate builtin specialization IDs were accepted");
    session.Freeze(DrawTrace::Clock::now());
    const auto json = session.Json();
    Check(json.find("\"source_kind\":\"builtin\"") != std::string::npos, "builtin source provenance was not explicit");
    Check(json.find("\"kind\":\"guest_shader\"") == std::string::npos && json.find("\"kind\":\"compiler_key\"") == std::string::npos, "builtin identity fabricated guest or compiler provenance");
}

void PipelineSemantics() {
    DrawTrace::Session session(Configuration());
    DrawTrace::SessionScope scope(session);
    auto shader = Shader();
    Graphics::Context context{};
    Graphics::State state{};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.samples = 1;
    Graphics::ColorTarget color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    state.colors.push_back(color);
    state.hasColorTarget = true;
    Graphics::VertexInputLayout input;
    Graphics::PipelineSetLayouts resources;
    const std::array<Graphics::CompiledShader, 1> shaders{{{ShaderStage::Vertex, &shader, 0u}}};
    const auto identity = [&] { return DrawTrace::GraphicsPipelineIdentity(context, state, input, resources, shaders, VK_IMAGE_LAYOUT_GENERAL); };
    const auto initial = identity();
    state.colors[0].address = 0xabc000u;
    state.viewport.width = 640.0f;
    state.scissor.extent = {640u, 480u};
    context.queueFamily = 9u;
    Check(identity() == initial, "guest address, dynamic viewport or queue family entered semantic pipeline key");
    state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    Check(identity() != initial, "topology omitted from semantic pipeline key");
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.colors[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    Check(identity() != initial, "attachment format omitted from semantic pipeline key");
    const auto stages = DrawTrace::RegisterShaderStages(shaders);
    Check(stages.size() == 1 && stages[0] != 0, "pipeline stage metadata missing");
    Check(DrawTrace::RegisterPipelineIdentity(identity(), stages) != 0, "semantic pipeline metadata missing");
    const std::array<DrawTrace::Id, 1> missing{0};
    Check(DrawTrace::RegisterPipelineIdentity(initial, missing) == 0, "pipeline emitted a dangling stage reference");
}

}

int main() {
    try {
#ifdef _WIN32
        Check(_putenv_s("APS5_TRACE_DETAIL", "draw") == 0, "cannot enable synthetic detail mode");
#else
        Check(setenv("APS5_TRACE_DETAIL", "draw", 1) == 0, "cannot enable synthetic detail mode");
#endif
        KnownSha256Vectors();
        CanonicalEncoding();
        SharedModuleIdentity();
        SourceAndCompilerIdentity();
        ShaderVariantSemanticsAndEpochs();
        BuiltinPipelineSemantics();
        PipelineSemantics();
        std::cout << "Draw trace identity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ShaderRecompiler;
using AgcDriver::Graphics::Require;

constexpr std::uint32_t Extent = 16;
constexpr std::uint32_t Dim2DMsaa = 6;
constexpr std::uint32_t Dim2DMsaaArray = 7;
alignas(256) std::array<std::uint32_t, Extent * Extent * 4> pixels{};
constexpr std::array<std::array<float, 4>, 3> positions{{{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}};
constexpr std::array<std::uint32_t, 6> vertexCode{0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};

std::array<std::uint32_t, 10> LoadCode(std::uint32_t dimension) {
    return {0x7e000280u, 0x7e020280u, 0x7e040280u, 0x7e060280u, 0xf0001f00u | (dimension << 3u), 0x00000400u, 0xbf8c3f70u, 0xf800180fu, 0x07060504u, 0xbf810000u};
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t dimension, DescriptorImageShape expected) {
    const std::string name = dimension == Dim2DMsaa ? "2D multisampled" : "2D multisampled array";
    const auto target = device.Target();
    const auto address = reinterpret_cast<std::uintptr_t>(positions.data());
    const std::array<std::uint32_t, 4> vertexUsers{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (16u << 16u), 3, 0x01016facu};
    RecompileRequest vertexRequest{{ShaderStage::Vertex, 0x10000, vertexCode, 0, {}}, {32, 0, vertexUsers, {}, {}, ShaderVertexStageInfo{}, {}}, target, {0, 0, 0, 128}};
    const auto vertex = Recompile(vertexRequest);
    const auto code = LoadCode(dimension);
    const std::vector<std::uint32_t> users(8, 0u);
    AgcDriver::Registers context{{0x1b6, 0x8000}, {0x1b3, 0}, {0x1b4, 0}, {0x203, 0}, {0x1c5, 9}};
    RecompileRequest request{{ShaderStage::Fragment, 0x20000, code, 0, {}}, {32, 0, users, {}, AgcDriver::Graphics::DecodePixelStageInfo(context, {}), {}, {}}, target, PixelPushLayout(0u, target)};
    const auto handle = PrepareShader(request);
    AgcDriver::DriverDetail::ShaderSnapshot snapshot{0x20000, 0, 1, {code.begin(), code.end()}, {}};
    snapshot.prepared->entries.push_back({0, handle});
    request.shader.code = snapshot.code;
    if (target.registrationModules) {
        Require(GetPreparedArtifact(*handle).registrationModule, (name + ": the fragment shader was not prepared as a registration module").c_str());
        const auto registration = RegistrationResult(request, *handle);
        Require(registration != nullptr, (name + ": registration materialization returned no result").c_str());
        const bool shaped = std::ranges::any_of(registration->bindings, [&](const DescriptorBinding& binding) { return binding.role == DescriptorRole::GuestImages && binding.imageShape == expected; });
        Require(shaped, (name + ": the registration bindings have no image heap of the multisampled shape").c_str());
        const std::array<AgcDriver::Graphics::CompiledShader, 1> shaders{{{ShaderStage::Fragment, registration.get(), PixelPushOffset(0u, target)}}};
        const AgcDriver::Graphics::ShaderStages stages{AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, {}, {}};
        const auto before = AgcDriver::Graphics::PipelineLibraryCountersOf(device.Device());
        Require(device.PrebuildGraphicsLibraries(stages, shaders), (name + ": the fragment library was not prebuilt").c_str());
        const auto after = AgcDriver::Graphics::PipelineLibraryCountersOf(device.Device());
        Require(after.built[2] - before.built[2] == 1u, (name + ": the prebuild did not build one fragment library").c_str());
    }
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0, 32, 32, {}, {}};
    state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), {Extent, Extent}, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(pixels), 0};
    state.color.elementBytes = 16;
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Extent, Extent};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Extent), static_cast<float>(Extent), -static_cast<float>(Extent), 0, 1};
    state.scissor = {{0, 0}, {Extent, Extent}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    request.context.pixel = AgcDriver::Graphics::DecodePixelStageInfo(context, AgcDriver::Graphics::ExportMappings(state));
    const auto invocation = AgcDriver::DriverDetail::InvocationFor(snapshot, 0, request);
    SrtRuntime runtime{users};
    const auto pixel = invocation.Materialize(*invocation.Capture(runtime));
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0}, {ShaderStage::Fragment, pixel.get(), PixelPushOffset(0u, target)}}};
    const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};
    pixels.fill(0xdeadbeefu);
    device.Draw(state, draw, shaders);
    device.WaitIdle();
    Require(std::ranges::all_of(pixels, [](std::uint32_t value) { return value == 0u; }), (name + ": a load through a null T# did not export zero").c_str());
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, Dim2DMsaa, DescriptorImageShape::Image2DMultisample);
        Run(*device, Dim2DMsaaArray, DescriptorImageShape::Image2DMultisampleArray);
        std::cout << "multisampled image prebuild tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

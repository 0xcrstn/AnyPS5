#include "prx/libSceAgcDriver/Graphics/include/PipelineLibraryNotes.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void FirstFragmentBuild() {
    const auto text = DescribeLibraryBuild({FragmentShaderLibraryPart, 5980.4, 1.2, true, false, 0u});
    Require(Contains(text, "fragment shader library 5980 ms in vkCreateGraphicsPipelines after 1 ms waiting for the pipeline cache"), "the fragment build does not name its part and times: " + text);
    Require(Contains(text, "retains link-time optimization info"), "the fragment build does not say it retains link-time optimization info: " + text);
    Require(Contains(text, "the first for these shaders"), "the first build of a shader is not named as such: " + text);
    Require(!Contains(text, "built again"), "the first build of a shader is described as a rebuild: " + text);
}

void RebuiltForFields() {
    const auto text = DescribeLibraryBuild({PreRasterizationLibraryPart, 310.0, 0.0, false, true, (std::uint64_t{1} << 0) | (std::uint64_t{1} << 3)});
    Require(Contains(text, "pre-rasterization library 310 ms"), "the pre-rasterization build does not name its part: " + text);
    Require(Contains(text, "built again for these shaders because push offset, depth clamp differed"), "the rebuild does not name the fields that differed: " + text);
    Require(!Contains(text, "retains link-time optimization info"), "a build without link-time optimization claims to retain its info: " + text);
    const auto same = DescribeLibraryBuild({FragmentShaderLibraryPart, 20.0, 0.0, false, true, 0u});
    Require(Contains(same, "built again for these shaders with the same fixed state"), "a rebuild with equal fields is not described: " + same);
    const auto fragment = DescribeLibraryBuild({FragmentShaderLibraryPart, 20.0, 0.0, false, true, std::uint64_t{1} << 1});
    Require(Contains(fragment, "because samples differed"), "the fragment field names are not used for a fragment rebuild: " + fragment);
}

void LinkStep() {
    const auto text = DescribeLibraryBuild({PipelineLinkPart, 4.4, 0.3, false, false, 0u});
    Require(text == "link 4 ms in vkCreateGraphicsPipelines after 0 ms waiting for the pipeline cache", "the link step is not described as a link: " + text);
}

void Joined() {
    Require(DescribeLibraryBuilds({}).empty(), "no builds produce text");
    const std::vector<LibraryBuildNote> notes{{FragmentShaderLibraryPart, 100.0, 0.0, false, false, 0u}, {PipelineLinkPart, 2.0, 0.0, false, false, 0u}};
    const auto text = DescribeLibraryBuilds(notes);
    Require(text == DescribeLibraryBuild(notes[0]) + "; " + DescribeLibraryBuild(notes[1]), "builds are not joined in order: " + text);
}

}

int main() {
    try {
        FirstFragmentBuild();
        RebuiltForFields();
        LinkStep();
        Joined();
        std::printf("pipeline library notes tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINELIBRARYNOTES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINELIBRARYNOTES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace AgcDriver::Graphics {

inline constexpr std::array<const char*, 6> PreRasterizationLibraryFields{"push offset", "provoking vertex", "clip range", "depth clamp", "conservative rasterization", "tessellation"};
inline constexpr std::array<const char*, 3> FragmentLibraryFields{"push offset", "samples", "sample locations"};

inline constexpr std::size_t PreRasterizationLibraryPart = 1;
inline constexpr std::size_t FragmentShaderLibraryPart = 2;
inline constexpr std::size_t PipelineLinkPart = 4;
inline constexpr std::array<const char*, 5> LibraryPartNames{"vertex input", "pre-rasterization", "fragment shader", "fragment output", "link"};

struct LibraryBuildNote {
    std::size_t part = 0;
    double createMs = 0;
    double lockWaitMs = 0;
    bool retainsLinkTimeOptimization = false;
    bool rebuilt = false;
    std::uint64_t differingFields = 0;
};

inline std::span<const char* const> LibraryFieldNames(std::size_t part) {
    if (part == PreRasterizationLibraryPart) return PreRasterizationLibraryFields;
    if (part == FragmentShaderLibraryPart) return FragmentLibraryFields;
    return {};
}

inline std::string DescribeLibraryBuild(const LibraryBuildNote& note) {
    char timing[96];
    std::snprintf(timing, sizeof(timing), "%.0f ms in vkCreateGraphicsPipelines after %.0f ms waiting for the pipeline cache", note.createMs, note.lockWaitMs);
    const char* name = note.part < LibraryPartNames.size() ? LibraryPartNames[note.part] : "unknown";
    if (note.part == PipelineLinkPart) return std::string(name) + " " + timing;
    std::string text = std::string(name) + " library " + timing;
    if (note.retainsLinkTimeOptimization) text += ", retains link-time optimization info";
    if (!note.rebuilt) return text + ", the first for these shaders";
    text += ", built again for these shaders";
    const auto fields = LibraryFieldNames(note.part);
    std::string differing;
    for (std::size_t field = 0; field < fields.size(); ++field) {
        if ((note.differingFields & (std::uint64_t{1} << field)) == 0) continue;
        differing += (differing.empty() ? "" : ", ") + std::string(fields[field]);
    }
    return text + (differing.empty() ? " with the same fixed state" : " because " + differing + " differed");
}

inline std::string DescribeLibraryBuilds(std::span<const LibraryBuildNote> notes) {
    std::string text;
    for (const auto& note : notes) text += (text.empty() ? "" : "; ") + DescribeLibraryBuild(note);
    return text;
}

}

#endif

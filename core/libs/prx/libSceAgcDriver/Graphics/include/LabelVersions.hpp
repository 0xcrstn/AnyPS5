#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LABELVERSIONS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LABELVERSIONS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace AgcDriver::Graphics::LabelVersions {

enum class Milestone : std::uint8_t { Recorded = 0, Submitted, DeviceReady, HostPublished, Retired };

inline constexpr std::size_t MilestoneCount = 5;
inline constexpr std::array<const char*, MilestoneCount> MilestoneNames{"recorded", "submitted", "device-ready", "host-published", "retired"};

struct Version {
    std::uint64_t stamp = 0;
    std::uint64_t address = 0;
    std::uint32_t bytes = 0;
    std::uint32_t queue = 0;
    std::array<std::int64_t, MilestoneCount> ns{};

    bool Reached(Milestone milestone) const { return ns[static_cast<std::size_t>(milestone)] != 0; }
    std::int64_t At(Milestone milestone) const { return ns[static_cast<std::size_t>(milestone)]; }
};

bool Enabled();
void SetEnabledForTest(bool enabled);
std::int64_t NowNs();
void Record(std::uint64_t stamp, std::uint64_t address, std::uint32_t bytes, std::uint32_t queue);
void Mark(std::uint64_t stamp, Milestone milestone);
void MarkAll(std::span<const std::uint64_t> stamps, Milestone milestone);
void MarkHostWritten(std::uint64_t stamp);
std::optional<Version> Find(std::uint64_t stamp);
std::string Describe(const Version& version, std::int64_t originNs);
std::string TakeReport();

}

#endif

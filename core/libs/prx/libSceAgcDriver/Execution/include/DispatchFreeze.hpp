#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHFREEZE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHFREEZE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::DispatchFreeze {

struct Reach {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    bool writable = false;
};

struct Range {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    bool writable = false;
    std::vector<std::byte> before;
    std::vector<std::byte> after;
};

struct Capture {
    std::string request;
    std::array<std::uint32_t, 3> groups{};
    std::uint64_t arguments = 0;
    std::uint64_t program = 0;
    std::vector<Range> ranges;
};

void Arm(std::filesystem::path path, std::string request);
[[nodiscard]] bool Armed();
void NoteDispatch(std::array<std::uint32_t, 3> groups, std::uint64_t arguments, std::uint64_t program, std::span<const Reach> reachable);
void Finish();
void Disarm();

void Write(const std::filesystem::path& path, const Capture& capture);
[[nodiscard]] Capture Read(const std::filesystem::path& path);
[[nodiscard]] std::vector<Range> Merge(std::span<const Reach> reachable);

}

#endif

#include "prx/libSceAgcDriver/Execution/include/DispatchFreeze.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace AgcDriver::DispatchFreeze {

namespace {

constexpr std::array<char, 8> Magic{'A', 'P', 'S', '5', 'F', 'R', 'Z', '1'};
constexpr std::uint32_t Version = 1;

struct Pending {
    std::filesystem::path path;
    Capture capture;
    bool noted = false;
    bool skipped = false;
};

thread_local Pending* pending = nullptr;

std::uint64_t LimitBytes() {
    static const std::uint64_t limit = [] {
        const char* text = std::getenv("APS5_FREEZE_MAX_MIB");
        if (text == nullptr) return std::uint64_t{512} << 20u;
        char* end = nullptr;
        const auto value = std::strtoull(text, &end, 10);
        if (end == text || *end != '\0' || value == 0) throw std::runtime_error("APS5_FREEZE_MAX_MIB must be a positive number of MiB");
        return value << 20u;
    }();
    return limit;
}

template<typename T>
void put(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template<typename T>
T take(std::ifstream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) throw std::runtime_error("truncated dispatch capture");
    return value;
}

void readBytes(std::ifstream& input, std::vector<std::byte>& bytes, std::uint64_t count) {
    bytes.resize(static_cast<std::size_t>(count));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    if (!input) throw std::runtime_error("truncated dispatch capture");
}

}

std::vector<Range> Merge(std::span<const Reach> reachable) {
    std::vector<Reach> sorted(reachable.begin(), reachable.end());
    sorted.erase(std::remove_if(sorted.begin(), sorted.end(), [](const Reach& reach) { return reach.end <= reach.begin; }), sorted.end());
    std::sort(sorted.begin(), sorted.end(), [](const Reach& left, const Reach& right) { return left.begin < right.begin; });
    std::vector<Range> merged;
    for (const auto& reach : sorted) {
        if (!merged.empty() && reach.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, reach.end);
            merged.back().writable = merged.back().writable || reach.writable;
            continue;
        }
        merged.push_back({reach.begin, reach.end, reach.writable, {}, {}});
    }
    return merged;
}

void Arm(std::filesystem::path path, std::string request) {
    delete pending;
    pending = new Pending();
    pending->path = std::move(path);
    pending->capture.request = std::move(request);
}

bool Armed() {
    return pending != nullptr;
}

void Disarm() {
    delete pending;
    pending = nullptr;
}

void NoteDispatch(std::array<std::uint32_t, 3> groups, std::uint64_t arguments, std::uint64_t program, std::span<const Reach> reachable) {
    if (pending == nullptr || pending->noted) return;
    pending->noted = true;
    auto& capture = pending->capture;
    capture.groups = groups;
    capture.arguments = arguments;
    capture.program = program;
    std::vector<Reach> all(reachable.begin(), reachable.end());
    if (arguments != 0) all.push_back({arguments, arguments + 12u, false});
    capture.ranges = Merge(all);
    std::uint64_t total = 0;
    for (const auto& range : capture.ranges) total += range.end - range.begin;
    if (total > LimitBytes()) {
        std::fprintf(stderr, "[freeze] %s: the dispatch reaches %llu MiB, more than APS5_FREEZE_MAX_MIB; not captured\n", pending->path.string().c_str(), static_cast<unsigned long long>(total >> 20u));
        pending->skipped = true;
        capture.ranges.clear();
        return;
    }
    for (auto& range : capture.ranges) {
        range.before.resize(static_cast<std::size_t>(range.end - range.begin));
        GuestMemory::Read(range.begin, range.before);
    }
}

void Finish() {
    if (pending == nullptr) return;
    const std::unique_ptr<Pending> armed(pending);
    pending = nullptr;
    if (!armed->noted || armed->skipped) {
        if (!armed->noted) std::fprintf(stderr, "[freeze] %s: the dispatch did not reach the record step; not captured\n", armed->path.string().c_str());
        return;
    }
    for (auto& range : armed->capture.ranges) {
        if (!range.writable) continue;
        range.after.resize(range.before.size());
        GuestMemory::Read(range.begin, range.after);
    }
    Write(armed->path, armed->capture);
    std::fprintf(stderr, "[freeze] wrote %s: %zu ranges, groups %ux%ux%u\n", armed->path.string().c_str(), armed->capture.ranges.size(), armed->capture.groups[0], armed->capture.groups[1], armed->capture.groups[2]);
}

void Write(const std::filesystem::path& path, const Capture& capture) {
    if (std::filesystem::exists(path)) throw std::runtime_error("dispatch capture already exists: " + path.string());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create dispatch capture " + path.string());
    output.write(Magic.data(), Magic.size());
    put(output, Version);
    put(output, static_cast<std::uint64_t>(capture.request.size()));
    output.write(capture.request.data(), static_cast<std::streamsize>(capture.request.size()));
    for (const auto group : capture.groups) put(output, group);
    put(output, capture.arguments);
    put(output, capture.program);
    put(output, static_cast<std::uint32_t>(capture.ranges.size()));
    for (const auto& range : capture.ranges) {
        put(output, range.begin);
        put(output, range.end);
        put(output, static_cast<std::uint8_t>(range.writable ? 1u : 0u));
        output.write(reinterpret_cast<const char*>(range.before.data()), static_cast<std::streamsize>(range.before.size()));
        if (range.writable) output.write(reinterpret_cast<const char*>(range.after.data()), static_cast<std::streamsize>(range.after.size()));
    }
    if (!output) throw std::runtime_error("cannot write dispatch capture " + path.string());
}

Capture Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open dispatch capture " + path.string());
    std::array<char, 8> magic{};
    input.read(magic.data(), magic.size());
    if (!input || magic != Magic) throw std::runtime_error("not a dispatch capture: " + path.string());
    if (take<std::uint32_t>(input) != Version) throw std::runtime_error("unsupported dispatch capture version: " + path.string());
    Capture capture;
    const auto requestBytes = take<std::uint64_t>(input);
    if (requestBytes > (std::uint64_t{64} << 20u)) throw std::runtime_error("dispatch capture request is too large");
    capture.request.resize(static_cast<std::size_t>(requestBytes));
    input.read(capture.request.data(), static_cast<std::streamsize>(requestBytes));
    for (auto& group : capture.groups) group = take<std::uint32_t>(input);
    capture.arguments = take<std::uint64_t>(input);
    capture.program = take<std::uint64_t>(input);
    const auto count = take<std::uint32_t>(input);
    for (std::uint32_t index = 0; index < count; ++index) {
        Range range;
        range.begin = take<std::uint64_t>(input);
        range.end = take<std::uint64_t>(input);
        range.writable = take<std::uint8_t>(input) != 0u;
        if (range.end <= range.begin || range.end - range.begin > LimitBytes()) throw std::runtime_error("dispatch capture has an invalid range");
        readBytes(input, range.before, range.end - range.begin);
        if (range.writable) readBytes(input, range.after, range.end - range.begin);
        capture.ranges.push_back(std::move(range));
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("dispatch capture has trailing data");
    return capture;
}

}

#include "prx/libSceAgcDriver/Execution/include/DispatchFreeze.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "Recompiler.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

using namespace AgcDriver;

struct Options {
    std::filesystem::path capture;
    std::uint32_t repeats = 5;
    bool allowMismatch = false;
};

Options Parse(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--repeat" && index + 1 < argc) {
            char* end = nullptr;
            const auto value = std::strtoul(argv[++index], &end, 10);
            if (end == argv[index] || *end != '\0' || value == 0 || value > 10000) throw std::runtime_error("--repeat needs a count from 1 to 10000");
            options.repeats = static_cast<std::uint32_t>(value);
        } else if (argument == "--allow-mismatch") {
            options.allowMismatch = true;
        } else if (argument.starts_with("--") || !options.capture.empty()) {
            throw std::runtime_error("usage: agc_dispatch_replay <capture> [--repeat N] [--allow-mismatch]");
        } else {
            options.capture = argument;
        }
    }
    if (options.capture.empty()) throw std::runtime_error("usage: agc_dispatch_replay <capture> [--repeat N] [--allow-mismatch]");
    return options;
}

class GuestWindows {
public:
    explicit GuestWindows(const std::vector<DispatchFreeze::Range>& ranges) {
        constexpr std::uint64_t granularity = 64u * 1024u;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> windows;
        for (const auto& range : ranges) {
            const auto begin = range.begin / granularity * granularity;
            const auto end = (range.end + granularity - 1u) / granularity * granularity;
            if (!windows.empty() && begin <= windows.back().second) windows.back().second = std::max(windows.back().second, end);
            else windows.emplace_back(begin, end);
        }
        for (const auto& [begin, end] : windows) {
            const auto bytes = static_cast<std::size_t>(end - begin);
#ifdef _WIN32
            void* pointer = VirtualAlloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(begin)), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
            void* pointer = mmap(reinterpret_cast<void*>(static_cast<std::uintptr_t>(begin)), bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (pointer == MAP_FAILED) pointer = nullptr;
#endif
            if (pointer != reinterpret_cast<void*>(static_cast<std::uintptr_t>(begin))) {
                char text[160];
                std::snprintf(text, sizeof(text), "the guest range 0x%llx-0x%llx is not free in this process", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end));
                throw std::runtime_error(text);
            }
            mapped.emplace_back(pointer, bytes);
            GuestAllocations::Mutation().Add(pointer, bytes, true, true, true);
        }
    }

    ~GuestWindows() {
        for (const auto& [pointer, bytes] : mapped) {
            GuestAllocations::Mutation().Remove(pointer);
#ifdef _WIN32
            VirtualFree(pointer, 0, MEM_RELEASE);
#else
            munmap(pointer, bytes);
#endif
        }
    }

    GuestWindows(const GuestWindows&) = delete;
    GuestWindows& operator=(const GuestWindows&) = delete;

private:
    std::vector<std::pair<void*, std::size_t>> mapped;
};

std::byte* At(std::uint64_t address) {
    return reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(address));
}

const char* Setting(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? "default" : value;
}

}

int main(int argc, char** argv) {
    try {
        const auto options = Parse(argc, argv);
        const auto capture = DispatchFreeze::Read(options.capture);
        const GuestWindows windows(capture.ranges);
        for (const auto& range : capture.ranges) std::memcpy(At(range.begin), range.before.data(), range.before.size());
        const auto request = ShaderRecompiler::RequestSerializer{}.Deserialize(capture.request);
        auto replayed = request.request;
        auto device = std::make_unique<VulkanDevice>();
        replayed.target = device->ComputeTarget(replayed.context.waveSize);
        replayed.useCache = false;
        const auto shader = ShaderRecompiler::Recompile(replayed);
        std::vector<Graphics::GuestMemorySnapshot> snapshots;
        for (const auto& region : replayed.context.memory) snapshots.push_back({region.guestAddress, region.bytes});
        std::vector<double> times;
        std::uint64_t mismatches = 0;
        std::uint64_t firstMismatch = 0;
        for (std::uint32_t repeat = 0; repeat < options.repeats; ++repeat) {
            for (const auto& range : capture.ranges) {
                if (range.writable) std::memcpy(At(range.begin), range.before.data(), range.before.size());
            }
            const auto started = std::chrono::steady_clock::now();
            if (capture.arguments != 0) static_cast<void>(device->DispatchIndirect(shader, capture.arguments, snapshots, capture.program));
            else device->Dispatch(shader, capture.groups[0], capture.groups[1], capture.groups[2], snapshots, capture.program);
            device->WaitIdle();
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            if (repeat != 0) continue;
            for (const auto& range : capture.ranges) {
                if (!range.writable) continue;
                for (std::size_t offset = 0; offset < range.after.size(); ++offset) {
                    if (At(range.begin)[offset] == range.after[offset]) continue;
                    if (mismatches++ == 0) firstMismatch = range.begin + offset;
                }
            }
        }
        std::sort(times.begin(), times.end());
        std::printf("{\"capture\":\"%s\",\"abi\":\"%s\",\"placement\":\"%s\",\"relative\":\"%s\",\"repeats\":%u,\"median_ms\":%.4f,\"min_ms\":%.4f,\"mismatched_bytes\":%llu,\"first_mismatch\":\"0x%llx\"}\n",
            options.capture.filename().string().c_str(), Setting("APS5_BDA_ABI"), Setting("APS5_BDA_PLACEMENT"), Setting("APS5_BDA_RELATIVE"), options.repeats, times[times.size() / 2u], times.front(),
            static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(firstMismatch));
        return mismatches != 0 && !options.allowMismatch ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}

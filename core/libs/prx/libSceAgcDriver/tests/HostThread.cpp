#include "prx/libSceAgcDriver/Execution/include/HostThread.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

constexpr std::size_t MiB = std::size_t{1} << 20u;
constexpr std::size_t StackReserve = 64 * MiB;
constexpr std::size_t FrameBytes = 48 * 1024;
constexpr unsigned Threads = 4;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t commit() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) == 0) throw std::runtime_error("read the process commit");
    return counters.PrivateUsage;
#else
    return 0;
#endif
}

struct Shared {
    std::atomic<bool> release{false};
    std::atomic<unsigned> started{0};
    std::atomic<unsigned> finished{0};
    std::atomic<std::uint64_t> checksum{0};
};

std::uint64_t descend(std::size_t depth) {
    volatile unsigned char frame[FrameBytes];
    std::memset(const_cast<unsigned char*>(frame), static_cast<int>(depth & 0xffu), sizeof(frame));
    const std::uint64_t below = depth == 0 ? 0 : descend(depth - 1);
    return below + frame[depth % sizeof(frame)];
}

void* worker(void* argument) {
    auto& shared = *static_cast<Shared*>(argument);
    shared.started.fetch_add(1);
    while (!shared.release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    shared.checksum.fetch_add(descend(32 * MiB / FrameBytes));
    shared.finished.fetch_add(1);
    return nullptr;
}

void waitFor(const std::atomic<unsigned>& counter, unsigned expected, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (counter.load() != expected) {
        require(std::chrono::steady_clock::now() < deadline, what);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}

int main() {
    try {
        static Shared shared;
        const auto before = commit();
        for (unsigned index = 0; index < Threads; ++index) require(AgcDriver::StartDetachedHostThread(&worker, &shared, StackReserve), "start a host thread");
        waitFor(shared.started, Threads, "the host threads did not start");
        const auto parked = commit();
#ifdef _WIN32
        require(parked < before + Threads * StackReserve / 4, "host thread stacks are committed up front: " + std::to_string((parked - before) / MiB) + " MiB for " + std::to_string(Threads) + " threads of " + std::to_string(StackReserve / MiB) + " MiB");
#endif
        shared.release.store(true);
        waitFor(shared.finished, Threads, "the host threads did not finish descending 32 MiB of stack");
        require(shared.checksum.load() != 0, "the descent did not run");
        std::printf("agc_host_thread_tests passed: %u threads reserving %zu MiB committed %.1f MiB before use\n", Threads, StackReserve / MiB, static_cast<double>(parked - before) / static_cast<double>(MiB));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_host_thread_tests: %s\n", error.what());
        return 1;
    }
    return 0;
}

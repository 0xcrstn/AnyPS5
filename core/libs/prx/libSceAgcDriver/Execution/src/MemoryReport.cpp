#include "prx/libSceAgcDriver/Execution/include/MemoryReport.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProcessMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestHeap.hpp"
#include "Recompiler.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace AgcDriver::MemoryReport {
namespace {

constexpr double MiB = 1048576.0;
constexpr double GiB = 1073741824.0;

std::string gib(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f GiB", static_cast<double>(bytes) / GiB);
    return text;
}

std::string mib(std::uint64_t bytes) {
    char text[32];
    const auto value = static_cast<double>(bytes) / MiB;
    std::snprintf(text, sizeof(text), value < 10.0 ? "%.1f MiB" : "%.0f MiB", value);
    return text;
}

}

std::string Describe(const std::function<std::string()>& device) {
    const auto started = std::chrono::steady_clock::now();
    std::uintptr_t arenaBase = 0;
    std::size_t arenaBytes = 0;
    if (GuestArena::GuestArenaAvailable_nid_postfix()) GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
    const auto process = ProcessMemory::ReadProcessCounters();
    static std::mutex stacksMutex;
    static std::vector<std::uintptr_t> stacks;
    static std::uint64_t reports = 0;
    std::lock_guard stacksLock(stacksMutex);
    if (reports++ % 10 == 0) stacks = ProcessMemory::ThreadStackAllocations();
    const auto space = ProcessMemory::WalkAddressSpace(arenaBase, arenaBytes, stacks);
    GuestHeap::GuestHeapStatistics guestHeap{};
    GuestHeap::GuestHeapGetStatistics_nid_no_patch(&guestHeap);
    const auto shaders = ShaderRecompiler::GetRetainedShaderMemory();
    const auto backlog = DriverDetail::RegistrationPreparationBacklog();
    const auto walked = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::string text = "[memory] process commit " + gib(process.commit) + " (peak " + gib(process.peakCommit) + "), working set " + gib(process.workingSet);
    text += " | guest arena private " + gib(process.commit > space.hostPrivate ? process.commit - space.hostPrivate : 0);
    text += "; guest heap live " + mib(guestHeap.largeLiveBytes) + " + small spans " + mib(guestHeap.smallSpanBytes) + ", freed blocks kept " + mib(guestHeap.largeCachedBytes);
    text += " | host private " + gib(space.hostPrivate) + ": " + std::to_string(space.threads) + " thread stacks " + mib(space.stacks) + ", heaps and other allocations " + gib(space.hostPrivate - space.stacks);
    if (!space.largest.empty()) {
        text += " (largest";
        for (const auto& [base, bytes] : space.largest) {
            char part[64];
            std::snprintf(part, sizeof(part), " 0x%llx=%s", static_cast<unsigned long long>(base), mib(bytes).c_str());
            text += part;
        }
        text += ")";
    }
    text += "; host mapped " + mib(space.hostMapped) + ", images " + mib(space.image);
    const auto vulkan = device ? device() : std::string{};
    if (!vulkan.empty()) text += " | vulkan " + vulkan;
    char part[1024];
    std::snprintf(part, sizeof(part), " | shaders registered %llu; preparation queue %llu (%llu of them used in earlier runs; %llu running on %llu threads), library prebuilds %llu; plans awaiting preparation %llu holding %llu decoded instructions (%s); recompiler sources %llu, resource plans %llu (%llu values, ~%s; %llu of them kept only in the disk cache, ~%s, %llu read back), retained IR programs %llu, variants %llu (%s SPIR-V in memory, %llu kept only in the disk cache with %s, %llu read back: %s), memoized results %llu (~%s), binding plans %llu, specialized modules %llu (%s SPIR-V), registration modules %llu",
        static_cast<unsigned long long>(backlog.registered), static_cast<unsigned long long>(backlog.queued), static_cast<unsigned long long>(backlog.queuedByUse), static_cast<unsigned long long>(backlog.running), static_cast<unsigned long long>(backlog.threads), static_cast<unsigned long long>(backlog.prebuildsQueued), static_cast<unsigned long long>(backlog.plans), static_cast<unsigned long long>(backlog.planInstructions), mib(backlog.planInstructionBytes).c_str(),
        static_cast<unsigned long long>(shaders.sources), static_cast<unsigned long long>(shaders.plans), static_cast<unsigned long long>(shaders.planValues), mib(shaders.planBytes).c_str(), static_cast<unsigned long long>(shaders.spilledPlans), mib(shaders.spilledPlanBytes).c_str(), static_cast<unsigned long long>(shaders.planReloads), static_cast<unsigned long long>(shaders.programs), static_cast<unsigned long long>(shaders.variants), mib(shaders.variantSpirvBytes).c_str(), static_cast<unsigned long long>(shaders.spilledVariants), mib(shaders.spilledSpirvBytes).c_str(), static_cast<unsigned long long>(shaders.spirvReloads), mib(shaders.spirvReloadBytes).c_str(), static_cast<unsigned long long>(shaders.memoResults), mib(shaders.memoBytes).c_str(), static_cast<unsigned long long>(shaders.bindingPlans), static_cast<unsigned long long>(shaders.specializedModules), mib(shaders.specializedSpirvBytes).c_str(), static_cast<unsigned long long>(shaders.registrationModules));
    text += part;
    std::snprintf(part, sizeof(part), " | took %.0f ms over %llu regions", walked, static_cast<unsigned long long>(space.regions));
    text += part;
    return text;
}

std::uint64_t IntervalSeconds() {
    static const std::uint64_t seconds = [] {
        const char* text = std::getenv("APS5_MEMORY_REPORT");
        if (text == nullptr || *text == '\0') return std::getenv("APS5_PROFILE_DRAW") != nullptr ? std::uint64_t{60} : std::uint64_t{0};
        char* end = nullptr;
        const auto value = std::strtoull(text, &end, 10);
        if (end == text || *end != '\0') throw std::runtime_error("APS5_MEMORY_REPORT is not a whole number of seconds: " + std::string(text));
        return static_cast<std::uint64_t>(value);
    }();
    return seconds;
}

namespace {

std::mutex reporterMutex;
std::jthread reporter;

}

void Start(std::function<std::string()> device) {
    const auto seconds = IntervalSeconds();
    if (seconds == 0) return;
    std::lock_guard lock(reporterMutex);
    if (reporter.joinable()) return;
    reporter = std::jthread([seconds, device = std::move(device)](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                if (wake.wait_for(lock, stop, std::chrono::seconds(seconds), [] { return false; }) || stop.stop_requested()) return;
            }
            try {
                const auto line = Describe(device);
                std::fprintf(stderr, "%s\n", line.c_str());
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[memory] report failed: %s\n", error.what());
            }
        }
    });
}

void Stop() {
    std::jthread stopping;
    {
        std::lock_guard lock(reporterMutex);
        stopping = std::move(reporter);
    }
    if (stopping.joinable()) {
        stopping.request_stop();
        stopping.join();
    }
}

}

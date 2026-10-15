#include "prx/libSceAgcDriver/Graphics/include/EmitRecording.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <stdexcept>
#include <utility>

namespace AgcDriver::Graphics {

struct EmitTicket {
    EmitUnit unit;
    std::shared_ptr<EmitPools> pools;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    std::string failure;
    std::vector<CommandList> canonical;
    std::vector<std::vector<std::size_t>> canonicalMarks;
    bool done = false;
    bool collected = false;
};

namespace {

struct EmitCounters {
    std::atomic<std::uint64_t> units{0};
    std::atomic<std::uint64_t> passes{0};
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> waits{0};
    std::atomic<std::uint64_t> waitNs{0};
    std::atomic<std::uint64_t> busyNs{0};
};

EmitCounters& Counters() {
    static EmitCounters counters;
    return counters;
}

bool Profiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profiled;
}

std::uint64_t Since(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

std::string FailedDraw(const EmitUnit& unit, std::size_t pass, std::size_t draw) {
    if (pass >= unit.passes.size()) return "emit unit";
    const auto& draws = unit.passes[pass].draws;
    if (draws.empty()) return "render pass " + std::to_string(pass) + " of an emit unit";
    return ChunkDrawName(draws[std::min(draw, draws.size() - 1)]);
}

}

bool EmitRecording::Verify() {
    static const bool verify = std::getenv("APS5_VERIFY_EMIT") != nullptr;
    return verify;
}

EmitStatistics EmitRecording::Statistics() {
    auto& counters = Counters();
    EmitStatistics statistics;
    statistics.units = counters.units.load(std::memory_order_relaxed);
    statistics.passes = counters.passes.load(std::memory_order_relaxed);
    statistics.draws = counters.draws.load(std::memory_order_relaxed);
    statistics.verified = counters.verified.load(std::memory_order_relaxed);
    statistics.failures = counters.failures.load(std::memory_order_relaxed);
    statistics.waits = counters.waits.load(std::memory_order_relaxed);
    statistics.waitNs = counters.waitNs.load(std::memory_order_relaxed);
    statistics.busyNs = counters.busyNs.load(std::memory_order_relaxed);
    return statistics;
}

EmitRecording::EmitRecording(const Context& context, std::uint32_t workers) : context(context) {
    Require(workers != 0, "emit recording needs at least one worker");
    this->context.bufferPool.reset();
    poolInfo = VkCommandPoolCreateInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = context.queueFamily;
    beginCommandBuffer = context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
    endCommandBuffer = context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
    threads.reserve(workers);
    try {
        for (std::uint32_t worker = 0; worker < workers; ++worker) threads.emplace_back([this, worker] { run(worker); });
    } catch (...) {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto& thread : threads) thread.join();
        throw;
    }
}

EmitRecording::~EmitRecording() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
}

std::shared_ptr<EmitPools> EmitRecording::TakePools() {
    {
        std::lock_guard lock(spareMutex);
        if (!sparePools.empty()) {
            auto pools = std::move(sparePools.back());
            sparePools.pop_back();
            return pools;
        }
    }
    return std::make_shared<EmitPools>(threads.size());
}

void EmitRecording::Recycle(std::shared_ptr<EmitPools> pools) noexcept {
    if (pools == nullptr) return;
    try {
        for (const auto& pool : pools->pools) {
            if (pool != nullptr) pool->Reset();
        }
        std::lock_guard lock(spareMutex);
        if (sparePools.size() < 16) sparePools.push_back(std::move(pools));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[emit] a batch's emit pools were dropped instead of reused: %s\n", error.what());
    }
}

std::shared_ptr<EmitTicket> EmitRecording::Post(EmitUnit&& unit, std::shared_ptr<EmitPools> pools) {
    Require(pools != nullptr && pools->pools.size() == threads.size(), "an emit unit is posted without its batch's pools");
    auto ticket = std::make_shared<EmitTicket>();
    ticket->unit = std::move(unit);
    ticket->pools = std::move(pools);
    auto& counters = Counters();
    counters.units.fetch_add(1, std::memory_order_relaxed);
    counters.passes.fetch_add(ticket->unit.passes.size(), std::memory_order_relaxed);
    std::size_t draws = 0;
    for (const auto& pass : ticket->unit.passes) draws += pass.draws.size();
    counters.draws.fetch_add(draws, std::memory_order_relaxed);
    {
        std::lock_guard lock(mutex);
        queue.push_back(ticket);
    }
    wake.notify_one();
    return ticket;
}

EmitRecording::Finished EmitRecording::Wait(EmitTicket& ticket) {
    Finished result;
    auto& counters = Counters();
    {
        std::unique_lock lock(mutex);
        if (!ticket.done) {
            const auto start = std::chrono::steady_clock::now();
            finished.wait(lock, [&] { return ticket.done; });
            counters.waits.fetch_add(1, std::memory_order_relaxed);
            counters.waitNs.fetch_add(Since(start), std::memory_order_relaxed);
        }
    }
    result.failure = ticket.failure;
    if (result.failure.empty() && Verify() && !ticket.collected) {
        for (std::size_t pass = 0; pass < ticket.unit.passes.size(); ++pass) {
            const auto& emitted = ticket.unit.passes[pass];
            try {
                CompareChunkCommands(emitted.commands.Words(), emitted.drawOffsets, ticket.canonical[pass].Words(), ticket.canonicalMarks[pass], emitted.draws);
                counters.verified.fetch_add(emitted.draws.size(), std::memory_order_relaxed);
            } catch (const std::logic_error& error) {
                result.failure = std::string("APS5_VERIFY_EMIT: ") + error.what();
                result.mismatch = true;
                ticket.failure = result.failure;
                break;
            }
        }
    }
    if (!result.failure.empty() && !ticket.collected) counters.failures.fetch_add(1, std::memory_order_relaxed);
    ticket.collected = true;
    if (result.failure.empty()) result.commands = ticket.commands;
    report();
    return result;
}

void EmitRecording::WaitQuietly(EmitTicket& ticket) noexcept {
    std::unique_lock lock(mutex);
    finished.wait(lock, [&] { return ticket.done; });
}

void EmitRecording::run(std::size_t worker) {
    CpuTopology::PinHelperThread("emit recorder");
    std::unique_lock lock(mutex);
    for (;;) {
        wake.wait(lock, [&] { return stopping || !queue.empty(); });
        if (queue.empty()) return;
        auto ticket = std::move(queue.front());
        queue.pop_front();
        lock.unlock();
        const auto start = std::chrono::steady_clock::now();
        record(worker, *ticket);
        Counters().busyNs.fetch_add(Since(start), std::memory_order_relaxed);
        lock.lock();
        ticket->done = true;
        finished.notify_all();
    }
}

void EmitRecording::record(std::size_t worker, EmitTicket& ticket) {
    const auto& unit = ticket.unit;
    const bool verify = Verify();
    std::size_t pass = 0;
    std::size_t draw = 0;
    try {
        auto& pool = ticket.pools->pools[worker];
        if (pool == nullptr) {
            VkCommandPool handle = VK_NULL_HANDLE;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &poolInfo, nullptr, &handle), "vkCreateCommandPool emit");
            try {
                pool = std::make_shared<ChunkPool>(context, handle, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
            } catch (...) {
                context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, handle, nullptr);
                throw;
            }
        }
        const auto commands = pool->Take();
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(beginCommandBuffer(commands, &begin), "vkBeginCommandBuffer emit");
        auto local = context;
        if (unit.functions != nullptr) local.functions = unit.functions;
        if (verify) {
            ticket.canonical.assign(unit.passes.size(), CommandList{});
            ticket.canonicalMarks.assign(unit.passes.size(), {});
        }
        for (pass = 0; pass < unit.passes.size(); ++pass) {
            const auto& emitted = unit.passes[pass];
            const auto words = emitted.commands.Words();
            draw = 0;
            RecordPassBegin(local, commands, emitted.begin, false);
            if (emitted.drawOffsets.empty()) {
                ReplayCommands(local, commands, words, verify ? &ticket.canonical[pass] : nullptr);
            } else {
                Require(emitted.drawOffsets.front() == 0, "an emit pass's first draw does not start its command list");
                for (draw = 0; draw < emitted.drawOffsets.size(); ++draw) {
                    const auto first = emitted.drawOffsets[draw];
                    const auto end = draw + 1 < emitted.drawOffsets.size() ? emitted.drawOffsets[draw + 1] : words.size();
                    Require(first <= end && end <= words.size(), "an emit pass's draw offsets run outside its command list");
                    if (verify) ticket.canonicalMarks[pass].push_back(ticket.canonical[pass].Words().size());
                    ReplayCommands(local, commands, words.subspan(first, end - first), verify ? &ticket.canonical[pass] : nullptr);
                }
            }
            if (emitted.begin.rendering) local.Resolved(&DeviceFunctions::cmdEndRendering, "vkCmdEndRenderingKHR")(commands);
            else local.Resolved(&DeviceFunctions::cmdEndRenderPass, "vkCmdEndRenderPass")(commands);
        }
        Check(endCommandBuffer(commands), "vkEndCommandBuffer emit");
        ticket.commands = commands;
    } catch (const std::exception& error) {
        ticket.failure = "emit recording failed at the " + FailedDraw(unit, pass, draw) + ": " + error.what();
    }
}

void EmitRecording::report() {
    if (!Profiled()) return;
    static std::mutex reportMutex;
    std::unique_lock lock(reportMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    static auto lastReport = std::chrono::steady_clock::now();
    static EmitStatistics last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    const auto seconds = std::chrono::duration<double>(now - lastReport).count();
    lastReport = now;
    const auto values = Statistics();
    const auto delta = [](std::uint64_t value, std::uint64_t before) { return static_cast<unsigned long long>(value - before); };
    const auto units = values.units - last.units;
    const auto waits = values.waits - last.waits;
    const auto waitedUs = static_cast<double>(values.waitNs - last.waitNs) / 1000.0;
    const auto busy = seconds > 0 ? static_cast<double>(values.busyNs - last.busyNs) / (seconds * 1e9 * static_cast<double>(threads.size())) * 100.0 : 0.0;
    AgcDriver::ProfilePrint_nid_no_patch("[emit] %.3f s: %llu units, %llu passes, %llu draws (%.1f per unit), %llu verified, %llu failed; submit waited for %llu units, %.1f us each (%.1f ms in all); %zu workers busy %.1f %%\n", seconds, static_cast<unsigned long long>(units), delta(values.passes, last.passes), delta(values.draws, last.draws), units != 0 ? static_cast<double>(values.draws - last.draws) / static_cast<double>(units) : 0.0, delta(values.verified, last.verified), delta(values.failures, last.failures), static_cast<unsigned long long>(waits), waits != 0 ? waitedUs / static_cast<double>(waits) : 0.0, waitedUs / 1000.0, threads.size(), busy);
    last = values;
}

}

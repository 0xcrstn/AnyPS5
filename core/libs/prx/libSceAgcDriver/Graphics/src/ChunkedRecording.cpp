#include "prx/libSceAgcDriver/Graphics/include/ChunkedRecording.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>

namespace AgcDriver::Graphics {

namespace {

struct ChunkCounters {
    std::atomic<std::uint64_t> chunks{0};
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> discarded{0};
    std::atomic<std::uint64_t> drains{0};
    std::atomic<std::uint64_t> waitNs{0};
    std::atomic<std::uint64_t> busyNs{0};
    std::array<std::atomic<std::uint64_t>, ChunkInlineReasonCount> inlinePasses{};
};

ChunkCounters& Counters() {
    static ChunkCounters counters;
    return counters;
}

constexpr std::array<const char*, ChunkInlineReasonCount> InlineReasonNames{"gpu timing", "occlusion samples"};

std::atomic<ChunkedRecording*> activeRecording{nullptr};

bool Profiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profiled;
}

std::uint64_t Since(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

}

struct ChunkedRecording::Chunk {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    ChunkInheritance inheritance;
    std::uintptr_t identity = 0;
    CommandList staging;
    ChunkDraw stagingDraw;
    std::vector<std::uint64_t> issued;
    std::vector<std::size_t> issuedMarks;
    std::vector<ChunkDraw> issuedDraws;
    std::vector<std::uint64_t> pending;
    std::vector<std::pair<std::size_t, ChunkDraw>> pendingMarks;
    std::uint64_t posted = 0;
    std::uint64_t recorded = 0;
    bool closing = false;
    bool finished = false;
    std::string failure;
    bool begun = false;
    std::string workerFailure;
    std::vector<std::uint64_t> work;
    std::vector<std::pair<std::size_t, ChunkDraw>> workMarks;
    CommandList canonical;
    std::vector<std::size_t> canonicalMarks;
};

ChunkPool::ChunkPool(const Context& context, VkCommandPool pool) : context(context), pool(pool) {
    this->context.bufferPool.reset();
}

ChunkPool::~ChunkPool() {
    if (pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, pool, nullptr);
}

VkCommandBuffer ChunkPool::Take() {
    if (used == buffers.size()) {
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocation, &commands), "vkAllocateCommandBuffers chunk");
        buffers.push_back(commands);
    }
    return buffers[used++];
}

void ChunkPool::Reset() {
    Check(context.Function<PFN_vkResetCommandPool>("vkResetCommandPool")(context.device, pool, 0), "vkResetCommandPool chunk");
    used = 0;
}

ChunkedRecording::ChunkedRecording(const Context& context) : context(context) {
    this->context.bufferPool.reset();
    poolInfo = VkCommandPoolCreateInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = context.queueFamily;
    beginCommandBuffer = context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
    endCommandBuffer = context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
    worker = std::thread([this] { run(); });
}

ChunkedRecording::~ChunkedRecording() {
    ChunkedRecording* self = this;
    activeRecording.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
    Discard();
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    if (worker.joinable()) worker.join();
}

bool ChunkedRecording::Enabled() {
    static const bool enabled = [] {
        if (std::getenv("APS5_CHUNKED_RECORDING") == nullptr) return false;
        if (std::getenv("APS5_RECORD_THREAD") == nullptr || std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr) {
            std::fprintf(stderr, "[chunked-recording] APS5_CHUNKED_RECORDING ignored: it needs APS5_RECORD_THREAD (without APS5_LOCKED_DRAW_PREPARE)\n");
            return false;
        }
        std::fprintf(stderr, "[chunked-recording] on: the draws of a render pass are recorded into secondary command buffers on a recording worker%s\n", Verify() ? ", every chunk compared with the commands encode issued" : "");
        return true;
    }();
    return enabled;
}

bool ChunkedRecording::Verify() {
    static const bool verify = std::getenv("APS5_VERIFY_CHUNKED_RECORDING") != nullptr;
    return verify;
}

void ChunkedRecording::CountInline(ChunkInlineReason reason) {
    Counters().inlinePasses[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

const char* ChunkedRecording::InlineReasonName(ChunkInlineReason reason) {
    const auto index = static_cast<std::size_t>(reason);
    return index < InlineReasonNames.size() ? InlineReasonNames[index] : "unknown";
}

ChunkStatistics ChunkedRecording::Statistics() {
    auto& counters = Counters();
    ChunkStatistics statistics;
    statistics.chunks = counters.chunks.load(std::memory_order_relaxed);
    statistics.draws = counters.draws.load(std::memory_order_relaxed);
    statistics.verified = counters.verified.load(std::memory_order_relaxed);
    statistics.failures = counters.failures.load(std::memory_order_relaxed);
    statistics.discarded = counters.discarded.load(std::memory_order_relaxed);
    statistics.drains = counters.drains.load(std::memory_order_relaxed);
    statistics.waitNs = counters.waitNs.load(std::memory_order_relaxed);
    statistics.busyNs = counters.busyNs.load(std::memory_order_relaxed);
    for (std::size_t reason = 0; reason < ChunkInlineReasonCount; ++reason) statistics.inlinePasses[reason] = counters.inlinePasses[reason].load(std::memory_order_relaxed);
    return statistics;
}

void ChunkedRecording::Activate() {
    activeRecording.store(this, std::memory_order_release);
}

void ChunkedRecording::DrainActive() {
    if (auto* recording = activeRecording.load(std::memory_order_acquire); recording != nullptr) recording->Drain();
}

std::shared_ptr<ChunkPool> ChunkedRecording::takePool() {
    if (!sparePools.empty()) {
        auto pool = std::move(sparePools.back());
        sparePools.pop_back();
        return pool;
    }
    VkCommandPool handle = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &poolInfo, nullptr, &handle), "vkCreateCommandPool chunk");
    try {
        return std::make_shared<ChunkPool>(context, handle);
    } catch (...) {
        context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, handle, nullptr);
        throw;
    }
}

void ChunkedRecording::Recycle(std::shared_ptr<ChunkPool> pool) noexcept {
    if (pool == nullptr) return;
    try {
        pool->Reset();
        if (sparePools.size() < 16) sparePools.push_back(std::move(pool));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[chunked-recording] a chunk pool was dropped instead of reused: %s\n", error.what());
    }
}

CommandSink ChunkedRecording::Begin(const Context& drawContext, std::shared_ptr<ChunkPool>& pool, const ChunkInheritance& inheritance, const ChunkDraw& draw) {
    Discard();
    if (pool == nullptr) pool = takePool();
    auto chunk = std::make_shared<Chunk>();
    chunk->commands = pool->Take();
    chunk->inheritance = inheritance;
    chunk->identity = static_cast<std::uintptr_t>((++serial << 1u) | 1u);
    chunk->stagingDraw = draw;
    {
        std::lock_guard lock(mutex);
        if (drawContext.functions != nullptr) context.functions = drawContext.functions;
        current = chunk;
    }
    open = chunk;
    Counters().chunks.fetch_add(1, std::memory_order_relaxed);
    return CommandSink(chunk->staging, chunk->identity);
}

CommandSink ChunkedRecording::Next(const ChunkDraw& draw) {
    Require(open != nullptr, "a chunked render pass has no open chunk");
    Post();
    open->stagingDraw = draw;
    return CommandSink(open->staging, open->identity);
}

void ChunkedRecording::Post() {
    if (open == nullptr || open->staging.Empty()) return;
    const auto words = open->staging.Words();
    {
        std::lock_guard lock(mutex);
        open->pendingMarks.emplace_back(open->pending.size(), open->stagingDraw);
        open->pending.insert(open->pending.end(), words.begin(), words.end());
        ++open->posted;
    }
    wake.notify_all();
    if (Verify()) {
        open->issuedMarks.push_back(open->issued.size());
        open->issued.insert(open->issued.end(), words.begin(), words.end());
        open->issuedDraws.push_back(open->stagingDraw);
    }
    open->staging.Clear();
    Counters().draws.fetch_add(1, std::memory_order_relaxed);
}

ChunkedRecording::Closed ChunkedRecording::Close() {
    Closed closed;
    if (open == nullptr) return closed;
    Post();
    const auto chunk = std::move(open);
    open = nullptr;
    const auto start = std::chrono::steady_clock::now();
    {
        std::unique_lock lock(mutex);
        chunk->closing = true;
        wake.notify_all();
        finished.wait(lock, [&] { return chunk->finished; });
        if (current == chunk) current = nullptr;
    }
    auto& counters = Counters();
    counters.waitNs.fetch_add(Since(start), std::memory_order_relaxed);
    closed.failure = chunk->failure;
    if (closed.failure.empty() && Verify()) {
        try {
            CompareChunkCommands(chunk->issued, chunk->issuedMarks, chunk->canonical.Words(), chunk->canonicalMarks, chunk->issuedDraws);
            counters.verified.fetch_add(chunk->issuedDraws.size(), std::memory_order_relaxed);
        } catch (const std::logic_error& error) {
            closed.failure = error.what();
            closed.mismatch = true;
        }
    }
    if (!closed.failure.empty()) counters.failures.fetch_add(1, std::memory_order_relaxed);
    else if (chunk->begun) closed.commands = chunk->commands;
    report();
    return closed;
}

void ChunkedRecording::Discard() noexcept {
    if (open == nullptr) return;
    try {
        open->staging.Clear();
        static_cast<void>(Close());
        Counters().discarded.fetch_add(1, std::memory_order_relaxed);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[chunked-recording] discarding a chunk failed: %s\n", error.what());
    }
}

void ChunkedRecording::Drain() {
    std::unique_lock lock(mutex);
    const auto chunk = current;
    if (chunk == nullptr) return;
    const auto target = chunk->posted;
    if (chunk->recorded >= target || chunk->finished) return;
    Counters().drains.fetch_add(1, std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    finished.wait(lock, [&] { return chunk->finished || chunk->recorded >= target; });
    Counters().waitNs.fetch_add(Since(start), std::memory_order_relaxed);
}

void ChunkedRecording::run() {
    CpuTopology::PinHelperThread("chunk recorder");
    std::unique_lock lock(mutex);
    for (;;) {
        wake.wait(lock, [&] { return stopping || (current != nullptr && !current->finished && (!current->pendingMarks.empty() || current->closing)); });
        if (current == nullptr || current->finished) {
            if (stopping) return;
            continue;
        }
        const auto chunk = current;
        chunk->work.swap(chunk->pending);
        chunk->workMarks.swap(chunk->pendingMarks);
        const bool closing = chunk->closing || stopping;
        lock.unlock();
        const auto start = std::chrono::steady_clock::now();
        recordWork(*chunk);
        if (closing) finishWork(*chunk);
        Counters().busyNs.fetch_add(Since(start), std::memory_order_relaxed);
        lock.lock();
        chunk->recorded += chunk->workMarks.size();
        chunk->work.clear();
        chunk->workMarks.clear();
        if (closing) {
            chunk->failure = chunk->workerFailure;
            chunk->finished = true;
        }
        finished.notify_all();
    }
}

void ChunkedRecording::recordWork(Chunk& chunk) {
    if (!chunk.workerFailure.empty() || chunk.workMarks.empty()) return;
    const bool verify = Verify();
    std::size_t index = 0;
    try {
        if (!chunk.begun) {
            const auto& inherited = chunk.inheritance;
            VkCommandBufferInheritanceRenderingInfoKHR rendering{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO_KHR};
            VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
            if (inherited.rendering) {
                rendering.colorAttachmentCount = static_cast<std::uint32_t>(inherited.colorFormats.size());
                rendering.pColorAttachmentFormats = inherited.colorFormats.empty() ? nullptr : inherited.colorFormats.data();
                rendering.depthAttachmentFormat = inherited.depthFormat;
                rendering.stencilAttachmentFormat = inherited.stencilFormat;
                rendering.rasterizationSamples = inherited.samples;
                inheritance.pNext = &rendering;
            } else {
                inheritance.renderPass = inherited.renderPass;
                inheritance.subpass = 0;
                inheritance.framebuffer = inherited.framebuffer;
            }
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (inherited.rendering || inherited.renderPass != VK_NULL_HANDLE) begin.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            begin.pInheritanceInfo = &inheritance;
            Check(beginCommandBuffer(chunk.commands, &begin), "vkBeginCommandBuffer chunk");
            chunk.begun = true;
        }
        for (; index < chunk.workMarks.size(); ++index) {
            const auto first = chunk.workMarks[index].first;
            const auto end = index + 1 < chunk.workMarks.size() ? chunk.workMarks[index + 1].first : chunk.work.size();
            if (verify) chunk.canonicalMarks.push_back(chunk.canonical.Words().size());
            ReplayCommands(context, chunk.commands, std::span<const std::uint64_t>(chunk.work).subspan(first, end - first), verify ? &chunk.canonical : nullptr);
        }
    } catch (const std::exception& error) {
        const auto& draw = chunk.workMarks[std::min(index, chunk.workMarks.size() - 1)].second;
        chunk.workerFailure = "chunked recording failed at the " + ChunkDrawName(draw) + ": " + error.what();
    }
}

void ChunkedRecording::finishWork(Chunk& chunk) {
    if (!chunk.begun || !chunk.workerFailure.empty()) return;
    try {
        Check(endCommandBuffer(chunk.commands), "vkEndCommandBuffer chunk");
    } catch (const std::exception& error) {
        chunk.workerFailure = std::string("chunked recording failed to end its secondary command buffer: ") + error.what();
    }
}

void ChunkedRecording::report() {
    if (!Profiled()) return;
    static std::mutex reportMutex;
    std::unique_lock lock(reportMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    static auto lastReport = std::chrono::steady_clock::now();
    static ChunkStatistics last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    const auto seconds = std::chrono::duration<double>(now - lastReport).count();
    lastReport = now;
    const auto values = Statistics();
    const auto delta = [](std::uint64_t value, std::uint64_t before) { return static_cast<unsigned long long>(value - before); };
    std::string inlineText;
    for (std::size_t reason = 0; reason < ChunkInlineReasonCount; ++reason) inlineText += std::string(reason == 0 ? " " : ", ") + InlineReasonNames[reason] + " " + std::to_string(values.inlinePasses[reason] - last.inlinePasses[reason]);
    const auto chunks = values.chunks - last.chunks;
    const auto draws = values.draws - last.draws;
    const auto waitedUs = static_cast<double>(values.waitNs - last.waitNs) / 1000.0;
    const auto waits = chunks + (values.drains - last.drains);
    const auto busy = seconds > 0 ? static_cast<double>(values.busyNs - last.busyNs) / (seconds * 1e9) * 100.0 : 0.0;
    AgcDriver::ProfilePrint_nid_no_patch("[chunked-recording] %.3f s: %llu chunks, %llu draws (%.1f per chunk), %llu verified, %llu failed, %llu discarded; render passes recorded inline by reason:%s; encode waited %.1f us per chunk close or drain (%llu drains, %.1f ms in all); recording worker busy %.1f %%\n", seconds, static_cast<unsigned long long>(chunks), static_cast<unsigned long long>(draws), chunks != 0 ? static_cast<double>(draws) / static_cast<double>(chunks) : 0.0, delta(values.verified, last.verified), delta(values.failures, last.failures), delta(values.discarded, last.discarded), inlineText.c_str(), waits != 0 ? waitedUs / static_cast<double>(waits) : 0.0, delta(values.drains, last.drains), waitedUs / 1000.0, busy);
    last = values;
}

}

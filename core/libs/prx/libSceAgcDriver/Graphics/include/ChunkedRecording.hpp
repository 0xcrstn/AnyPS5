#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CHUNKEDRECORDING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CHUNKEDRECORDING_HPP

#include "prx/libSceAgcDriver/Graphics/include/CommandSink.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

enum class ChunkInlineReason : std::uint8_t { GpuTiming, OcclusionSamples, Count };
constexpr std::size_t ChunkInlineReasonCount = static_cast<std::size_t>(ChunkInlineReason::Count);

struct ChunkStatistics {
    std::uint64_t chunks = 0;
    std::uint64_t draws = 0;
    std::uint64_t verified = 0;
    std::uint64_t failures = 0;
    std::uint64_t discarded = 0;
    std::uint64_t drains = 0;
    std::uint64_t waitNs = 0;
    std::uint64_t busyNs = 0;
    std::array<std::uint64_t, ChunkInlineReasonCount> inlinePasses{};
};

class ChunkPool {
public:
    ChunkPool(const Context& context, VkCommandPool pool);
    ~ChunkPool();
    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
    VkCommandBuffer Take();
    void Reset();

private:
    Context context;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> buffers;
    std::size_t used = 0;
};

class ChunkedRecording {
public:
    explicit ChunkedRecording(const Context& context);
    ~ChunkedRecording();
    ChunkedRecording(const ChunkedRecording&) = delete;
    ChunkedRecording& operator=(const ChunkedRecording&) = delete;
    static bool Enabled();
    static bool Verify();
    static void CountInline(ChunkInlineReason reason);
    static const char* InlineReasonName(ChunkInlineReason reason);
    static ChunkStatistics Statistics();
    static void DrainActive();
    void Activate();
    bool IsOpen() const { return open != nullptr; }
    CommandSink Begin(const Context& drawContext, std::shared_ptr<ChunkPool>& pool, const ChunkInheritance& inheritance, const ChunkDraw& draw);
    CommandSink Next(const ChunkDraw& draw);
    void Post();
    struct Closed {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        std::string failure;
        bool mismatch = false;
    };
    Closed Close();
    void Discard() noexcept;
    void Drain();
    void Recycle(std::shared_ptr<ChunkPool> pool) noexcept;

private:
    struct Chunk;
    void run();
    void recordWork(Chunk& chunk);
    void finishWork(Chunk& chunk);
    std::shared_ptr<ChunkPool> takePool();
    void report();
    Context context;
    VkCommandPoolCreateInfo poolInfo{};
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable finished;
    std::shared_ptr<Chunk> current;
    std::shared_ptr<Chunk> open;
    bool stopping = false;
    std::uint64_t serial = 0;
    std::vector<std::shared_ptr<ChunkPool>> sparePools;
    std::thread worker;
};

}

#endif

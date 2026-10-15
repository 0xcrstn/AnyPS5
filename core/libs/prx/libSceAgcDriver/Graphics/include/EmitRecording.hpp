#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EMITRECORDING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EMITRECORDING_HPP

#include "prx/libSceAgcDriver/Graphics/include/ChunkedRecording.hpp"
#include "prx/libSceAgcDriver/Graphics/include/EmitUnit.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AgcDriver::Graphics {

struct EmitStatistics {
    std::uint64_t units = 0;
    std::uint64_t passes = 0;
    std::uint64_t draws = 0;
    std::uint64_t verified = 0;
    std::uint64_t failures = 0;
    std::uint64_t waits = 0;
    std::uint64_t waitNs = 0;
    std::uint64_t busyNs = 0;
};

class EmitPools {
public:
    explicit EmitPools(std::size_t workers) : pools(workers) {}
    std::vector<std::shared_ptr<ChunkPool>> pools;
};

struct EmitTicket;

class EmitRecording {
public:
    EmitRecording(const Context& context, std::uint32_t workers);
    ~EmitRecording();
    EmitRecording(const EmitRecording&) = delete;
    EmitRecording& operator=(const EmitRecording&) = delete;
    static bool Verify();
    static EmitStatistics Statistics();
    std::shared_ptr<EmitPools> TakePools();
    void Recycle(std::shared_ptr<EmitPools> pools) noexcept;
    std::shared_ptr<EmitTicket> Post(EmitUnit&& unit, std::shared_ptr<EmitPools> pools);
    struct Finished {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        std::string failure;
        bool mismatch = false;
    };
    Finished Wait(EmitTicket& ticket);
    void WaitQuietly(EmitTicket& ticket) noexcept;

private:
    void run(std::size_t worker);
    void record(std::size_t worker, EmitTicket& ticket);
    void report();
    Context context;
    VkCommandPoolCreateInfo poolInfo{};
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable finished;
    std::deque<std::shared_ptr<EmitTicket>> queue;
    bool stopping = false;
    std::mutex spareMutex;
    std::vector<std::shared_ptr<EmitPools>> sparePools;
    std::vector<std::thread> threads;
};

}

#endif

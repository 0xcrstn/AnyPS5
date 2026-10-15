#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WAITBROKER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WAITBROKER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace AgcDriver {

class WaitBroker {
public:
    static WaitBroker& Instance();
    static bool Enabled();
    static std::chrono::microseconds LongestSleep();

    std::uint64_t Sequence() const { return sequence.load(std::memory_order_seq_cst); }

    void Publish() {
        sequence.fetch_add(1, std::memory_order_seq_cst);
        if (waiters.load(std::memory_order_seq_cst) == 0) return;
        { std::lock_guard lock(mutex); }
        changed.notify_all();
    }

    bool WaitFor(std::uint64_t seen, std::chrono::microseconds timeout) {
        waiters.fetch_add(1, std::memory_order_seq_cst);
        bool moved = false;
        {
            std::unique_lock lock(mutex);
            moved = changed.wait_for(lock, timeout, [&] { return sequence.load(std::memory_order_seq_cst) != seen; });
        }
        waiters.fetch_sub(1, std::memory_order_seq_cst);
        return moved;
    }

private:
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint32_t> waiters{0};
    std::mutex mutex;
    std::condition_variable changed;
};

}

#endif

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_RECORDQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_RECORDQUEUE_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace AgcDriver {

class RecordQueue {
public:
    using Job = std::function<void()>;
    RecordQueue() = default;
    ~RecordQueue();
    RecordQueue(const RecordQueue&) = delete;
    RecordQueue& operator=(const RecordQueue&) = delete;

    void Start(std::function<void()> serve);
    void Stop();
    bool Started() const { return worker.joinable(); }
    std::uint64_t Post(Job job);
    std::uint64_t Posted() const { return posted.load(std::memory_order_acquire); }
    std::uint64_t Finished() const { return finished.load(std::memory_order_acquire); }
    bool Pending() const { return Finished() != Posted(); }
    void RunPending();
    static bool Running();
    void WaitFinished(std::uint64_t ticket);
    void WaitIdle() { WaitFinished(Posted()); }
    std::exception_ptr TakeFailure();

    struct Statistics {
        std::uint64_t posted = 0;
        std::uint64_t finished = 0;
        std::uint64_t byWorker = 0;
        std::uint64_t byOthers = 0;
        std::uint64_t failed = 0;
        std::uint64_t waits = 0;
        std::uint64_t blockedWaits = 0;
        std::uint64_t waitedNs = 0;
        std::uint64_t jobNs = 0;
    };
    Statistics Counts() const;

private:
    void serveLoop(std::function<void()> serve);

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    std::exception_ptr failure;
    std::atomic<std::uint64_t> posted{0};
    std::atomic<std::uint64_t> finished{0};
    std::atomic<std::uint64_t> byWorker{0};
    std::atomic<std::uint64_t> byOthers{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> waits{0};
    std::atomic<std::uint64_t> blockedWaits{0};
    std::atomic<std::uint64_t> waitedNs{0};
    std::atomic<std::uint64_t> jobNs{0};
    bool stopping = false;
    std::thread worker;
};

}

#endif

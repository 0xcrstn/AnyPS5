#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINECOMPILEQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINECOMPILEQUEUE_HPP

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver {

using PipelineCompileKey = std::pair<std::uint64_t, std::uint32_t>;

class PipelineCompileQueue {
public:
    struct Statistics {
        std::uint64_t accepted = 0, built = 0, present = 0, failed = 0;
        std::uint64_t refused = 0, duplicate = 0, demanded = 0, cancelled = 0;
        std::size_t retainedBytes = 0, peakBytes = 0, outstanding = 0;
    };

    explicit PipelineCompileQueue(std::size_t threads, std::size_t maxTasks = 32, std::size_t maxBytes = 64u * 1024u * 1024u)
        : maxTasks(maxTasks), maxBytes(maxBytes) {
        if (threads == 0 || threads > 4 || maxTasks == 0 || maxBytes == 0) throw std::invalid_argument("invalid pipeline compile queue limits");
        workers.reserve(threads);
        try {
            for (std::size_t i = 0; i < threads; ++i) workers.emplace_back([this] { run(); });
        } catch (...) {
            Cancel();
            Wait();
            throw;
        }
    }

    ~PipelineCompileQueue() {
        Cancel();
        Wait();
    }

    PipelineCompileQueue(const PipelineCompileQueue&) = delete;
    PipelineCompileQueue& operator=(const PipelineCompileQueue&) = delete;

    bool Contains(const PipelineCompileKey& key) const {
        std::lock_guard lock(mutex);
        return pending.contains(key);
    }

    bool TryEnqueue(PipelineCompileKey key, std::size_t bytes, std::function<bool()> work) {
        return TryEnqueuePrepared(key, bytes, [&] { return std::move(work); });
    }

    template<typename TPrepare>
    bool TryEnqueuePrepared(PipelineCompileKey key, std::size_t bytes, TPrepare&& prepare) {
        Task task{key, bytes, {}};
        std::lock_guard lock(mutex);
        if (pending.contains(key)) {
            ++stats.duplicate;
            return false;
        }
        if (closed || pending.size() >= maxTasks || bytes > maxBytes - stats.retainedBytes) {
            ++stats.refused;
            return false;
        }
        task.work = prepare();
        pending.insert(key);
        try {
            queued.emplace_back();
            queued.back() = std::move(task);
        } catch (...) {
            pending.erase(key);
            throw;
        }
        ++stats.accepted;
        stats.retainedBytes += bytes;
        stats.peakBytes = std::max(stats.peakBytes, stats.retainedBytes);
        stats.outstanding = pending.size();
        changed.notify_one();
        return true;
    }

    bool Demand(const PipelineCompileKey& key) {
        std::function<bool()> abandoned;
        {
            std::lock_guard lock(mutex);
            for (auto it = queued.begin(); it != queued.end(); ++it) {
                if (it->key != key) continue;
                abandoned = std::move(it->work);
                stats.retainedBytes -= it->bytes;
                queued.erase(it);
                pending.erase(key);
                ++stats.demanded;
                stats.outstanding = pending.size();
                settled.notify_all();
                return true;
            }
        }
        return false;
    }

    void Cancel() {
        std::deque<Task> abandoned;
        {
            std::lock_guard lock(mutex);
            closed = true;
            abandoned.swap(queued);
            stats.cancelled += abandoned.size();
            for (const auto& task : abandoned) {
                pending.erase(task.key);
                stats.retainedBytes -= task.bytes;
            }
            stats.outstanding = pending.size();
            changed.notify_all();
            settled.notify_all();
        }
    }

    void Drain() {
        std::unique_lock lock(mutex);
        settled.wait(lock, [&] { return pending.empty(); });
    }

    void Wait() {
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }

    Statistics Counters() const {
        std::lock_guard lock(mutex);
        return stats;
    }

private:
    struct Task {
        PipelineCompileKey key;
        std::size_t bytes;
        std::function<bool()> work;
    };

    void run() noexcept {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return closed || !queued.empty(); });
                if (queued.empty()) return;
                task = std::move(queued.front());
                queued.pop_front();
            }
            bool built = false, failed = false;
            try { built = task.work(); } catch (...) { failed = true; }
            task.work = {};
            {
                std::lock_guard lock(mutex);
                if (failed) ++stats.failed;
                else if (built) ++stats.built;
                else ++stats.present;
                pending.erase(task.key);
                stats.retainedBytes -= task.bytes;
                stats.outstanding = pending.size();
                settled.notify_all();
            }
        }
    }

    const std::size_t maxTasks, maxBytes;
    mutable std::mutex mutex;
    std::condition_variable changed, settled;
    std::deque<Task> queued;
    std::set<PipelineCompileKey> pending;
    std::vector<std::thread> workers;
    Statistics stats;
    bool closed = false;
};

}

#endif

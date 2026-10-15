#include "prx/libSceAgcDriver/Execution/include/RecordQueue.hpp"
#include <chrono>
#include <utility>

namespace AgcDriver {

namespace {

thread_local bool runningJobs = false;
thread_local bool workerThread = false;

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

}

RecordQueue::~RecordQueue() {
    Stop();
}

void RecordQueue::Start(std::function<void()> serve) {
    std::lock_guard lock(mutex);
    if (worker.joinable()) return;
    stopping = false;
    worker = std::thread([this, serve = std::move(serve)]() mutable { serveLoop(std::move(serve)); });
}

void RecordQueue::Stop() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_all();
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
}

std::uint64_t RecordQueue::Post(Job job) {
    std::uint64_t ticket = 0;
    {
        std::lock_guard lock(mutex);
        jobs.push_back(std::move(job));
        ticket = posted.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    changed.notify_all();
    return ticket;
}

bool RecordQueue::Running() {
    return runningJobs;
}

bool RecordQueue::OnWorker() {
    return workerThread;
}

void RecordQueue::RunPending() {
    if (runningJobs) return;
    runningJobs = true;
    struct Reset {
        ~Reset() { runningJobs = false; }
    } reset;
    for (;;) {
        Job job;
        {
            std::lock_guard lock(mutex);
            if (jobs.empty()) return;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        const auto start = std::chrono::steady_clock::now();
        try {
            job();
        } catch (...) {
            failed.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(mutex);
            if (failure == nullptr) failure = std::current_exception();
        }
        job = nullptr;
        jobNs.fetch_add(elapsedNs(start), std::memory_order_relaxed);
        (workerThread ? byWorker : byOthers).fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex);
            finished.fetch_add(1, std::memory_order_acq_rel);
        }
        changed.notify_all();
    }
}

void RecordQueue::WaitFinished(std::uint64_t ticket) {
    waits.fetch_add(1, std::memory_order_relaxed);
    if (Finished() >= ticket) return;
    const auto start = std::chrono::steady_clock::now();
    blockedWaits.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return finished.load(std::memory_order_acquire) >= ticket; });
    lock.unlock();
    waitedNs.fetch_add(elapsedNs(start), std::memory_order_relaxed);
}

std::exception_ptr RecordQueue::TakeFailure() {
    std::lock_guard lock(mutex);
    return std::exchange(failure, nullptr);
}

RecordQueue::Statistics RecordQueue::Counts() const {
    Statistics counts;
    counts.posted = posted.load(std::memory_order_acquire);
    counts.finished = finished.load(std::memory_order_acquire);
    counts.byWorker = byWorker.load(std::memory_order_relaxed);
    counts.byOthers = byOthers.load(std::memory_order_relaxed);
    counts.failed = failed.load(std::memory_order_relaxed);
    counts.waits = waits.load(std::memory_order_relaxed);
    counts.blockedWaits = blockedWaits.load(std::memory_order_relaxed);
    counts.waitedNs = waitedNs.load(std::memory_order_relaxed);
    counts.jobNs = jobNs.load(std::memory_order_relaxed);
    return counts;
}

void RecordQueue::serveLoop(std::function<void()> serve) {
    workerThread = true;
    for (;;) {
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return stopping || !jobs.empty(); });
            if (stopping) return;
        }
        serve();
    }
}

}

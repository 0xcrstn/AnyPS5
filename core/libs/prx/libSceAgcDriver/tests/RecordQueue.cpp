#include "prx/libSceAgcDriver/Execution/include/RecordQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using AgcDriver::RecordQueue;
using namespace AgcDriver::DriverDetail;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

class Exclusion {
public:
    explicit Exclusion(RecordQueue& queue) : queue(queue) {}
    void lock() {
        mutex.lock();
        if (++depth != 1) return;
        lock();
        queue.RunPending();
        unlock();
    }
    void unlock() {
        --depth;
        mutex.unlock();
    }

private:
    RecordQueue& queue;
    std::recursive_mutex mutex;
    int depth = 0;
};

void testJobsRunBeforeTheNextHolder() {
    RecordQueue queue;
    Exclusion exclusion(queue);
    std::vector<int> order;
    queue.Post([&] { order.push_back(1); });
    queue.Post([&] { order.push_back(2); });
    Expect(queue.Pending(), "posted jobs are pending until a holder runs them");
    {
        std::lock_guard lock(exclusion);
        Expect(queue.Finished() == 2, "a holder runs every earlier job at its acquisition");
        order.push_back(3);
    }
    Expect(order == std::vector<int>({1, 2, 3}), "the holder's own work follows the jobs");
    Expect(!queue.Pending(), "nothing is pending after the holder");
}

void testOrderAcrossRunners() {
    RecordQueue queue;
    Exclusion exclusion(queue);
    queue.Start([&] { std::lock_guard lock(exclusion); });
    std::vector<int> order;
    std::atomic<bool> stop{false};
    std::vector<std::thread> holders;
    for (int i = 0; i < 3; ++i) {
        holders.emplace_back([&] {
            while (!stop.load()) {
                std::lock_guard lock(exclusion);
            }
        });
    }
    constexpr int Jobs = 2000;
    for (int i = 0; i < Jobs; ++i) queue.Post([&order, i] { order.push_back(i); });
    queue.WaitIdle();
    stop = true;
    for (auto& holder : holders) holder.join();
    queue.Stop();
    bool ordered = order.size() == Jobs;
    for (int i = 0; ordered && i < Jobs; ++i) ordered = order[static_cast<std::size_t>(i)] == i;
    Expect(ordered, "jobs run once each, in posting order, whichever holder runs them");
    const auto counts = queue.Counts();
    Expect(counts.byWorker + counts.byOthers == Jobs && counts.finished == Jobs, "every job is counted once, by its runner");
}

void testWaitFinished() {
    RecordQueue queue;
    Exclusion exclusion(queue);
    queue.Start([&] { std::lock_guard lock(exclusion); });
    std::atomic<int> done{0};
    std::uint64_t third = 0;
    for (int i = 0; i < 6; ++i) {
        const auto ticket = queue.Post([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++done;
        });
        if (i == 2) third = ticket;
    }
    queue.WaitFinished(third);
    Expect(done.load() >= 3 && queue.Finished() >= third, "a wait for a ticket returns once that job and every earlier one finished");
    queue.WaitIdle();
    Expect(done.load() == 6 && !queue.Pending(), "a wait for the newest ticket covers every job");
    queue.Stop();
}

void testFailureKept() {
    RecordQueue queue;
    Exclusion exclusion(queue);
    bool later = false;
    queue.Post([] { throw std::runtime_error("record failed"); });
    queue.Post([&] { later = true; });
    {
        std::lock_guard lock(exclusion);
    }
    Expect(later, "a job after a failing one still runs");
    std::string message;
    try {
        if (auto failure = queue.TakeFailure()) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        message = error.what();
    }
    Expect(message == "record failed", "the producer receives the job's exception");
    Expect(queue.TakeFailure() == nullptr, "a failure is reported once");
    Expect(queue.Counts().failed == 1, "the failure is counted");
}

void testNoReentrantRun() {
    RecordQueue queue;
    Exclusion exclusion(queue);
    std::vector<std::string> order;
    queue.Post([&] {
        order.push_back("first begins");
        Expect(RecordQueue::Running(), "a job runs inside RunPending");
        std::lock_guard nested(exclusion);
        queue.RunPending();
        order.push_back("first ends");
    });
    queue.Post([&] { order.push_back("second"); });
    {
        std::lock_guard lock(exclusion);
    }
    Expect(order == std::vector<std::string>({"first begins", "first ends", "second"}), "a job never runs the next one inside itself");
    Expect(!RecordQueue::Running(), "the run ends with the drain");
}

class GatedExclusion {
public:
    explicit GatedExclusion(RecordQueue& queue) : queue(queue) {}
    void lock() {
        if (queue.Pending() && !RecordQueue::OnWorker()) queue.WaitIdle();
        mutex.lock();
    }
    void unlock() { mutex.unlock(); }

private:
    RecordQueue& queue;
    std::recursive_mutex mutex;
};

void testHoldersWaitForTheWorker() {
    RecordQueue queue;
    GatedExclusion exclusion(queue);
    queue.Start([&] {
        std::lock_guard lock(exclusion);
        queue.RunPending();
    });
    std::atomic<std::uint64_t> late{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> holders;
    for (int i = 0; i < 3; ++i) {
        holders.emplace_back([&] {
            while (!stop.load()) {
                const auto before = queue.Posted();
                std::lock_guard lock(exclusion);
                if (queue.Finished() < before) ++late;
            }
        });
    }
    std::vector<int> order;
    constexpr int Jobs = 500;
    for (int i = 0; i < Jobs; ++i) queue.Post([&order, i] { order.push_back(i); });
    queue.WaitIdle();
    stop = true;
    for (auto& holder : holders) holder.join();
    queue.Stop();
    bool ordered = order.size() == Jobs;
    for (int i = 0; ordered && i < Jobs; ++i) ordered = order[static_cast<std::size_t>(i)] == i;
    Expect(ordered, "the worker runs every job in posting order");
    Expect(late.load() == 0, "a holder finds every job posted before its acquisition finished");
    Expect(queue.Counts().byOthers == 0, "only the worker runs jobs when holders wait for it");
    Expect(!RecordQueue::OnWorker(), "the producer is not the worker");
}

void testStalePreparation() {
    const std::vector<PreparedRead> reads{{0x1000, 0x100}, {0x8000, 0x40}};
    const auto noPending = [](std::uint64_t, std::size_t) { return false; };
    const auto pendingAt8000 = [](std::uint64_t address, std::size_t bytes) { return address < 0x8040 && 0x8000 < address + bytes; };
    DrawRecordEffects quiet{5, 5, {}};
    Expect(StaleDrawPrepare(quiet, reads, pendingAt8000) == DrawPrepareStale::No, "a job that noted and stored nothing leaves the preparation valid");
    DrawRecordEffects noted{5, 6, {}};
    Expect(StaleDrawPrepare(noted, reads, noPending) == DrawPrepareStale::No, "noted writes elsewhere leave the preparation valid");
    Expect(StaleDrawPrepare(noted, reads, pendingAt8000) == DrawPrepareStale::PendingWrite, "a pending write over a prepared read makes it stale");
    Expect(StaleDrawPrepare(quiet, reads, pendingAt8000) == DrawPrepareStale::No, "older pending writes are not the job's");
    DrawRecordEffects stored{5, 5, {{0x10f0, 0x1200}}};
    Expect(StaleDrawPrepare(stored, reads, noPending) == DrawPrepareStale::Stored, "a driver store over a prepared read makes it stale");
    DrawRecordEffects touching{5, 5, {{0x1100, 0x2000}, {0x0, 0x1000}}};
    Expect(StaleDrawPrepare(touching, reads, noPending) == DrawPrepareStale::No, "stores that only touch a read leave it valid");
    const std::vector<PreparedRead> empty{{0x1000, 0}};
    Expect(StaleDrawPrepare(stored, empty, pendingAt8000) == DrawPrepareStale::No, "empty reads are never stale");
    DrawRecordEffects failed{5, 6, {}};
    Expect(StaleDrawPrepare(failed, reads, pendingAt8000) != DrawPrepareStale::No, "a job that did not finish counts as having noted writes");
}

void testOverlappingPackets() {
    Expect(OverlapsDrawRecords(0x27, false) && OverlapsDrawRecords(0x2d, false) && OverlapsDrawRecords(0x35, false), "direct draws overlap a pending record");
    Expect(OverlapsDrawRecords(0x24, false) && OverlapsDrawRecords(0x38, false), "indirect draws overlap a pending record (they settle themselves)");
    Expect(OverlapsDrawRecords(0x69, false) && OverlapsDrawRecords(0x76, false) && OverlapsDrawRecords(0x79, false), "register writes overlap a pending record");
    Expect(!OverlapsDrawRecords(0x3c, false) && !OverlapsDrawRecords(0x93, false), "memory waits wait for the records");
    Expect(!OverlapsDrawRecords(0x49, false) && !OverlapsDrawRecords(0x37, false) && !OverlapsDrawRecords(0x40, false), "labels and stores wait for the records");
    Expect(!OverlapsDrawRecords(0x15, false) && !OverlapsDrawRecords(0x16, false), "dispatches wait for the records");
    Expect(!OverlapsDrawRecords(0x50, false) && !OverlapsDrawRecords(0x22, false) && !OverlapsDrawRecords(0x10, false), "copies, conditions and NOP payloads wait for the records");
    Expect(!OverlapsDrawRecords(0x27, true) && !OverlapsDrawRecords(0x69, true), "a predicated packet waits for the records");
    Expect(!OverlapsDrawRecords(0, false), "flips and rendering waits wait for the records");
}

}

int main() {
    testJobsRunBeforeTheNextHolder();
    testOrderAcrossRunners();
    testWaitFinished();
    testFailureKept();
    testNoReentrantRun();
    testHoldersWaitForTheWorker();
    testStalePreparation();
    testOverlappingPackets();
    if (failures != 0) {
        std::fprintf(stderr, "%d record queue checks failed\n", failures);
        return 1;
    }
    std::printf("record queue checks passed\n");
    return 0;
}

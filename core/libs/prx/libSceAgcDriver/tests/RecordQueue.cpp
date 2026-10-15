#include "prx/libSceAgcDriver/Execution/include/RecordQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
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
    Expect(OverlapsDrawRecords(0x15, false, true) && OverlapsDrawRecords(0x16, false, true), "deferred dispatches overlap a pending record");
    Expect(!OverlapsDrawRecords(0x16, true, true), "a predicated dispatch waits for the records");
    Expect(!OverlapsDrawRecords(0x49, false, true) && !OverlapsDrawRecords(0x3c, false, true), "labels and waits wait with dispatches deferred too");
}

struct AheadJob {
    std::uint64_t ticket = 0;
    DrawRecordEffects effects;
};

bool noPendingWrite(std::uint64_t, std::size_t) {
    return false;
}

void testAheadWaitTicket() {
    Expect(AheadWaitTicket(0, 0, 3) == 0, "an idle queue never waits");
    Expect(AheadWaitTicket(2, 0, 3) == 0, "fewer than k records in flight leave room for one more");
    Expect(AheadWaitTicket(3, 0, 3) == 1, "k records in flight wait for the oldest");
    Expect(AheadWaitTicket(7, 1, 3) == 5, "more than k records in flight wait until k - 1 remain");
    Expect(AheadWaitTicket(4, 2, 1) == 4, "depth 1 waits for every earlier record");
}

void testAheadOrderingWithKInFlight() {
    constexpr std::uint32_t Depth = 3;
    constexpr int Jobs = 60;
    RecordQueue queue;
    std::mutex gate;
    queue.Start([&] {
        std::lock_guard lock(gate);
        queue.RunPending();
    });
    std::vector<int> order;
    std::uint64_t deepest = 0;
    bool bounded = true;
    std::unique_lock held(gate);
    for (int i = 0; i < Jobs; ++i) {
        if (const auto ticket = AheadWaitTicket(queue.Posted(), queue.Finished(), Depth); ticket != 0) {
            if (held.owns_lock()) held.unlock();
            queue.WaitFinished(ticket);
        }
        queue.Post([&order, i] {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            order.push_back(i);
        });
        const auto depth = queue.Posted() - queue.Finished();
        deepest = std::max(deepest, depth);
        bounded = bounded && depth <= Depth;
    }
    if (held.owns_lock()) held.unlock();
    queue.WaitIdle();
    queue.Stop();
    bool ordered = order.size() == Jobs;
    for (int i = 0; ordered && i < Jobs; ++i) ordered = order[static_cast<std::size_t>(i)] == i;
    Expect(ordered, "records posted k deep run once each, in posting order");
    Expect(bounded, "the producer never has more than k records in flight");
    Expect(deepest == Depth, "the producer posts k records before it waits for the first");
}

void testAheadStaleAgainstEarlierJob() {
    const std::vector<PreparedRead> reads{{0x1000, 0x100}};
    PrepareAheadWindow<AheadJob> window;
    window.Begin(0);
    auto first = std::make_shared<AheadJob>(AheadJob{1, DrawRecordEffects{5, 5, {{0x1080, 0x1090}}}});
    auto second = std::make_shared<AheadJob>(AheadJob{2, DrawRecordEffects{5, 5, {}}});
    auto third = std::make_shared<AheadJob>(AheadJob{3, DrawRecordEffects{5, 6, {{0x9000, 0x9100}}}});
    window.Add(first);
    window.Add(second);
    window.Add(third);
    Expect(window.CheckFinished(reads, 3, noPendingWrite) == DrawPrepareStale::Stored, "a store by the oldest of three finished records makes the preparation stale");
    Expect(window.CheckFinished(reads, 0, noPendingWrite) == DrawPrepareStale::No, "records that have not finished are not judged at post");
    window.Begin(1);
    Expect(window.Size() == 2, "records finished before the preparation began leave the window");
    Expect(window.CheckFinished(reads, 3, noPendingWrite) == DrawPrepareStale::No, "a store finished before the preparation began leaves it valid");
    Expect(!window.Touched(2), "a quiet record leaves an unsettled preparation valid");
    Expect(window.Touched(3), "a record that noted writes makes an unsettled preparation stale");
    const auto earlier = window.Between(1, 3);
    Expect(earlier.size() == 2, "the provisional job carries every record still unfinished at post");
    Expect(StaleAfterEach(earlier, reads, noPendingWrite) == DrawPrepareStale::No, "the unfinished records are clean when the job is posted");
    third->effects.stores.push_back({0x10f8, 0x1100});
    Expect(StaleAfterEach(earlier, reads, noPendingWrite) == DrawPrepareStale::Stored, "a store an unfinished record makes after the post is seen when the provisional job starts");
}

void testAheadStaleAtRunFailsClosed() {
    RecordQueue queue;
    queue.StopAfterFailure(true);
    Exclusion exclusion(queue);
    const std::vector<PreparedRead> reads{{0x2000, 0x40}};
    auto earlier = std::make_shared<AheadJob>(AheadJob{1, DrawRecordEffects{}});
    const std::vector<std::shared_ptr<const DrawRecordEffects>> behind{std::shared_ptr<const DrawRecordEffects>(earlier, &earlier->effects)};
    bool provisionalRan = false;
    bool laterRan = false;
    queue.Post([earlier] { earlier->effects.stores.push_back({0x2020, 0x2030}); });
    queue.Post([&reads, &provisionalRan, behind] {
        try {
            if (StaleAfterEach(behind, reads, noPendingWrite) != DrawPrepareStale::No) throw std::runtime_error("went stale");
            provisionalRan = true;
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("draw at dword 7: ") + error.what());
        }
    });
    queue.Post([&laterRan] { laterRan = true; });
    {
        std::lock_guard lock(exclusion);
    }
    Expect(!provisionalRan, "a provisional job whose preparation went stale is not recorded");
    Expect(!laterRan, "no job after a failed one runs before the producer takes the failure");
    Expect(queue.Finished() == 3 && !queue.Pending(), "dropped jobs still finish their tickets");
    std::string message;
    try {
        if (auto failure = queue.TakeFailure()) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        message = error.what();
    }
    Expect(message == "draw at dword 7: went stale", "the failure names the job that went stale");
    Expect(queue.Counts().failed == 1 && queue.Counts().dropped == 1, "the stale job counts as failed and the later one as dropped");

    queue.Post([] { throw std::runtime_error("record failed"); });
    {
        std::lock_guard lock(exclusion);
    }
    bool postedBeforeTake = false;
    queue.Post([&postedBeforeTake] { postedBeforeTake = true; });
    Expect(queue.TakeFailure() != nullptr, "the second failure is reported");
    {
        std::lock_guard lock(exclusion);
    }
    Expect(!postedBeforeTake, "a job posted before the producer took the failure is dropped as well");
    bool postedAfterTake = false;
    queue.Post([&postedAfterTake] { postedAfterTake = true; });
    {
        std::lock_guard lock(exclusion);
    }
    Expect(postedAfterTake, "jobs posted after the failure was taken run again");
    Expect(queue.TakeFailure() == nullptr, "dropping a job is not a new failure");
}

void testAheadDrainAtNonDrawPacket() {
    constexpr int Depth = 4;
    RecordQueue queue;
    std::mutex gate;
    queue.Start([&] {
        std::lock_guard lock(gate);
        queue.RunPending();
    });
    std::atomic<int> ran{0};
    const auto packet = [&](std::uint32_t opcode) {
        if (!OverlapsDrawRecords(opcode, false)) queue.WaitIdle();
    };
    std::unique_lock held(gate);
    for (int i = 0; i < Depth; ++i) queue.Post([&ran] { ++ran; });
    packet(0x69);
    packet(0x27);
    Expect(queue.Posted() - queue.Finished() == Depth && ran.load() == 0, "register writes and draws leave k records in flight");
    held.unlock();
    packet(0x3c);
    Expect(!queue.Pending() && ran.load() == Depth, "a memory wait drains every record in flight, not only the newest");
    queue.Stop();
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
    testAheadWaitTicket();
    testAheadOrderingWithKInFlight();
    testAheadStaleAgainstEarlierJob();
    testAheadStaleAtRunFailsClosed();
    testAheadDrainAtNonDrawPacket();
    if (failures != 0) {
        std::fprintf(stderr, "%d record queue checks failed\n", failures);
        return 1;
    }
    std::printf("record queue checks passed\n");
    return 0;
}

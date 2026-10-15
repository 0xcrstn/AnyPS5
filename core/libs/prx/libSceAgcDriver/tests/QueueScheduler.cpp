#include "prx/libSceAgcDriver/Execution/include/QueueScheduler.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using AgcDriver::ParkedOp;
using AgcDriver::QueueScheduler;
using AgcDriver::SchedulerMode;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

template<typename TAction>
std::string Failure(TAction action) {
    try {
        action();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

bool WaitUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void TestModes() {
    Expect(QueueScheduler::ParseMode(nullptr) == SchedulerMode::Dag, "an unset scheduler mode is dag");
    Expect(QueueScheduler::ParseMode("off") == SchedulerMode::Off, "off parses");
    Expect(QueueScheduler::ParseMode("serial") == SchedulerMode::Serial, "serial parses");
    Expect(QueueScheduler::ParseMode("dag") == SchedulerMode::Dag, "dag parses");
    Expect(!Failure([] { QueueScheduler::ParseMode("fast"); }).empty(), "an unknown scheduler mode throws");
    QueueScheduler off(SchedulerMode::Off, false);
    Expect(!off.Active(), "the off scheduler is inactive");
    const auto turn = off.Begin(0, 1, 0, 1);
    Expect(off.Holder() == QueueScheduler::NoQueue, "the off scheduler grants no turn");
}

void TestSerialExclusion() {
    QueueScheduler scheduler(SchedulerMode::Serial, true);
    std::atomic<int> inside{0};
    std::atomic<bool> overlapped{false};
    std::vector<std::thread> workers;
    for (std::uint32_t queue = 0; queue < 4; ++queue) {
        workers.emplace_back([&, queue] {
            for (std::size_t cursor = 0; cursor < 200; ++cursor) {
                const auto turn = scheduler.Begin(queue, 1, cursor, queue + 1);
                if (inside.fetch_add(1) != 0) overlapped = true;
                std::this_thread::yield();
                inside.fetch_sub(1);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Expect(!overlapped.load(), "two streams ran a packet at once in serial mode");
    const auto statistics = scheduler.Take();
    Expect(statistics.turns == 800, "every packet took one turn");
}

void TestRunnableHeadOrder() {
    QueueScheduler scheduler(SchedulerMode::Serial, false);
    std::mutex orderMutex;
    std::vector<std::uint32_t> order;
    auto first = std::make_unique<QueueScheduler::Turn>(scheduler.Begin(1, 1, 0, 10));
    const auto request = [&](std::uint32_t queue, std::uint64_t received) {
        const auto turn = scheduler.Begin(queue, 1, 0, received);
        std::lock_guard lock(orderMutex);
        order.push_back(queue);
    };
    std::thread later(request, 2, 30);
    std::thread older(request, 3, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    first.reset();
    later.join();
    older.join();
    Expect(order.size() == 2 && order[0] == 3 && order[1] == 2, "the serial turn went to a later head before the oldest runnable head");
}

void TestBlockReleasesTurn() {
    QueueScheduler scheduler(SchedulerMode::Serial, false);
    std::atomic<bool> otherRan{false};
    {
        const auto turn = scheduler.Begin(1, 1, 0, 1);
        {
            const auto blocked = scheduler.Block(1);
            Expect(scheduler.Holder() == QueueScheduler::NoQueue, "a blocked stream kept the serial turn");
            std::thread other([&] {
                const auto otherTurn = scheduler.Begin(2, 1, 0, 2);
                otherRan = true;
            });
            other.join();
        }
        Expect(otherRan.load(), "another stream could not run while the holder was blocked");
        Expect(scheduler.Holder() == 1, "a stream did not get its turn back after its block");
    }
    Expect(scheduler.Holder() == QueueScheduler::NoQueue, "the turn was not released at the end of the packet");
}

void TestParkResume() {
    QueueScheduler scheduler(SchedulerMode::Serial, true);
    std::function<void()> wake;
    std::mutex wakeMutex;
    std::atomic<bool> resumed{false};
    std::atomic<bool> otherRan{false};
    std::thread parker([&] {
        const auto turn = scheduler.Begin(1, 1, 0, 1);
        ParkedOp op;
        op.queue = 1;
        op.submission = 1;
        op.what = "draw at dword 0";
        op.arm = [&](std::function<void()> callback) {
            std::lock_guard lock(wakeMutex);
            wake = std::move(callback);
            return true;
        };
        op.resume = [&] {
            Expect(scheduler.Holder() == 1, "a parked op resumed without its serial turn");
            resumed = true;
        };
        scheduler.Park(std::move(op));
    });
    Expect(WaitUntil([&] { std::lock_guard lock(wakeMutex); return static_cast<bool>(wake); }), "the parked op was never armed");
    {
        const auto turn = scheduler.Begin(2, 1, 0, 2);
        otherRan = true;
    }
    Expect(!resumed.load(), "a parked op resumed before its ticket settled");
    {
        std::lock_guard lock(wakeMutex);
        wake();
    }
    parker.join();
    Expect(otherRan.load() && resumed.load(), "a parked stream blocked another stream or never resumed");

    bool inline_ = false;
    {
        const auto turn = scheduler.Begin(1, 1, 1, 1);
        ParkedOp settled;
        settled.queue = 1;
        settled.arm = [](std::function<void()>) { return false; };
        settled.resume = [&] { inline_ = true; };
        scheduler.Park(std::move(settled));
    }
    Expect(inline_, "an op parked on a settled ticket did not resume");
    const auto statistics = scheduler.Take();
    Expect(statistics.parks == 2 && statistics.parksSettledAtArm == 1, "park statistics are wrong");
}

void TestParkFailure() {
    QueueScheduler scheduler(SchedulerMode::Dag, false);
    ParkedOp failing;
    failing.queue = 3;
    failing.arm = [](std::function<void()>) { return false; };
    failing.resume = [] { throw std::runtime_error("pipeline compile failed"); };
    Expect(Failure([&] { scheduler.Park(std::move(failing)); }) == "pipeline compile failed", "a parked op's failure did not reach its stream");

    std::atomic<bool> armed{false};
    std::string closed;
    std::thread parker([&] {
        ParkedOp never;
        never.queue = 4;
        never.arm = [&](std::function<void()>) { armed = true; return true; };
        never.resume = [] {};
        closed = Failure([&] { scheduler.Park(std::move(never)); });
    });
    Expect(WaitUntil([&] { return armed.load(); }), "the parked op was never armed");
    scheduler.Close(std::make_exception_ptr(std::runtime_error("driver shutting down")));
    parker.join();
    Expect(closed == "driver shutting down", "closing the scheduler did not fail a parked stream");
    Expect(!Failure([&] { scheduler.Begin(4, 2, 0, 9); }).empty(), "a closed scheduler began a packet");
}

void TestDagRunsConcurrently() {
    QueueScheduler scheduler(SchedulerMode::Dag, false);
    std::atomic<int> inside{0};
    std::atomic<bool> together{false};
    std::vector<std::thread> workers;
    for (std::uint32_t queue = 0; queue < 2; ++queue) {
        workers.emplace_back([&, queue] {
            const auto turn = scheduler.Begin(queue, 1, 0, queue);
            inside.fetch_add(1);
            if (WaitUntil([&] { return inside.load() == 2; })) together = true;
        });
    }
    for (auto& worker : workers) worker.join();
    Expect(together.load(), "dag mode serialized two streams");
}

void TestVerifyInjection() {
    QueueScheduler scheduler(SchedulerMode::Dag, true);
    { const auto turn = scheduler.Begin(5, 2, 10, 1); }
    const auto reordered = Failure([&] { scheduler.Begin(5, 2, 4, 1); });
    Expect(reordered.find("APS5_QUEUE_SCHEDULER_VERIFY") != std::string::npos, "verify mode accepted a stream running a packet out of order");
    const auto older = Failure([&] { scheduler.Begin(5, 1, 20, 1); });
    Expect(older.find("APS5_QUEUE_SCHEDULER_VERIFY") != std::string::npos, "verify mode accepted an older submission after a newer one");

    std::string whileParked;
    ParkedOp op;
    op.queue = 6;
    op.arm = [&](std::function<void()>) {
        whileParked = Failure([&] { scheduler.Begin(6, 1, 0, 1); });
        return false;
    };
    op.resume = [] {};
    scheduler.Park(std::move(op));
    Expect(whileParked.find("APS5_QUEUE_SCHEDULER_VERIFY") != std::string::npos, "verify mode let a parked stream begin another packet");

    QueueScheduler unverified(SchedulerMode::Dag, false);
    { const auto turn = unverified.Begin(5, 2, 10, 1); }
    Expect(Failure([&] { unverified.Begin(5, 2, 4, 1); }).empty(), "the order check ran without verify mode");
}

}

std::string SchedulePath(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("aps5-schedule-") + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt")).string();
}

std::vector<std::pair<std::uint32_t, std::size_t>> RunStreams(QueueScheduler& scheduler, bool computeFirst) {
    std::mutex orderMutex;
    std::vector<std::pair<std::uint32_t, std::size_t>> order;
    const auto run = [&](std::uint32_t queue, bool delayed) {
        if (delayed) std::this_thread::sleep_for(std::chrono::milliseconds(30));
        for (std::uint64_t submission = 1; submission <= 3; ++submission) {
            for (std::size_t cursor = 0; cursor < 3; ++cursor) {
                const auto turn = scheduler.Begin(queue, submission * 10 + queue, cursor, submission * 10 + queue);
                {
                    std::lock_guard lock(orderMutex);
                    order.emplace_back(queue, submission * 10 + cursor);
                }
                if (cursor == 1) scheduler.NoteWait(queue, submission * 0x100 + queue, 3);
            }
        }
    };
    std::thread graphics([&] { run(0, computeFirst); });
    std::thread compute([&] { run(0x20, !computeFirst); });
    graphics.join();
    compute.join();
    return order;
}

void TestCaptureReplay() {
    Expect(QueueScheduler::ParseMode("replay") == SchedulerMode::Replay, "replay parses");
    Expect(Failure([] { QueueScheduler scheduler(SchedulerMode::Replay, false); }).find("APS5_QUEUE_SCHEDULER_REPLAY") != std::string::npos, "replay without a capture file throws");
    const auto path = SchedulePath("order");
    std::vector<std::pair<std::uint32_t, std::size_t>> captured;
    {
        QueueScheduler capture(SchedulerMode::Serial, true, {path, ""});
        Expect(capture.Capturing() && capture.Active(), "a capturing scheduler is active");
        captured = RunStreams(capture, false);
        const auto statistics = capture.Take();
        Expect(statistics.capturedTurns == 18 && statistics.capturedWaits == 6, "the capture did not record every turn and wait");
    }
    {
        QueueScheduler replay(SchedulerMode::Replay, true, {"", path});
        const auto replayed = RunStreams(replay, true);
        Expect(replayed == captured, "the replay did not reproduce the captured order of turns");
        const auto statistics = replay.Take();
        Expect(statistics.replayedTurns == 18 && statistics.replayedWaits == 6 && statistics.replayMismatches == 0, "the replay did not follow and check every captured turn and wait");
    }
    {
        QueueScheduler replay(SchedulerMode::Replay, true, {"", path});
        const auto turn = replay.Begin(0, 10, 0, 10);
        Expect(Failure([&] { replay.NoteWait(0, 0x999, 3); }).find("the capture has no wait there") != std::string::npos, "a wait the capture did not have was not reported");
    }
    {
        QueueScheduler replay(SchedulerMode::Replay, true, {"", path});
        std::string failure;
        {
            const auto first = replay.Begin(0, 10, 0, 10);
        }
        {
            const auto second = replay.Begin(0, 10, 1, 10);
            failure = Failure([&] { replay.NoteWait(0, 0x777, 3); });
        }
        Expect(failure.find("the capture observed 0x10") != std::string::npos, "a wait that observed another value than the capture was not reported");
        Expect(Failure([&] { const auto third = replay.Begin(0, 10, 4, 10); }).find("where the capture had") != std::string::npos, "a replay that reached another packet than the capture was not reported");
    }
    {
        QueueScheduler replay(SchedulerMode::Replay, false, {"", path});
        {
            const auto first = replay.Begin(0, 10, 0, 10);
        }
        {
            const auto second = replay.Begin(0, 10, 1, 10);
            replay.NoteWait(0, 0x777, 3);
        }
        Expect(replay.Take().replayMismatches == 1, "without verify a differing wait is counted, not thrown");
    }
    std::filesystem::remove(path);
}

int main() {
    TestModes();
    TestSerialExclusion();
    TestRunnableHeadOrder();
    TestBlockReleasesTurn();
    TestParkResume();
    TestParkFailure();
    TestDagRunsConcurrently();
    TestVerifyInjection();
    TestCaptureReplay();
    if (failures != 0) return 1;
    std::puts("queue scheduler tests passed");
    return 0;
}

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/PrepareWorkers.hpp"
#include "prx/libSceAgcDriver/Execution/include/RecordQueue.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using AgcDriver::QueueState;
using AgcDriver::RecordQueue;
using AgcDriver::Registers;
using namespace AgcDriver::DriverDetail;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

template <class TCondition>
bool WaitUntil(TCondition condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool NoPendingWrite(std::uint64_t, std::size_t) {
    return false;
}

void testEmitInTicketOrderWithOutOfOrderCompletion() {
    std::mutex mutex;
    std::vector<int> emitted;
    std::vector<int> completed;
    std::atomic<int> concurrent{0};
    std::atomic<int> peak{0};
    {
        OrderedPrepare<int, int> pipeline(4, 6, [&](int& job) {
            const auto now = ++concurrent;
            auto seen = peak.load();
            while (seen < now && !peak.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8 - (job - 1) % 8));
            --concurrent;
            {
                std::lock_guard lock(mutex);
                completed.push_back(job);
            }
            return job * 10;
        }, [&](int& job, std::optional<int>& result, std::exception_ptr error) {
            std::lock_guard lock(mutex);
            Expect(error == nullptr && result.has_value() && *result == job * 10, "an emitted result belongs to its own job");
            emitted.push_back(job);
        });
        for (int job = 1; job <= 40; ++job) pipeline.Submit(job);
        pipeline.WaitEmitted();
        Expect(pipeline.Idle(), "the pipeline is idle after a drain");
        const auto counts = pipeline.Counts();
        Expect(counts.submitted == 40 && counts.emitted == 40, "every submitted job was emitted");
    }
    Expect(emitted.size() == 40, "forty jobs emitted");
    for (std::size_t index = 0; index < emitted.size(); ++index) Expect(emitted[index] == static_cast<int>(index) + 1, "jobs are emitted in ticket order");
    Expect(!std::is_sorted(completed.begin(), completed.end()), "preparations completed out of ticket order");
    Expect(peak.load() >= 2, "preparations ran concurrently");
}

void testBoundBlocksTheProducer() {
    std::atomic<bool> release{false};
    std::atomic<int> emitted{0};
    OrderedPrepare<int, int> pipeline(2, 3, [&](int& job) {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return job;
    }, [&](int&, std::optional<int>&, std::exception_ptr) { ++emitted; });
    for (int job = 1; job <= 3; ++job) pipeline.Submit(job);
    Expect(pipeline.Counts().fullWaits == 0, "jobs within the bound do not wait");
    std::atomic<bool> fourth{false};
    std::thread producer([&] {
        pipeline.Submit(4);
        fourth = true;
    });
    Expect(WaitUntil([&] { return pipeline.Counts().fullWaits == 1; }), "the job past the bound waits for the reorder buffer");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(!fourth.load(), "the producer stays blocked while the buffer is full");
    release = true;
    producer.join();
    pipeline.WaitEmitted();
    Expect(fourth.load() && emitted.load() == 4, "the blocked job went in once results were emitted");
}

void testDrainWaitsForEveryEmit() {
    std::atomic<int> emitted{0};
    OrderedPrepare<int, int> pipeline(3, 8, [](int& job) { return job; }, [&](int&, std::optional<int>&, std::exception_ptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ++emitted;
    });
    for (int job = 1; job <= 5; ++job) pipeline.Submit(job);
    Expect(!pipeline.Idle() || emitted.load() == 5, "the pipeline reports pending work before its emits ran");
    pipeline.WaitEmitted();
    Expect(emitted.load() == 5, "a drain returns only after every submitted job was emitted");
    Expect(pipeline.Idle(), "the pipeline is idle after a drain");
    pipeline.WaitEmitted();
    Expect(emitted.load() == 5, "a drain of an idle pipeline returns at once");
}

void testWorkerErrorsAreEmittedInOrder() {
    std::vector<std::pair<int, bool>> emitted;
    std::mutex mutex;
    OrderedPrepare<int, int> pipeline(3, 4, [](int& job) {
        if (job == 3) throw std::runtime_error("preparation failed");
        return job;
    }, [&](int& job, std::optional<int>& result, std::exception_ptr error) {
        std::lock_guard lock(mutex);
        emitted.emplace_back(job, error != nullptr && !result.has_value());
    });
    for (int job = 1; job <= 6; ++job) pipeline.Submit(job);
    pipeline.WaitEmitted();
    Expect(emitted.size() == 6, "a failed preparation does not stop later jobs");
    for (std::size_t index = 0; index < emitted.size(); ++index) {
        Expect(emitted[index].first == static_cast<int>(index) + 1, "a failed preparation keeps its place in the order");
        Expect(emitted[index].second == (emitted[index].first == 3), "only the failed job carries its error");
    }
}

void testEmitFailureReachesTheProducer() {
    OrderedPrepare<int, int> pipeline(2, 4, [](int& job) { return job; }, [](int& job, std::optional<int>&, std::exception_ptr) {
        if (job == 2) throw std::runtime_error("emit failed");
    });
    pipeline.Submit(1);
    pipeline.Submit(2);
    bool thrown = false;
    try {
        pipeline.WaitEmitted();
    } catch (const std::runtime_error& error) {
        thrown = std::string(error.what()) == "emit failed";
    }
    Expect(thrown, "a drain rethrows an emit failure");
    thrown = false;
    try {
        pipeline.Submit(3);
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    Expect(thrown, "a submit after an emit failure rethrows it");
}

DrawRecordEffects Stores(std::uint64_t begin, std::uint64_t end) {
    DrawRecordEffects effects;
    effects.writeGenerationBefore = 1;
    effects.writeGenerationAfter = 2;
    effects.stores.emplace_back(begin, end);
    return effects;
}

void testFrozenStaleCheck() {
    RecordHistory history(16);
    history.Note(1, std::make_shared<const DrawRecordEffects>(Stores(0x1000, 0x1004)));
    history.Note(2, std::make_shared<const DrawRecordEffects>());
    const std::array<PreparedRead, 1> overlapping{PreparedRead{0x1000, 4}};
    const std::array<PreparedRead, 1> elsewhere{PreparedRead{0x2000, 4}};
    Expect(CheckFrozenPrepare(history, 0, 3, overlapping, NoPendingWrite) == FrozenPrepareVerdict::Stored, "a store by a record that finished after the preparation started makes it stale");
    Expect(CheckFrozenPrepare(history, 1, 3, overlapping, NoPendingWrite) == FrozenPrepareVerdict::Fresh, "a record that finished before the preparation started is not held against it");
    Expect(CheckFrozenPrepare(history, 0, 3, elsewhere, NoPendingWrite) == FrozenPrepareVerdict::Fresh, "a store elsewhere leaves the preparation fresh");
    Expect(CheckFrozenPrepare(history, 0, 1, overlapping, NoPendingWrite) == FrozenPrepareVerdict::Fresh, "the first record has no earlier record to check");
    DrawRecordEffects noted;
    noted.writeGenerationBefore = 4;
    noted.writeGenerationAfter = 5;
    history.Note(3, std::make_shared<const DrawRecordEffects>(noted));
    const auto pending = [](std::uint64_t address, std::size_t) { return address == 0x2000; };
    Expect(CheckFrozenPrepare(history, 2, 4, elsewhere, pending) == FrozenPrepareVerdict::PendingWrite, "a pending write noted by an earlier record makes the preparation stale");
    Expect(CheckFrozenPrepare(history, 2, 4, overlapping, pending) == FrozenPrepareVerdict::Fresh, "a pending write elsewhere leaves the preparation fresh");
    history.Note(9, std::make_shared<const DrawRecordEffects>());
    Expect(CheckFrozenPrepare(history, 2, 10, elsewhere, NoPendingWrite) == FrozenPrepareVerdict::Unchecked, "a gap in the record history is never taken as fresh");
    Expect(CheckFrozenPrepare(history, 8, 10, elsewhere, NoPendingWrite) == FrozenPrepareVerdict::Fresh, "the history restarts after a gap");
    RecordHistory bounded(2);
    for (std::uint64_t ticket = 1; ticket <= 5; ++ticket) bounded.Note(ticket, std::make_shared<const DrawRecordEffects>());
    Expect(CheckFrozenPrepare(bounded, 1, 6, elsewhere, NoPendingWrite) == FrozenPrepareVerdict::Unchecked, "records evicted from the history are not taken as fresh");
    Expect(CheckFrozenPrepare(bounded, 3, 6, elsewhere, NoPendingWrite) == FrozenPrepareVerdict::Fresh, "records still in the history are checked");
}

struct FrozenJob {
    std::size_t reads;
    std::optional<std::size_t> stores;
    std::uint32_t value;
};

struct PreparedJob {
    std::uint64_t start;
    std::uint32_t read;
};

void testStaleResultIsRedoneInOrder() {
    std::array<std::uint32_t, 4> memory{};
    std::mutex memoryMutex;
    const auto address = [](std::size_t slot) { return 0x1000 + slot * sizeof(std::uint32_t); };
    RecordQueue records;
    RecordHistory history(64);
    std::vector<std::uint32_t> recorded(4, 0xffffffffu);
    std::atomic<int> redone{0};
    const std::array<FrozenJob, 4> jobs{FrozenJob{3, 0, 7}, FrozenJob{0, std::nullopt, 0}, FrozenJob{2, std::nullopt, 0}, FrozenJob{2, 2, 9}};
    const auto read = [&](std::size_t slot) {
        std::lock_guard lock(memoryMutex);
        return memory[slot];
    };
    {
        OrderedPrepare<std::size_t, PreparedJob> pipeline(3, 8, [&](std::size_t& index) {
            return PreparedJob{records.Finished(), read(jobs[index].reads)};
        }, [&](std::size_t& index, std::optional<PreparedJob>& prepared, std::exception_ptr error) {
            Expect(error == nullptr && prepared.has_value(), "the preparation succeeded");
            records.Post([&, index, prepared = *prepared] {
                const auto ticket = records.Finished() + 1;
                const auto& job = jobs[index];
                auto effects = std::make_shared<DrawRecordEffects>();
                effects->writeGenerationBefore = 1;
                effects->writeGenerationAfter = 1;
                auto value = prepared.read;
                const std::array<PreparedRead, 1> reads{PreparedRead{address(job.reads), 4}};
                if (CheckFrozenPrepare(history, prepared.start, ticket, reads, NoPendingWrite) != FrozenPrepareVerdict::Fresh) {
                    ++redone;
                    value = read(job.reads);
                }
                recorded[index] = value;
                if (job.stores) {
                    std::lock_guard lock(memoryMutex);
                    memory[*job.stores] = job.value;
                    effects->stores.emplace_back(address(*job.stores), address(*job.stores) + 4);
                }
                history.Note(ticket, effects);
            });
        });
        for (std::size_t index = 0; index < jobs.size(); ++index) pipeline.Submit(index);
        pipeline.WaitEmitted();
    }
    records.RunPending();
    Expect(records.Finished() == 4, "every record ran");
    Expect(recorded[0] == 0, "the first draw reads the value before any record");
    Expect(recorded[1] == 7, "a draw prepared before the earlier record's store is redone and reads the stored value");
    Expect(recorded[2] == 0, "a later store does not reach an earlier draw");
    Expect(recorded[3] == 0, "a draw reads before its own store and keeps its preparation");
    Expect(redone.load() == 1, "only the stale preparation was redone");
}

void testRegisterSnapshotCopyOnWrite() {
    QueueState queue;
    RegisterFreezer freezer;
    const auto first = freezer.Freeze(queue);
    Expect(freezer.Copies() == 3, "the first freeze copies every register file");
    const auto second = freezer.Freeze(queue);
    Expect(second.registers.shader == first.registers.shader && second.registers.context == first.registers.context && second.registers.userConfig == first.registers.userConfig, "an unchanged queue shares its frozen register files");
    Expect(freezer.Copies() == 3, "an unchanged queue copies nothing");
    queue.shader.insert_or_assign(0x10, 5);
    const auto third = freezer.Freeze(queue);
    Expect(third.registers.shader != first.registers.shader, "a register write gives the next draw a new shader file");
    Expect(third.registers.context == first.registers.context && third.registers.userConfig == first.registers.userConfig, "untouched files stay shared");
    Expect(!first.registers.shader->contains(0x10), "an earlier draw's frozen file does not see a later write");
    Expect(third.registers.shader->at(0x10) == 5, "the new frozen file holds the write");
    Expect(freezer.Copies() == 4, "only the written file was copied");
    queue.shader.insert_or_assign(0x10, 5);
    Expect(freezer.Freeze(queue).registers.shader == third.registers.shader, "rewriting the same value copies nothing");
    queue.ClearContext();
    const auto fourth = freezer.Freeze(queue);
    Expect(fourth.registers.context != third.registers.context, "an assigned register file is copied again");
    queue.userConfig[0x24a] = 7;
    queue.indexBase = 0x1234;
    queue.instanceCount = 3;
    const auto fifth = freezer.Freeze(queue);
    Expect(fifth.registers.userConfig != fourth.registers.userConfig && fifth.registers.userConfig->at(0x24a) == 7, "a write through a register reference is seen");
    Expect(fifth.indexBase == 0x1234 && fifth.instanceCount == 3, "the queue's draw scalars are frozen");
    queue.shader.erase(0x10);
    Expect(freezer.Freeze(queue).registers.shader != fifth.registers.shader, "an erase is a write");

    FrozenQueueCopy copy;
    auto& state = copy.Apply(fifth);
    Expect(copy.Copies() == 3, "a worker copies every file the first time");
    Expect(state.shader.at(0x10) == 5 && state.userConfig.at(0x24a) == 7 && state.indexBase == 0x1234 && state.instanceCount == 3, "the worker copy holds the frozen state");
    copy.Apply(fifth);
    Expect(copy.Copies() == 3, "the same frozen files are not copied again");
    state.shader.insert_or_assign(0x11, 1);
    copy.Apply(fifth);
    Expect(copy.Copies() == 4 && !state.shader.contains(0x11), "a worker copy that was written is restored from its frozen file");
    QueueState other;
    const auto foreign = freezer.Freeze(other);
    Expect(foreign.registers.shader != fifth.registers.shader && freezer.Copies() >= 9, "a different queue state is frozen from scratch");
}

}

int main() {
    testEmitInTicketOrderWithOutOfOrderCompletion();
    testBoundBlocksTheProducer();
    testDrainWaitsForEveryEmit();
    testWorkerErrorsAreEmittedInOrder();
    testEmitFailureReachesTheProducer();
    testFrozenStaleCheck();
    testStaleResultIsRedoneInOrder();
    testRegisterSnapshotCopyOnWrite();
    if (failures != 0) {
        std::fprintf(stderr, "%d prepare worker checks failed\n", failures);
        return 1;
    }
    std::printf("prepare worker checks passed\n");
    return 0;
}

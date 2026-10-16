#include "prx/libSceAgcDriver/Execution/include/MeasurementWorkload.hpp"
#include <iostream>
#include <thread>

using AgcDriver::MeasurementWorkload;
using Kind = MeasurementWorkload::Kind;
using Outcome = MeasurementWorkload::Outcome;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void CompleteWindowRetainsPerPresentCounts() {
    MeasurementWorkload ledger(2);
    ledger.Open(600, 1000);
    ledger.Begin(Kind::Draw);
    ledger.Begin(Kind::HostDraw);
    ledger.End(Kind::HostDraw, Outcome::Complete);
    ledger.End(Kind::Draw, Outcome::Complete);
    ledger.Boundary(601, 2000, false);
    ledger.Begin(Kind::Dispatch);
    ledger.End(Kind::Dispatch, Outcome::Skipped);
    ledger.Boundary(602, 3000, true);
    const auto saved = ledger.Collect();
    Require(saved.closed && saved.frames.size() == 2 && saved.lost == 0 && !saved.lifetimeError, "complete bounded window");
    Require(saved.frames[0].present == 600 && saved.frames[0].counts[0] == 1 && saved.frames[0].counts[3] == 1, "first present owns draw observations");
    Require(saved.frames[1].present == 601 && saved.frames[1].counts[1] == 1 && saved.frames[1].counts[6] == 1, "second present owns skipped dispatch");
    Require(saved.counts[0] == 1 && saved.counts[1] == 1 && saved.counts[5] == 1 && saved.counts[6] == 1, "requested and terminal counts retained");
    Require(saved.pendingAtOpen == MeasurementWorkload::Pending{} && saved.pendingAtClose == MeasurementWorkload::Pending{}, "no fabricated pending work");
}

void CrossingWorkRemainsVisible() {
    MeasurementWorkload ledger(1);
    ledger.Begin(Kind::Draw);
    ledger.Open(10, 100);
    ledger.End(Kind::Draw, Outcome::Complete);
    ledger.Begin(Kind::Dispatch);
    ledger.Boundary(11, 200, true);
    ledger.End(Kind::Dispatch, Outcome::Complete);
    const auto saved = ledger.Collect();
    Require(saved.pendingAtOpen[0] == 1 && saved.pendingAtClose[0] == 1, "both edge crossings recorded");
    Require(saved.counts[0] == 0 && saved.counts[1] == 1 && saved.counts[5] == 1, "outside-window actions are not retrospectively counted");
    Require(ledger.Collect().pendingAtClose[0] == 1, "late completion cannot rewrite frozen close state");
}

void DeferredJobsPreserveQueueLifetime() {
    MeasurementWorkload ledger(2);
    ledger.Open(1, 100);
    ledger.Begin(Kind::Draw);
    ledger.Begin(Kind::Job);
    ledger.End(Kind::Draw, Outcome::Complete);
    ledger.Boundary(2, 200, false);
    std::thread worker([&] {
        ledger.Begin(Kind::HostDraw);
        ledger.End(Kind::HostDraw, Outcome::Complete);
        ledger.End(Kind::Job, Outcome::Complete);
    });
    worker.join();
    ledger.Boundary(3, 300, true);
    const auto saved = ledger.Collect();
    Require(saved.frames[0].pendingAtEnd[2] == 1 && saved.frames[1].pendingAtBegin[2] == 1, "queued job spans intermediate boundary");
    Require(saved.frames[1].counts[3] == 1 && saved.counts[12] == 1 && saved.counts[13] == 1, "worker emission and completion recorded");
    Require(saved.pendingAtClose == MeasurementWorkload::Pending{}, "worker completion releases pending lifetime");
}

void ConcurrentObservationsAreNotLost() {
    MeasurementWorkload ledger(1);
    ledger.Open(0, 100);
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned index = 0; index < 100; ++index) {
            ledger.Begin(Kind::Dispatch);
            ledger.Begin(Kind::HostDispatch);
            ledger.End(Kind::HostDispatch, Outcome::Complete);
            ledger.End(Kind::Dispatch, Outcome::Complete);
        }
    });
    for (auto& worker : workers) worker.join();
    ledger.Boundary(1, 200, true);
    const auto saved = ledger.Collect();
    Require(saved.counts[1] == 400 && saved.counts[4] == 400 && saved.counts[5] == 400 && saved.counts[10] == 400, "concurrent observations exact");
    Require(!saved.lifetimeError && saved.pendingAtClose == MeasurementWorkload::Pending{}, "concurrent lifetimes balance");
}

void CapacityAndCorruptLifetimesCannotLookComplete() {
    MeasurementWorkload ledger(1);
    ledger.Open(5, 100);
    ledger.Boundary(6, 200, false);
    ledger.End(Kind::Draw, Outcome::Complete);
    ledger.Boundary(8, 300, true);
    const auto saved = ledger.Collect();
    Require(saved.frames.size() == 1 && saved.lost == 1 && saved.lifetimeError, "lost interval and unmatched end remain explicit");
    bool rejected = false;
    try { MeasurementWorkload oversized(MeasurementWorkload::MaximumIntervals + 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "bounded allocation rejects oversized window");
    auto counter = std::numeric_limits<std::uint64_t>::max();
    Require(!MeasurementWorkload::Increment(counter) && counter == std::numeric_limits<std::uint64_t>::max(), "counter saturation never wraps to zero");
}

void OutcomesAndIndirectRecordsStaySeparate() {
    MeasurementWorkload ledger(1);
    ledger.Open(0, 100);
    ledger.Begin(Kind::Draw);
    ledger.Begin(Kind::IndirectRecord);
    ledger.End(Kind::IndirectRecord, Outcome::Skipped);
    ledger.End(Kind::Draw, Outcome::Rejected);
    ledger.Begin(Kind::HostDispatch);
    ledger.End(Kind::HostDispatch, Outcome::Failed);
    ledger.Begin(Kind::Job);
    ledger.End(Kind::Job, Outcome::Cancelled);
    ledger.Boundary(1, 200, true);
    const auto saved = ledger.Collect();
    Require(saved.counts[0] == 1 && saved.counts[2] == 1 && saved.counts[6] == 1 && saved.counts[7] == 1, "indirect records do not inflate requested packet count");
    Require(saved.counts[11] == 1 && saved.counts[14] == 1 && saved.counts[10] == 0, "failed emission and cancelled job are not successful zero work");
}

int main() {
    try {
        CompleteWindowRetainsPerPresentCounts();
        CrossingWorkRemainsVisible();
        DeferredJobsPreserveQueueLifetime();
        ConcurrentObservationsAreNotLost();
        CapacityAndCorruptLifetimesCannotLookComplete();
        OutcomesAndIndirectRecordsStaySeparate();
        std::cout << "6 measurement workload cases passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

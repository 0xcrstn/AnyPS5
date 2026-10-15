#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitProof.hpp"
#include "prx/libSceAgcDriver/Execution/include/WaitBroker.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using namespace AgcDriver::DriverDetail;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

template<typename TAction>
bool Throws(TAction action) {
    try {
        action();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

ProofInput Recorded(WaitSource source, std::uint64_t stamp, std::uint64_t activation) {
    ProofInput input;
    input.source = source;
    input.function = 3;
    input.reference = 0xb9a;
    input.mask = ~0ull;
    input.waitBytes = 8;
    input.value = 0xb9a;
    input.storeBytes = 8;
    input.waitingQueue = 0x2b;
    input.producerQueue = 0;
    input.producerStamp = stamp;
    input.activation = activation;
    return input;
}

void TestPredicates() {
    Expect(CompareMasked(0, 5, 9, 0), "always passes");
    Expect(CompareMasked(1, 4, 5, ~0ull) && !CompareMasked(1, 5, 5, ~0ull), "less than");
    Expect(CompareMasked(2, 5, 5, ~0ull) && !CompareMasked(2, 6, 5, ~0ull), "less or equal");
    Expect(CompareMasked(3, 0x1234, 0x34, 0xff) && !CompareMasked(3, 0x1235, 0x34, 0xff), "masked equal compares the masked bits only");
    Expect(CompareMasked(4, 0x100, 0, 0xff) == false, "not equal over a mask that hides the difference");
    Expect(CompareMasked(5, 7, 7, ~0ull) && CompareMasked(6, 8, 7, ~0ull) && !CompareMasked(6, 7, 7, ~0ull), "greater comparisons");
    Expect(CompareMasked(3, 0xffffffff00000001ull, 1, 0xffffffffull), "a 64-bit wait with a low mask ignores the high dword");
    Expect(CompareMasked(3, 0, 0, 0), "a zero mask equals a zero reference");
    Expect(Throws([] { CompareMasked(7, 1, 1, ~0ull); }), "the reserved compare function throws");
}

void TestProofMatrix() {
    Expect(Prove(Recorded(WaitSource::History, 11, 10)).lift == Lift::Producer, "a store recorded after activation proves a producer");
    Expect(Prove(Recorded(WaitSource::Table, 11, 10)).lift == Lift::Producer, "a table entry recorded after activation proves a producer");
    Expect(Prove(Recorded(WaitSource::History, 9, 10)).lift == Lift::Exact, "a store overwritten before activation proves nothing");
    Expect(Prove(Recorded(WaitSource::History, 10, 10)).lift == Lift::Exact, "a store at the activation stamp proves nothing");
    Expect(Prove(Recorded(WaitSource::TableLate, 9, 10)).lift == Lift::Authoritative, "a late-rule entry is the authoritative value");
    auto partial = Recorded(WaitSource::History, 11, 10);
    partial.storeBytes = 4;
    Expect(Prove(partial).lift == Lift::Exact, "a store over part of a 64-bit wait proves nothing");
    auto mismatch = Recorded(WaitSource::History, 11, 10);
    mismatch.value = 0xb99;
    Expect(Prove(mismatch).lift == Lift::Exact, "a value that fails the predicate proves nothing");
    auto memory = Recorded(WaitSource::Memory, 0, 10);
    Expect(Prove(memory).lift == Lift::Latched, "a predicate seen in memory latches");
    auto reserved = Recorded(WaitSource::History, 11, 10);
    reserved.function = 7;
    Expect(Throws([&] { Prove(reserved); }), "a wait with the reserved compare function throws");
    auto width = Recorded(WaitSource::History, 11, 10);
    width.waitBytes = 2;
    Expect(Throws([&] { Prove(width); }), "a wait of unknown width throws");
    Expect(Throws([] { Prove(ProofInput{}); }), "a proof without an observation source throws");
}

void TestObservationFloor() {
    WaitObservation stale;
    stale.source = WaitSource::History;
    stale.received = 5;
    stale.producerQueue = 0;
    stale.queue = 0x20;
    stale.producerStamp = 7;
    Expect(ObservationProven(stale, 0), "a store after receipt is proven without an earlier discharge");
    Expect(!ObservationProven(stale, 9), "a store before the queue's last discharge is not proven");
    Expect(WaitActivationStamp(WaitFloorMode::Activation, 5, 9) == 9 && WaitActivationStamp(WaitFloorMode::Submission, 5, 9) == 5, "activation stamps by mode");
}

void TestModes() {
    Expect(ParseLabelTimeline(nullptr) == LabelTimelineMode::On && ParseLabelTimeline("off") == LabelTimelineMode::Off && ParseLabelTimeline("verify") == LabelTimelineMode::Verify, "label timeline modes parse");
    Expect(Throws([] { ParseLabelTimeline("1"); }), "an unknown label timeline mode throws");
}

void TestBroker() {
    AgcDriver::WaitBroker broker;
    const auto seen = broker.Sequence();
    broker.Publish();
    const auto start = std::chrono::steady_clock::now();
    Expect(broker.WaitFor(seen, std::chrono::seconds(5)), "a publish before the wait was lost");
    Expect(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "a publish before the wait did not return at once");
    const auto quiet = broker.Sequence();
    Expect(!broker.WaitFor(quiet, std::chrono::milliseconds(5)), "a wait with no publish did not time out");
    std::atomic<bool> woken{false};
    const auto before = broker.Sequence();
    std::thread waiter([&] { woken = broker.WaitFor(before, std::chrono::seconds(5)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    broker.Publish();
    waiter.join();
    Expect(woken.load(), "a parked waiter missed a publish");
}

}

int main() {
    TestPredicates();
    TestProofMatrix();
    TestObservationFloor();
    TestModes();
    TestBroker();
    if (failures != 0) return 1;
    std::puts("wait proof tests passed");
    return 0;
}

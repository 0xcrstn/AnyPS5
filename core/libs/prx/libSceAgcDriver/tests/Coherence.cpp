#include "prx/libSceAgcDriver/Execution/include/Coherence.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace AgcDriver::Coherence;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
std::string Throws(const TAction& action) {
    try {
        action();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

void Reset(Mode audit, Mode ordering, Mode directory = Mode::Reference) {
    SetModesForTests(audit, ordering, directory);
    ResetForTests();
}

constexpr std::uint64_t AliasA = 0x100000;
constexpr std::uint64_t AliasB = 0x200000;
constexpr std::uint64_t AliasBytes = 0x10000;
constexpr std::uint64_t BackingOld = (std::uint64_t{1} << 63) | 0x40000000;
constexpr std::uint64_t BackingNew = (std::uint64_t{1} << 63) | 0x50000000;
bool remappedA = false;
bool brokenTranslator = false;

std::size_t FakeBacking(std::uintptr_t address, std::size_t bytes, std::uintptr_t* begins, std::uintptr_t* ends, std::uint64_t* canonicals, std::size_t capacity) {
    std::vector<BackingRun> runs;
    const auto stop = address + bytes;
    for (auto cursor = address; cursor < stop;) {
        const auto inA = cursor >= AliasA && cursor < AliasA + AliasBytes;
        const auto inB = cursor >= AliasB && cursor < AliasB + AliasBytes;
        std::uint64_t end = stop;
        std::uint64_t canonical = cursor;
        if (inA || inB) {
            const auto base = inA ? AliasA : AliasB;
            end = std::min<std::uint64_t>(stop, base + AliasBytes);
            canonical = (inA && remappedA ? BackingNew : BackingOld) + (cursor - base);
        } else if (cursor < AliasA && stop > AliasA) {
            end = AliasA;
        } else if (cursor < AliasB && stop > AliasB) {
            end = AliasB;
        }
        runs.push_back({cursor, end, canonical});
        cursor = end;
    }
    if (brokenTranslator && !runs.empty()) runs.front().begin += 1;
    for (std::size_t index = 0; index < runs.size() && index < capacity; ++index) {
        begins[index] = runs[index].begin;
        ends[index] = runs[index].end;
        canonicals[index] = runs[index].canonical;
    }
    return runs.size();
}

void ModeTests() {
    Require(ParseMode("X", nullptr) == Mode::Reference, "an unset switch is not the reference");
    Require(ParseMode("X", "0") == Mode::Reference, "0 is not the reference");
    Require(ParseMode("X", "1") == Mode::New, "1 is not the new path");
    Require(ParseMode("X", "verify") == Mode::Verify, "verify is not the verify mode");
    Require(Throws([] { static_cast<void>(ParseMode("X", "2")); }).find("expected 0, 1 or verify") != std::string::npos, "an unsupported value was accepted");
    Reset(Mode::Reference, Mode::Reference);
    Require(!Tracking() && !Ordering(), "the reference modes track or order");
    Reset(Mode::Reference, Mode::New);
    Require(!Tracking() && Ordering(), "ordering 1 alone must order without the version map");
    Reset(Mode::Reference, Mode::Verify);
    Require(Tracking() && Ordering(), "ordering verify must order and track");
    Reset(Mode::New, Mode::Reference);
    Require(Tracking() && !Ordering(), "audit 1 must track on the reference path");
}

void ReferenceTests() {
    Reset(Mode::Reference, Mode::Reference);
    const std::vector<Interval> heap{{0x10000, 0x90000}};
    auto token = Reserve(Producer::PointerWriter, heap, true, 0);
    Require(!token.Held(), "the reference path reserved a writer");
    Require(CheckPublication(Source::ShadowUnit, 0, {0x20000, 0x30000}, "flip"), "the reference path judged a publication");
    Require(CurrentCounters().publicationsChecked == 0, "the reference path counted a publication");
}

void StalePublicationTests() {
    Reset(Mode::New, Mode::Reference);
    const auto image = NextVersion();
    const std::vector<Interval> heap{{0x10000, 0x90000}};
    auto writer = Reserve(Producer::PointerWriter, heap, true, 0);
    Require(writer.Held() && writer.Id() > image, "a reserved writer does not order after the older image");
    Require(!CheckPublication(Source::ShadowUnit, image, {0x20000, 0x30000}, "flip"), "an older shadow publishing over a reserved writer was not flagged");
    writer.Recorded();
    Require(!CheckPublication(Source::PendingImage, image, {0x8f000, 0x91000}, "flip"), "an older image publishing over a recorded writer was not flagged");
    Require(CheckPublication(Source::PendingImage, image, {0x90000, 0xa0000}, "flip"), "a publication beside the writer was flagged");
    const auto later = NextVersion();
    Require(CheckPublication(Source::ShadowUnit, later, {0x20000, 0x30000}, "flip"), "a newer representation was flagged");
    const auto conflict = NewerWriter({0x20000, 0x30000}, image);
    Require(conflict && conflict->version == writer.Id() && conflict->recorded && conflict->conservative && !conflict->completed, "the conflicting writer is misreported");
    const std::vector<Interval> written{{0x20000, 0x21000}, {0x8f000, 0x95000}};
    writer.Complete(written);
    Require(!writer.Held(), "a completed token is still held");
    Require(CheckPublication(Source::ShadowUnit, image, {0x40000, 0x50000}, "flip"), "the conservative remainder outlived the completion");
    Require(!CheckPublication(Source::ShadowUnit, image, {0x20800, 0x20900}, "flip"), "a written interval was forgotten at completion");
    Require(CheckPublication(Source::ShadowUnit, image, {0x90000, 0x95000}, "flip"), "a written interval outside the reservation was committed");
    const auto counters = CurrentCounters();
    Require(counters.stalePublications == 3 && counters.publicationsChecked == 7 && counters.reservations == 1 && counters.completions == 1 && counters.activeWriters == 0, "the counters disagree: " + Report());
}

void RecordOrderTests() {
    Reset(Mode::New, Mode::Reference);
    auto writer = Reserve(Producer::PointerWriter, std::vector<Interval>{{0x1000, 0x9000}}, true, 0);
    const auto reserved = writer.Id();
    const auto between = NextVersion();
    Require(between > reserved, "a version drawn after a reservation is not newer");
    Require(CheckPublication(Source::PendingImage, between, {0x2000, 0x3000}, "flip"), "a result newer than a reservation that is not yet recorded was flagged");
    writer.Recorded();
    Require(writer.Id() > between, "recording did not order the writer after work recorded before it");
    Require(!CheckPublication(Source::PendingImage, between, {0x2000, 0x3000}, "flip"), "a result recorded before the writer published over it without a flag");
    writer.Recorded();
    Require(writer.Held(), "a second Recorded dropped the token");
}

void DirectoryTests() {
    SetBackingTranslator(&FakeBacking);
    remappedA = false;
    brokenTranslator = false;
    Reset(Mode::New, Mode::Reference, Mode::Reference);
    const auto image = NextVersion();
    {
        auto writer = Reserve(Producer::PointerWriter, std::vector<Interval>{{AliasA + 64, AliasA + 96}}, false, 0);
        writer.Recorded();
        Require(CheckPublication(Source::PendingImage, image, {AliasB + 64, AliasB + 96}, "hook"), "without the directory an alias was already canonical");
    }
    Reset(Mode::New, Mode::Reference, Mode::New);
    const auto older = NextVersion();
    const auto translated = Translate({AliasA - 0x100, AliasA + 0x100});
    Require(translated.size() == 2 && translated[0].canonical == AliasA - 0x100 && translated[1].canonical == BackingOld, "the directory did not split a range at its backing boundary");
    auto writer = Reserve(Producer::PointerWriter, std::vector<Interval>{{AliasA + 64, AliasA + 96}}, false, 0);
    writer.Recorded();
    Require(!CheckPublication(Source::PendingImage, older, {AliasB + 64, AliasB + 96}, "hook"), "an old image published through alias B over a write recorded through alias A was not flagged");
    Require(CheckPublication(Source::PendingImage, older, {AliasB + 96, AliasB + 128}, "hook"), "bytes beside the write were flagged through the alias");
    remappedA = true;
    writer.Complete(std::vector<Interval>{{AliasA + 64, AliasA + 96}});
    Require(CheckPublication(Source::PendingImage, older, {AliasA + 64, AliasA + 96}, "hook"), "a completion after A was remapped modified the replacement backing");
    Require(!CheckPublication(Source::PendingImage, older, {AliasB + 64, AliasB + 96}, "hook"), "a completion after A was remapped lost the write on the old backing");
    Require(CurrentCounters().remappedCompletions == 0, "directory 1 checked the remap");

    remappedA = false;
    Reset(Mode::New, Mode::Reference, Mode::Verify);
    auto verified = Reserve(Producer::PointerWriter, std::vector<Interval>{{AliasA, AliasA + 0x100}}, true, 0);
    verified.Recorded();
    remappedA = true;
    const auto remapped = Throws([&] { verified.Complete(std::vector<Interval>{{AliasA, AliasA + 4}}); });
    Require(remapped.find("mapped to another backing") != std::string::npos, "directory verify accepted a completion whose mapping changed: " + remapped);
    remappedA = false;
    brokenTranslator = true;
    Require(Throws([] { static_cast<void>(Translate({AliasA, AliasA + 16})); }).find("do not tile") != std::string::npos, "a translator that does not tile its range was accepted");
    brokenTranslator = false;
    SetBackingTranslator(nullptr);
    Require(!Throws([] { static_cast<void>(Translate({AliasA, AliasA + 16})); }).empty(), "the directory ran without a translator");
    Reset(Mode::Reference, Mode::Reference);
    Require(Translate({AliasA, AliasA + 16}).front().canonical == AliasA, "the reference directory is not the identity");
}

void ReleaseTests() {
    Reset(Mode::New, Mode::Reference);
    auto first = Reserve(Producer::PointerWriter, std::vector<Interval>{{0x1000, 0x2000}}, true, 3);
    auto second = Reserve(Producer::ElementWriter, std::vector<Interval>{{0x3000, 0x3100}}, false, 5);
    first.Recorded();
    auto unrecorded = Reserve(Producer::ElementWriter, std::vector<Interval>{{0x4000, 0x4100}}, false, 3);
    const auto release = SnapshotRelease(3, {0x8000, 0x8008}, 7, 42);
    Require(release.queue == 3 && release.value == 7 && release.labelStamp == 42 && release.version > first.Id(), "the release does not describe its label");
    Require(release.frontier.size() == 1 && release.frontier[0].first == 3 && release.frontier[0].second == first.Id(), "the release frontier is not the newest recorded writer per queue");
    second.Recorded();
    Require(QueueFrontier(9).empty(), "a queue imported a frontier it never waited for");
    ImportFrontier(9, release);
    auto frontier = QueueFrontier(9);
    Require(frontier.size() == 1 && frontier[0].second == first.Id(), "the consumer did not import the release frontier");
    const auto later = SnapshotRelease(5, {0x8000, 0x8008}, 8, 43);
    ImportFrontier(9, later);
    ImportFrontier(9, release);
    frontier = QueueFrontier(9);
    Require(frontier.size() == 2 && frontier[0].second == first.Id() && frontier[1].second == second.Id(), "an older release moved an imported frontier back");
    Require(Throws([] { CheckDrained(false, false, "label", false); }).empty(), "audit 1 failed a release ahead of owed write-backs");
    Require(CurrentCounters().undrainedReleases == 1, "a release ahead of owed write-backs was not counted");
    Reset(Mode::Reference, Mode::Verify);
    Require(Throws([] { CheckDrained(false, false, "label", false); }).find("write-backs still owed") != std::string::npos, "ordering verify accepted a release ahead of owed write-backs");
}

void LifetimeTests() {
    Reset(Mode::New, Mode::Reference);
    const auto before = NextVersion();
    const std::vector<Interval> range{{0x1000, 0x2000}};
    {
        auto abandoned = Reserve(Producer::ElementWriter, range, false, 0);
        Require(abandoned.Held(), "the element writer was not reserved");
    }
    Require(CheckPublication(Source::PendingImage, before, {0x1000, 0x2000}, "hook"), "an abandoned writer still conflicts");
    {
        auto recorded = Reserve(Producer::ElementWriter, range, false, 0);
        recorded.Recorded();
        auto moved = std::move(recorded);
        Require(!recorded.Held() && moved.Held(), "a moved token kept its reservation");
    }
    Require(!CheckPublication(Source::PendingImage, before, {0x1800, 0x1900}, "hook"), "a recorded writer destroyed without completion was not committed");
    const auto counters = CurrentCounters();
    Require(counters.abandoned == 1 && counters.completions == 1, "the lifetime counters disagree: " + Report());
    auto twice = Reserve(Producer::ElementWriter, range, false, 0);
    twice.Complete(range);
    twice.Complete(range);
    Require(!twice.Held(), "a second completion revived the token");
}

void CommitSplitTests() {
    Reset(Mode::New, Mode::Reference);
    const auto base = NextVersion();
    auto outer = Reserve(Producer::ElementWriter, std::vector<Interval>{{0, 100}}, false, 0);
    outer.Complete(std::vector<Interval>{{0, 100}});
    auto inner = Reserve(Producer::ElementWriter, std::vector<Interval>{{40, 60}}, false, 0);
    const auto innerId = inner.Id();
    inner.Complete(std::vector<Interval>{{40, 60}});
    const auto newest = NewerWriter({0, 100}, base);
    Require(newest && newest->version == innerId && newest->range.begin == 40 && newest->range.end == 60, "the newest writer of a split interval is wrong");
    const auto tail = NewerWriter({60, 100}, innerId - 1);
    Require(!tail, "the tail of the outer write took the inner version");
    const auto head = NewerWriter({0, 40}, base);
    Require(head && head->version != innerId && head->range.end == 40, "the head of the outer write was lost");
}

void EvictionTests() {
    Reset(Mode::New, Mode::Reference);
    for (std::uint64_t index = 0; index < 70000; ++index) {
        auto token = Reserve(Producer::ElementWriter, std::vector<Interval>{{index * 16, index * 16 + 8}}, false, 0);
        token.Complete(std::vector<Interval>{{index * 16, index * 16 + 8}});
    }
    const auto counters = CurrentCounters();
    Require(counters.evictedIntervals != 0 && counters.committedIntervals <= 65536, "the committed map is unbounded: " + Report());
    Require(NewerWriter({69999 * 16, 69999 * 16 + 8}, 0).has_value(), "eviction dropped the newest interval");
}

void VerifyTests() {
    Reset(Mode::Verify, Mode::Reference);
    const auto shadow = NextVersion();
    auto writer = Reserve(Producer::PointerWriter, std::vector<Interval>{{0x10000, 0x20000}}, true, 3);
    writer.Recorded();
    const auto failure = Throws([&] { static_cast<void>(CheckPublication(Source::ShadowUnit, shadow, {0x10000, 0x11000}, "flip")); });
    Require(failure.find("stale publication: shadow-unit") != std::string::npos, "audit verify did not fail on a stale publication: " + failure);
    Require(failure.find("reserve pointer-writer") != std::string::npos && failure.find("recorded pointer-writer") != std::string::npos, "the failure lacks the writer's provenance: " + failure);
    Require(Throws([&] { static_cast<void>(CheckPublication(Source::ShadowUnit, writer.Id(), {0x10000, 0x11000}, "flip")); }).empty(), "audit verify failed a current publication");

    Reset(Mode::New, Mode::Reference);
    Require(Throws([] { static_cast<void>(CheckRead(Source::PendingImage, 7, {0x1000, 0x2000}, "address-based heap")); }).empty(), "audit 1 failed a stale read");
    Require(CurrentCounters().staleReads == 1, "audit 1 did not count a stale read");
    Require(Throws([] { CheckDrained(true, false, "label"); }).empty(), "audit 1 failed an undrained release");

    Reset(Mode::Reference, Mode::Verify);
    Require(Throws([] { static_cast<void>(CheckRead(Source::PendingImage, 7, {0x1000, 0x2000}, "address-based heap")); }).find("stale read") != std::string::npos, "ordering verify did not fail on a stale read");
    Require(Throws([] { CheckDrained(false, true, "label"); }).find("release at label") != std::string::npos, "ordering verify did not fail on an undrained release");
    Require(Throws([] { CheckDrained(false, false, "label"); }).empty(), "ordering verify failed a drained release");
}

void InjectedMismatchTests() {
    Reset(Mode::Verify, Mode::Reference);
    const auto image = NextVersion();
    const auto retile = image;
    auto pointer = Reserve(Producer::PointerWriter, std::vector<Interval>{{0x40000, 0x80000}}, true, 0);
    pointer.Recorded();
    const auto injected = Throws([&] { static_cast<void>(CheckPublication(Source::ShadowUnit, retile, {0x50000, 0x60000}, "flip")); });
    Require(!injected.empty(), "verify mode accepted an injected publication of an old shadow over a recorded pointer writer");
    pointer.Complete(std::vector<Interval>{{0x50000, 0x50100}});
    Require(!Throws([&] { static_cast<void>(CheckPublication(Source::PendingImage, image, {0x50000, 0x60000}, "hook")); }).empty(), "verify mode accepted an injected write-back over a completed pointer store");
    Require(Throws([&] { static_cast<void>(CheckPublication(Source::PendingImage, image, {0x60000, 0x70000}, "hook")); }).empty(), "verify mode failed a write-back the pointer writer never reached");
}

}

int main() {
    try {
        ModeTests();
        ReferenceTests();
        StalePublicationTests();
        RecordOrderTests();
        DirectoryTests();
        ReleaseTests();
        LifetimeTests();
        CommitSplitTests();
        EvictionTests();
        VerifyTests();
        InjectedMismatchTests();
        std::cout << "coherence directory tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "coherence directory tests failed: " << error.what() << '\n';
        return 1;
    }
}

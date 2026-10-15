#include "prx/libSceAgcDriver/Execution/include/Coherence.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace AgcDriver::Coherence {

namespace {

constexpr std::size_t RingEvents = 4096;
constexpr std::size_t MaxCommitted = 65536;
constexpr std::size_t ReportedDivergences = 16;
constexpr std::size_t TraceEvents = 24;
constexpr std::uint64_t ProtectionGranule = 4096;

enum class EventType : std::uint8_t { Reserve, Recorded, Complete, Commit, Abandon, Publish, StalePublish, StaleRead, Undrained, Remapped };

constexpr const char* EventNames[] = {"reserve", "recorded", "complete", "commit", "abandon", "publish", "stale-publish", "stale-read", "undrained", "remapped"};
constexpr const char* ProducerNames[] = {"pointer-writer", "element-writer", "label-store"};
constexpr const char* SourceNames[] = {"pending-image", "shadow-unit", "resident-staging", "read-lease"};

struct Event {
    std::uint64_t serial = 0;
    EventType type = EventType::Reserve;
    std::uint8_t kind = 0;
    std::uint32_t queue = 0;
    Version version = 0;
    Version other = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    const char* reason = nullptr;
};

struct Active {
    Producer kind = Producer::PointerWriter;
    std::vector<Interval> intervals;
    std::vector<BackingRun> runs;
    bool conservative = false;
    bool recorded = false;
    std::uint32_t queue = 0;
};

struct Committed {
    std::uint64_t end = 0;
    Version version = 0;
    Producer kind = Producer::PointerWriter;
};

struct State {
    std::mutex mutex;
    std::map<Version, Active> active;
    std::map<std::uint64_t, Committed> committed;
    std::array<Event, RingEvents> ring{};
    std::uint64_t events = 0;
    Counters counters;
    std::size_t publicationsReported = 0;
    std::size_t readsReported = 0;
    std::size_t releasesReported = 0;
    std::size_t remapsReported = 0;
    std::size_t uncoveredReported = 0;
    std::map<std::uint32_t, Version> recordedByQueue;
    std::map<std::uint32_t, std::map<std::uint32_t, Version>> imported;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

State& Directory() {
    static State state;
    return state;
}

std::atomic<Version> versions{0};
std::atomic<BackingTranslator> translator{nullptr};

struct Modes {
    Mode audit;
    Mode ordering;
    Mode directory;
};

std::atomic<bool> overridden{false};
Modes overrideModes{Mode::Reference, Mode::Reference, Mode::Reference};
std::atomic<bool> faultsOverridden{false};
std::atomic<Mode> overrideFaults{Mode::Reference};

struct Protection {
    std::uint64_t token = 0;
    Interval range;
    Source source = Source::PendingImage;
    Version version = 0;
};

struct Ledger {
    std::mutex mutex;
    std::map<std::uint64_t, Protection> live;
    std::uint64_t next = 0;
    ProtectionBackend backend;
    Materializer materializer = nullptr;
    Materializer waiter = nullptr;
    std::uint64_t protections = 0;
    std::uint64_t leases = 0;
    std::uint64_t leaseWaits = 0;
    std::uint64_t refused = 0;
    std::uint64_t partial = 0;
    std::uint64_t resolved = 0;
    std::uint64_t locked = 0;
};

Ledger& Protections() {
    static Ledger ledger;
    return ledger;
}

struct ResolverContext {
    std::uint64_t protection;
};

[[noreturn]] void AbortInFault(const char* what, Interval range, const char* detail) {
    std::fprintf(stderr, "[coherence] %s [0x%llx, 0x%llx): %s\n", what, static_cast<unsigned long long>(range.begin), static_cast<unsigned long long>(range.end), detail);
    std::fflush(stderr);
    std::abort();
}

bool ResolveProtection(std::uint64_t token, void* context) {
    const auto id = static_cast<ResolverContext*>(context)->protection;
    auto& ledger = Protections();
    Interval range;
    Materializer materializer = nullptr;
    ProtectionBackend backend;
    bool live = false;
    bool lease = false;
    {
        std::lock_guard lock(ledger.mutex);
        backend = ledger.backend;
        const auto found = ledger.live.find(id);
        if (found != ledger.live.end()) {
            live = true;
            range = found->second.range;
            lease = found->second.source == Source::ReadLease;
            materializer = lease ? ledger.waiter : ledger.materializer;
        }
    }
    if (live) {
        try {
            if (materializer == nullptr) throw std::runtime_error(lease ? "no lease waiter is installed" : "no materializer is installed");
            materializer(range);
        } catch (const std::exception& error) {
            AbortInFault(lease ? "a native access to guest memory under a read lease could not wait for its readers" : "native access to device-owned guest memory could not be materialized", range, error.what());
        }
        std::lock_guard lock(ledger.mutex);
        ledger.live.erase(id);
        ++(lease ? ledger.leaseWaits : ledger.resolved);
    }
    if (backend.release != nullptr) backend.release(token);
    return true;
}

void ReleaseResolverContext(void* context) {
    delete static_cast<ResolverContext*>(context);
}

Mode EnvironmentFaults() {
    static const Mode faults = ParseMode("APS5_COHERENCE_FAULTS", std::getenv("APS5_COHERENCE_FAULTS"));
    return faults;
}

Mode EnvironmentLeases() {
    static const Mode leases = ParseMode("APS5_COHERENCE_INPUT_LEASES", std::getenv("APS5_COHERENCE_INPUT_LEASES"));
    return leases;
}

std::atomic<bool> leasesOverridden{false};
std::atomic<Mode> overrideLeases{Mode::Reference};
std::atomic<bool> residencyOverridden{false};
std::atomic<Mode> overrideResidency{Mode::Reference};
std::atomic<bool> releaseOverridden{false};
std::atomic<Mode> overrideRelease{Mode::Reference};

Mode EnvironmentRelease() {
    static const Mode release = ParseMode("APS5_COHERENCE_LAZY_RELEASE", std::getenv("APS5_COHERENCE_LAZY_RELEASE"));
    return release;
}

Mode EnvironmentResidency() {
    static const Mode residency = ParseMode("APS5_COHERENCE_RESIDENCY", std::getenv("APS5_COHERENCE_RESIDENCY"));
    return residency;
}

std::uint64_t Guard(Interval range, Source source, Version version, bool outward, bool verify, const char* variable) {
    const auto begin = outward ? range.begin & ~(ProtectionGranule - 1) : (range.begin + ProtectionGranule - 1) & ~(ProtectionGranule - 1);
    const auto end = outward ? (range.end + ProtectionGranule - 1) & ~(ProtectionGranule - 1) : range.end & ~(ProtectionGranule - 1);
    const bool whole = begin == range.begin && end == range.end;
    auto& ledger = Protections();
    ProtectionBackend backend;
    std::uint64_t id = 0;
    {
        std::lock_guard lock(ledger.mutex);
        backend = ledger.backend;
        if (backend.protect == nullptr || backend.release == nullptr) throw std::runtime_error(std::string(variable) + ": no guest access protection backend is installed");
        if (!whole && !outward) ++ledger.partial;
    }
    if (verify && !whole && !outward) {
        char text[192];
        std::snprintf(text, sizeof(text), "%s=verify: device-owned guest memory [0x%llx, 0x%llx) does not fill whole protection pages", variable, static_cast<unsigned long long>(range.begin), static_cast<unsigned long long>(range.end));
        throw std::runtime_error(text);
    }
    if (begin >= end) return 0;
    {
        std::lock_guard lock(ledger.mutex);
        id = ++ledger.next;
        ledger.live.emplace(id, Protection{0, {begin, end}, source, version});
    }
    auto context = std::make_unique<ResolverContext>(ResolverContext{id});
    const auto token = backend.protect(static_cast<std::uintptr_t>(begin), static_cast<std::size_t>(end - begin), &ResolveProtection, context.get(), &ReleaseResolverContext);
    if (token != 0) static_cast<void>(context.release());
    std::unique_lock lock(ledger.mutex);
    if (token == 0) {
        ledger.live.erase(id);
        ++ledger.refused;
        lock.unlock();
        if (!verify) return 0;
        char text[192];
        std::snprintf(text, sizeof(text), "%s=verify: guest memory [0x%llx, 0x%llx) refused access protection", variable, static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end));
        throw std::runtime_error(text);
    }
    ++(source == Source::ReadLease ? ledger.leases : ledger.protections);
    const auto found = ledger.live.find(id);
    if (found == ledger.live.end()) return 0;
    found->second.token = token;
    return id;
}

const Modes& EnvironmentModes() {
    static const Modes modes{ParseMode("APS5_COHERENCE_AUDIT", std::getenv("APS5_COHERENCE_AUDIT")), ParseMode("APS5_COHERENCE_ORDERING", std::getenv("APS5_COHERENCE_ORDERING")), ParseMode("APS5_COHERENCE_DIRECTORY", std::getenv("APS5_COHERENCE_DIRECTORY"))};
    return modes;
}

const Modes& CurrentModes() {
    return overridden.load(std::memory_order_acquire) ? overrideModes : EnvironmentModes();
}

bool Overlaps(const Interval& left, const Interval& right) {
    return left.begin < right.end && right.begin < left.end;
}

Interval Clip(const Interval& left, const Interval& right) {
    return {std::max(left.begin, right.begin), std::min(left.end, right.end)};
}

std::vector<Interval> Spans(std::span<const BackingRun> runs) {
    std::vector<Interval> spans;
    for (const auto& run : runs) {
        const Interval span{run.canonical, run.canonical + (run.end - run.begin)};
        if (!spans.empty() && spans.back().end == span.begin) spans.back().end = span.end;
        else spans.push_back(span);
    }
    return spans;
}

void Note(State& state, EventType type, std::uint8_t kind, std::uint32_t queue, Version version, Version other, Interval range, const char* reason) {
    auto& event = state.ring[state.events % RingEvents];
    event = {state.events, type, kind, queue, version, other, range.begin, range.end, reason};
    ++state.events;
}

std::string Format(const Event& event) {
    char text[256];
    const bool producer = event.type == EventType::Reserve || event.type == EventType::Recorded || event.type == EventType::Complete || event.type == EventType::Commit || event.type == EventType::Abandon || event.type == EventType::Remapped;
    const char* kind = producer ? ProducerNames[event.kind] : event.type == EventType::Undrained ? "release" : SourceNames[event.kind];
    std::snprintf(text, sizeof(text), "#%llu %s %s v%llu [0x%llx, 0x%llx) other v%llu queue %u%s%s", static_cast<unsigned long long>(event.serial), EventNames[static_cast<std::size_t>(event.type)], kind, static_cast<unsigned long long>(event.version), static_cast<unsigned long long>(event.begin), static_cast<unsigned long long>(event.end), static_cast<unsigned long long>(event.other), event.queue, event.reason != nullptr ? " for " : "", event.reason != nullptr ? event.reason : "");
    return text;
}

std::vector<std::string> Recent(const State& state, std::span<const Interval> ranges, std::size_t limit) {
    std::vector<std::string> lines;
    const auto held = std::min<std::uint64_t>(state.events, RingEvents);
    for (std::uint64_t back = 0; back < held && lines.size() < limit; ++back) {
        const auto& event = state.ring[(state.events - 1 - back) % RingEvents];
        const Interval range{event.begin, event.end};
        if (std::any_of(ranges.begin(), ranges.end(), [&](const Interval& wanted) { return Overlaps(range, wanted); })) lines.push_back(Format(event));
    }
    std::reverse(lines.begin(), lines.end());
    return lines;
}

void Commit(State& state, Interval range, Version version, Producer kind) {
    if (range.end <= range.begin) return;
    auto& committed = state.committed;
    auto it = committed.lower_bound(range.begin);
    if (it != committed.begin()) {
        auto previous = std::prev(it);
        if (previous->second.end > range.begin) it = previous;
    }
    std::vector<std::pair<std::uint64_t, Committed>> kept;
    while (it != committed.end() && it->first < range.end) {
        if (it->first < range.begin) kept.emplace_back(it->first, Committed{range.begin, it->second.version, it->second.kind});
        if (it->second.end > range.end) kept.emplace_back(range.end, Committed{it->second.end, it->second.version, it->second.kind});
        it = committed.erase(it);
    }
    for (const auto& [begin, entry] : kept) committed.emplace(begin, entry);
    committed.emplace(range.begin, Committed{range.end, version, kind});
    if (committed.size() <= MaxCommitted) return;
    std::vector<Version> ages;
    ages.reserve(committed.size());
    for (const auto& [begin, entry] : committed) ages.push_back(entry.version);
    const auto cut = ages.begin() + static_cast<std::ptrdiff_t>(ages.size() / 4);
    std::nth_element(ages.begin(), cut, ages.end());
    const auto oldest = *cut;
    for (auto entry = committed.begin(); entry != committed.end();) {
        if (entry->second.version < oldest) {
            entry = committed.erase(entry);
            ++state.counters.evictedIntervals;
        } else {
            ++entry;
        }
    }
}

std::optional<Writer> Newer(const State& state, Interval destination, Version source) {
    std::optional<Writer> newest;
    const auto consider = [&](const Writer& writer) {
        if (!newest || writer.version > newest->version) newest = writer;
    };
    for (auto it = state.active.upper_bound(source); it != state.active.end(); ++it) {
        for (const auto& interval : it->second.intervals) {
            if (!Overlaps(interval, destination)) continue;
            consider({Clip(interval, destination), it->first, it->second.kind, it->second.conservative, it->second.recorded, false});
            break;
        }
    }
    auto it = state.committed.lower_bound(destination.begin);
    if (it != state.committed.begin()) {
        auto previous = std::prev(it);
        if (previous->second.end > destination.begin) it = previous;
    }
    for (; it != state.committed.end() && it->first < destination.end; ++it) {
        if (it->second.version <= source) continue;
        consider({Clip({it->first, it->second.end}, destination), it->second.version, it->second.kind, false, true, true});
    }
    return newest;
}

std::optional<Writer> NewerOverSpans(const State& state, std::span<const Interval> spans, Version source) {
    std::optional<Writer> newest;
    for (const auto& span : spans) {
        const auto writer = Newer(state, span, source);
        if (writer && (!newest || writer->version > newest->version)) newest = writer;
    }
    return newest;
}

bool Verifying() {
    const auto& modes = CurrentModes();
    return modes.audit == Mode::Verify || modes.ordering == Mode::Verify;
}

void Divergence(State& state, const std::string& headline, std::span<const Interval> ranges, bool fatal, std::size_t& reported) {
    if (!fatal && reported >= ReportedDivergences) return;
    std::string message = headline;
    if (reported == 0 || fatal) {
        message += "; provenance:";
        for (const auto& line : Recent(state, ranges, TraceEvents)) message += "\n  " + line;
    }
    ++reported;
    if (fatal) throw std::runtime_error(message);
    std::fprintf(stderr, "%s\n", message.c_str());
}

void MaybeReport(State& state) {
    const auto now = std::chrono::steady_clock::now();
    if (now - state.lastReport < std::chrono::seconds(10)) return;
    state.lastReport = now;
    const auto& c = state.counters;
    std::fprintf(stderr, "[coherence] reservations %llu, completions %llu, abandoned %llu, active %zu, publications checked %llu, stale publications %llu, stale reads %llu, undrained releases %llu, remapped completions %llu, committed %zu, evicted %llu\n", static_cast<unsigned long long>(c.reservations), static_cast<unsigned long long>(c.completions), static_cast<unsigned long long>(c.abandoned), state.active.size(), static_cast<unsigned long long>(c.publicationsChecked), static_cast<unsigned long long>(c.stalePublications), static_cast<unsigned long long>(c.staleReads), static_cast<unsigned long long>(c.undrainedReleases), static_cast<unsigned long long>(c.remappedCompletions), state.committed.size(), static_cast<unsigned long long>(c.evictedIntervals));
}

std::vector<BackingRun> MapThrough(std::span<const BackingRun> runs, Interval range) {
    std::vector<BackingRun> mapped;
    for (const auto& run : runs) {
        const auto clipped = Clip({run.begin, run.end}, range);
        if (clipped.end <= clipped.begin) continue;
        mapped.push_back({clipped.begin, clipped.end, run.canonical + (clipped.begin - run.begin)});
    }
    return mapped;
}

bool SameRuns(std::span<const BackingRun> left, std::span<const BackingRun> right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](const BackingRun& a, const BackingRun& b) { return a.begin == b.begin && a.end == b.end && a.canonical == b.canonical; });
}

}

Mode ParseMode(const char* variable, const char* value) {
    if (value == nullptr || std::strcmp(value, "0") == 0) return Mode::Reference;
    if (std::strcmp(value, "1") == 0) return Mode::New;
    if (std::strcmp(value, "verify") == 0) return Mode::Verify;
    throw std::runtime_error(std::string(variable) + "=" + value + ": expected 0, 1 or verify");
}

Mode AuditMode() {
    return CurrentModes().audit;
}

Mode OrderingMode() {
    return CurrentModes().ordering;
}

Mode DirectoryMode() {
    return CurrentModes().directory;
}

Mode FaultsMode() {
    return faultsOverridden.load(std::memory_order_acquire) ? overrideFaults.load(std::memory_order_acquire) : EnvironmentFaults();
}

void SetFaultsModeForTests(Mode faults) {
    overrideFaults.store(faults, std::memory_order_release);
    faultsOverridden.store(true, std::memory_order_release);
}

bool FaultOwnership() {
    return FaultsMode() != Mode::Reference;
}

void SetProtectionBackend(ProtectionBackend backend) {
    auto& ledger = Protections();
    std::lock_guard lock(ledger.mutex);
    ledger.backend = backend;
}

void SetMaterializer(Materializer materializer) {
    auto& ledger = Protections();
    std::lock_guard lock(ledger.mutex);
    ledger.materializer = materializer;
}

std::uint64_t ProtectDeviceOwned(Interval range, Source source, Version version) {
    if (!FaultOwnership() || range.end <= range.begin) return 0;
    if (source == Source::ReadLease) throw std::runtime_error("coherence: a read lease is not device-owned memory");
    return Guard(range, source, version, false, FaultsMode() == Mode::Verify, "APS5_COHERENCE_FAULTS");
}

Mode InputLeasesMode() {
    return leasesOverridden.load(std::memory_order_acquire) ? overrideLeases.load(std::memory_order_acquire) : EnvironmentLeases();
}

void SetInputLeasesModeForTests(Mode leases) {
    overrideLeases.store(leases, std::memory_order_release);
    leasesOverridden.store(true, std::memory_order_release);
}

bool InputLeases() {
    return InputLeasesMode() != Mode::Reference;
}

void SetLeaseWaiter(Materializer waiter) {
    auto& ledger = Protections();
    std::lock_guard lock(ledger.mutex);
    ledger.waiter = waiter;
}

std::uint64_t AcquireReadLease(Interval range, Version version) {
    if (!InputLeases() || range.end <= range.begin) return 0;
    return Guard(range, Source::ReadLease, version, true, InputLeasesMode() == Mode::Verify, "APS5_COHERENCE_INPUT_LEASES");
}

void RetireReadLease(std::uint64_t lease) {
    ReleaseDeviceOwned(lease);
}

Mode ResidencyMode() {
    return residencyOverridden.load(std::memory_order_acquire) ? overrideResidency.load(std::memory_order_acquire) : EnvironmentResidency();
}

void SetResidencyModeForTests(Mode residency) {
    overrideResidency.store(residency, std::memory_order_release);
    residencyOverridden.store(true, std::memory_order_release);
}

bool Residency() {
    return ResidencyMode() != Mode::Reference;
}

Mode LazyReleaseMode() {
    return releaseOverridden.load(std::memory_order_acquire) ? overrideRelease.load(std::memory_order_acquire) : EnvironmentRelease();
}

void SetLazyReleaseModeForTests(Mode release) {
    overrideRelease.store(release, std::memory_order_release);
    releaseOverridden.store(true, std::memory_order_release);
}

bool LazyRelease() {
    return LazyReleaseMode() != Mode::Reference;
}

std::uint64_t ProtectResident(Interval range, Version version) {
    if (!Residency() || range.end <= range.begin) return 0;
    return Guard(range, Source::ResidentStaging, version, false, ResidencyMode() == Mode::Verify, "APS5_COHERENCE_RESIDENCY");
}

void ReleaseDeviceOwned(std::uint64_t protection) {
    if (protection == 0) return;
    auto& ledger = Protections();
    std::uint64_t token = 0;
    ProtectionBackend backend;
    {
        std::lock_guard lock(ledger.mutex);
        const auto found = ledger.live.find(protection);
        if (found == ledger.live.end()) return;
        token = found->second.token;
        backend = ledger.backend;
        ledger.live.erase(found);
    }
    if (token != 0 && backend.release != nullptr) backend.release(token);
}

void NoteLockedFault(Interval range) {
    auto& ledger = Protections();
    {
        std::lock_guard lock(ledger.mutex);
        ++ledger.locked;
    }
    if (FaultsMode() == Mode::Verify) AbortInFault("APS5_COHERENCE_FAULTS=verify: the driver touched device-owned guest memory while holding the GPU mutex", range, "a driver access must resolve its range first");
}

void SetModesForTests(Mode audit, Mode ordering, Mode directory) {
    overrideModes = {audit, ordering, directory};
    overridden.store(true, std::memory_order_release);
}

bool Tracking() {
    const auto& modes = CurrentModes();
    return modes.audit != Mode::Reference || modes.ordering == Mode::Verify || modes.directory == Mode::Verify;
}

bool Ordering() {
    return CurrentModes().ordering != Mode::Reference;
}

bool Canonicalizing() {
    return CurrentModes().directory != Mode::Reference;
}

void SetBackingTranslator(BackingTranslator backing) {
    translator.store(backing, std::memory_order_release);
}

std::vector<BackingRun> Translate(Interval range) {
    if (range.end <= range.begin) return {};
    if (!Canonicalizing()) return {{range.begin, range.end, range.begin}};
    const auto backing = translator.load(std::memory_order_acquire);
    if (backing == nullptr) throw std::runtime_error("APS5_COHERENCE_DIRECTORY: no guest backing translator is installed");
    std::size_t capacity = 8;
    std::vector<std::uintptr_t> begins, ends;
    std::vector<std::uint64_t> canonicals;
    std::size_t count = 0;
    for (;;) {
        begins.resize(capacity);
        ends.resize(capacity);
        canonicals.resize(capacity);
        count = backing(static_cast<std::uintptr_t>(range.begin), static_cast<std::size_t>(range.end - range.begin), begins.data(), ends.data(), canonicals.data(), capacity);
        if (count <= capacity) break;
        capacity = count;
    }
    std::vector<BackingRun> runs;
    runs.reserve(count);
    auto expected = range.begin;
    for (std::size_t index = 0; index < count; ++index) {
        if (begins[index] != expected || ends[index] <= begins[index] || ends[index] > range.end) throw std::runtime_error("coherence: the guest backing translator returned runs that do not tile the range");
        runs.push_back({begins[index], ends[index], canonicals[index]});
        expected = ends[index];
    }
    if (expected != range.end) throw std::runtime_error("coherence: the guest backing translator left part of the range untranslated");
    return runs;
}

Version NextVersion() {
    return versions.fetch_add(1, std::memory_order_acq_rel) + 1;
}

Version CurrentVersion() {
    return versions.load(std::memory_order_acquire);
}

const char* ProducerName(Producer producer) {
    return ProducerNames[static_cast<std::size_t>(producer)];
}

const char* SourceName(Source source) {
    return SourceNames[static_cast<std::size_t>(source)];
}

ProducerToken::ProducerToken(ProducerToken&& other) noexcept : id(std::exchange(other.id, 0)) {}

ProducerToken& ProducerToken::operator=(ProducerToken&& other) noexcept {
    if (this != &other) {
        release();
        id = std::exchange(other.id, 0);
    }
    return *this;
}

ProducerToken::~ProducerToken() {
    release();
}

void ProducerToken::Recorded() {
    if (id == 0) return;
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    const auto it = state.active.find(id);
    if (it == state.active.end()) throw std::runtime_error("coherence: a producer token was recorded after it completed");
    if (it->second.recorded) return;
    auto active = std::move(it->second);
    state.active.erase(it);
    active.recorded = true;
    const auto reserved = id;
    id = NextVersion();
    auto& newest = state.recordedByQueue[active.queue];
    newest = std::max(newest, id);
    for (const auto& interval : active.intervals) Note(state, EventType::Recorded, static_cast<std::uint8_t>(active.kind), active.queue, id, reserved, interval, nullptr);
    state.active.emplace(id, std::move(active));
}

void ProducerToken::Complete(std::span<const Interval> written) {
    if (id == 0) return;
    auto& state = Directory();
    Active active;
    {
        std::lock_guard lock(state.mutex);
        const auto it = state.active.find(id);
        if (it == state.active.end()) throw std::runtime_error("coherence: a producer token completed twice");
        active = std::move(it->second);
        state.active.erase(it);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> remapped;
    if (DirectoryMode() == Mode::Verify) {
        for (const auto& run : active.runs) {
            const auto now = Translate({run.begin, run.end});
            if (!SameRuns(now, std::span(&run, 1))) remapped.emplace_back(run.begin, run.end);
        }
    }
    std::lock_guard lock(state.mutex);
    for (const auto& range : written) {
        for (const auto& span : Spans(MapThrough(active.runs, range))) {
            for (const auto& interval : active.intervals) {
                if (!Overlaps(span, interval)) continue;
                const auto clipped = Clip(span, interval);
                Commit(state, clipped, id, active.kind);
                Note(state, EventType::Commit, static_cast<std::uint8_t>(active.kind), active.queue, id, 0, clipped, nullptr);
            }
        }
    }
    for (const auto& interval : active.intervals) Note(state, EventType::Complete, static_cast<std::uint8_t>(active.kind), active.queue, id, 0, interval, nullptr);
    ++state.counters.completions;
    const auto completed = id;
    id = 0;
    if (remapped.empty()) return;
    state.counters.remappedCompletions += remapped.size();
    for (const auto& [begin, end] : remapped) Note(state, EventType::Remapped, static_cast<std::uint8_t>(active.kind), active.queue, completed, 0, {begin, end}, nullptr);
    char text[256];
    std::snprintf(text, sizeof(text), "[coherence] %s v%llu completed after [0x%llx, 0x%llx) was mapped to another backing; its writes stay with the backing it was reserved on", ProducerName(active.kind), static_cast<unsigned long long>(completed), static_cast<unsigned long long>(remapped.front().first), static_cast<unsigned long long>(remapped.front().second));
    Divergence(state, text, active.intervals, true, state.remapsReported);
}

void ProducerToken::release() noexcept {
    if (id == 0) return;
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    const auto it = state.active.find(id);
    if (it != state.active.end()) {
        if (it->second.recorded) {
            for (const auto& interval : it->second.intervals) {
                Commit(state, interval, id, it->second.kind);
                Note(state, EventType::Commit, static_cast<std::uint8_t>(it->second.kind), it->second.queue, id, 0, interval, nullptr);
            }
            ++state.counters.completions;
        } else {
            for (const auto& interval : it->second.intervals) Note(state, EventType::Abandon, static_cast<std::uint8_t>(it->second.kind), it->second.queue, id, 0, interval, nullptr);
            ++state.counters.abandoned;
        }
        state.active.erase(it);
    }
    id = 0;
}

ProducerToken Reserve(Producer kind, std::span<const Interval> intervals, bool conservative, std::uint32_t queue) {
    if (!Tracking()) return {};
    Active active{kind, {}, {}, conservative, false, queue};
    for (const auto& interval : intervals) {
        if (interval.end <= interval.begin) continue;
        auto runs = Translate(interval);
        active.runs.insert(active.runs.end(), runs.begin(), runs.end());
    }
    if (active.runs.empty()) return {};
    active.intervals = Spans(active.runs);
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    const auto version = NextVersion();
    for (const auto& interval : active.intervals) Note(state, EventType::Reserve, static_cast<std::uint8_t>(kind), queue, version, conservative ? 1 : 0, interval, nullptr);
    state.active.emplace(version, std::move(active));
    ++state.counters.reservations;
    MaybeReport(state);
    return ProducerToken(version);
}

std::optional<Writer> NewerWriter(Interval destination, Version source) {
    const auto spans = Spans(Translate(destination));
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    return NewerOverSpans(state, spans, source);
}

bool CheckPublication(Source source, Version sourceVersion, Interval destination, const char* reason) {
    if (!Tracking() || destination.end <= destination.begin) return true;
    const auto spans = Spans(Translate(destination));
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    ++state.counters.publicationsChecked;
    const auto writer = NewerOverSpans(state, spans, sourceVersion);
    if (!writer) {
        for (const auto& span : spans) Note(state, EventType::Publish, static_cast<std::uint8_t>(source), 0, sourceVersion, 0, span, reason);
        MaybeReport(state);
        return true;
    }
    ++state.counters.stalePublications;
    for (const auto& span : spans) Note(state, EventType::StalePublish, static_cast<std::uint8_t>(source), 0, sourceVersion, writer->version, span, reason);
    char text[448];
    std::snprintf(text, sizeof(text), "[coherence] stale publication: %s v%llu over [0x%llx, 0x%llx) for %s; newer %s v%llu over backing [0x%llx, 0x%llx) (%s%s)", SourceName(source), static_cast<unsigned long long>(sourceVersion), static_cast<unsigned long long>(destination.begin), static_cast<unsigned long long>(destination.end), reason != nullptr ? reason : "?", ProducerName(writer->kind), static_cast<unsigned long long>(writer->version), static_cast<unsigned long long>(writer->range.begin), static_cast<unsigned long long>(writer->range.end), writer->completed ? "completed" : writer->recorded ? "recorded" : "reserved", writer->conservative ? ", conservative" : "");
    Divergence(state, text, spans, Verifying(), state.publicationsReported);
    MaybeReport(state);
    return false;
}

bool CheckRead(Source newer, Version newerVersion, Interval range, const char* reader) {
    if (!Tracking() || range.end <= range.begin) return true;
    const auto spans = Spans(Translate(range));
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    ++state.counters.staleReads;
    for (const auto& span : spans) Note(state, EventType::StaleRead, static_cast<std::uint8_t>(newer), 0, newerVersion, 0, span, reader);
    char text[320];
    std::snprintf(text, sizeof(text), "[coherence] stale read: %s binds host bytes [0x%llx, 0x%llx) while %s v%llu over them is unpublished", reader != nullptr ? reader : "?", static_cast<unsigned long long>(range.begin), static_cast<unsigned long long>(range.end), SourceName(newer), static_cast<unsigned long long>(newerVersion));
    Divergence(state, text, spans, OrderingMode() == Mode::Verify, state.readsReported);
    MaybeReport(state);
    return false;
}

void CheckDrained(bool pendingImages, bool liveShadows, const char* where, bool writeBacksOrdered) {
    if (!writeBacksOrdered && Tracking()) {
        auto& state = Directory();
        std::lock_guard lock(state.mutex);
        ++state.counters.undrainedReleases;
        Note(state, EventType::Undrained, 0, 0, CurrentVersion(), 0, {0, ~std::uint64_t{0}}, where);
        char text[192];
        std::snprintf(text, sizeof(text), "[coherence] release at %s is CPU-visible ahead of guest write-backs still owed", where != nullptr ? where : "?");
        Divergence(state, text, {}, Verifying(), state.releasesReported);
    }
    if (!Tracking() || (!pendingImages && !liveShadows)) return;
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    ++state.counters.undrainedReleases;
    Note(state, EventType::Undrained, 0, 0, CurrentVersion(), 0, {0, ~std::uint64_t{0}}, where);
    char text[256];
    std::snprintf(text, sizeof(text), "[coherence] release at %s is CPU-visible while %s%s%s are unpublished", where != nullptr ? where : "?", pendingImages ? "pending images" : "", pendingImages && liveShadows ? " and " : "", liveShadows ? "shadow units" : "");
    Divergence(state, text, {}, OrderingMode() == Mode::Verify, state.releasesReported);
}

void CheckReleaseCovered(bool uncoveredImages, bool uncoveredShadows, const char* where) {
    if (!uncoveredImages && !uncoveredShadows) return;
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    ++state.counters.uncoveredReleases;
    Note(state, EventType::Undrained, 0, 0, CurrentVersion(), 0, {0, ~std::uint64_t{0}}, where);
    char text[256];
    std::snprintf(text, sizeof(text), "[coherence] lazy release at %s left %s%s%s neither stored nor protected", where != nullptr ? where : "?", uncoveredImages ? "pending images" : "", uncoveredImages && uncoveredShadows ? " and " : "", uncoveredShadows ? "shadow units" : "");
    Divergence(state, text, {}, LazyReleaseMode() == Mode::Verify, state.uncoveredReported);
}

Release SnapshotRelease(std::uint32_t queue, Interval label, std::uint64_t value, std::uint64_t labelStamp) {
    Release release{queue, label, value, labelStamp, NextVersion(), {}};
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    release.frontier.assign(state.recordedByQueue.begin(), state.recordedByQueue.end());
    return release;
}

void ImportFrontier(std::uint32_t consumerQueue, const Release& release) {
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    auto& frontier = state.imported[consumerQueue];
    for (const auto& [queue, version] : release.frontier) {
        auto& known = frontier[queue];
        known = std::max(known, version);
    }
}

std::vector<std::pair<std::uint32_t, Version>> QueueFrontier(std::uint32_t queue) {
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    const auto found = state.imported.find(queue);
    if (found == state.imported.end()) return {};
    return {found->second.begin(), found->second.end()};
}

Counters CurrentCounters() {
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    auto counters = state.counters;
    counters.committedIntervals = state.committed.size();
    counters.activeWriters = state.active.size();
    auto& ledger = Protections();
    std::lock_guard protections(ledger.mutex);
    counters.protections = ledger.protections;
    counters.refusedProtections = ledger.refused;
    counters.partialProtections = ledger.partial;
    counters.faultsResolved = ledger.resolved;
    counters.lockedFaults = ledger.locked;
    counters.liveProtections = ledger.live.size();
    counters.leases = ledger.leases;
    counters.leaseWaits = ledger.leaseWaits;
    return counters;
}

std::string Report() {
    const auto c = CurrentCounters();
    char text[768];
    std::snprintf(text, sizeof(text), "[coherence] reservations %llu, completions %llu, abandoned %llu, active %llu, publications checked %llu, stale publications %llu, stale reads %llu, undrained releases %llu, remapped completions %llu, committed %llu, evicted %llu", static_cast<unsigned long long>(c.reservations), static_cast<unsigned long long>(c.completions), static_cast<unsigned long long>(c.abandoned), static_cast<unsigned long long>(c.activeWriters), static_cast<unsigned long long>(c.publicationsChecked), static_cast<unsigned long long>(c.stalePublications), static_cast<unsigned long long>(c.staleReads), static_cast<unsigned long long>(c.undrainedReleases), static_cast<unsigned long long>(c.remappedCompletions), static_cast<unsigned long long>(c.committedIntervals), static_cast<unsigned long long>(c.evictedIntervals));
    std::string line = text;
    std::snprintf(text, sizeof(text), "; protections %llu (refused %llu, partial %llu, live %llu), faults resolved %llu, locked faults %llu, leases %llu, lease waits %llu, uncovered releases %llu", static_cast<unsigned long long>(c.protections), static_cast<unsigned long long>(c.refusedProtections), static_cast<unsigned long long>(c.partialProtections), static_cast<unsigned long long>(c.liveProtections), static_cast<unsigned long long>(c.faultsResolved), static_cast<unsigned long long>(c.lockedFaults), static_cast<unsigned long long>(c.leases), static_cast<unsigned long long>(c.leaseWaits), static_cast<unsigned long long>(c.uncoveredReleases));
    line += text;
    return line;
}

std::vector<std::string> RecentEvents(Interval range, std::size_t limit) {
    const auto spans = Spans(Translate(range));
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    return Recent(state, spans, limit);
}

void ResetForTests() {
    auto& state = Directory();
    std::lock_guard lock(state.mutex);
    state.active.clear();
    state.committed.clear();
    state.events = 0;
    state.counters = {};
    state.publicationsReported = 0;
    state.readsReported = 0;
    state.releasesReported = 0;
    state.remapsReported = 0;
    state.uncoveredReported = 0;
    state.recordedByQueue.clear();
    state.imported.clear();
    auto& ledger = Protections();
    std::lock_guard protections(ledger.mutex);
    ledger.live.clear();
    ledger.protections = 0;
    ledger.refused = 0;
    ledger.partial = 0;
    ledger.resolved = 0;
    ledger.locked = 0;
    ledger.leases = 0;
    ledger.leaseWaits = 0;
}

}

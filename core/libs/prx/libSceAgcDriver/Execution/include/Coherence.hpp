#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COHERENCE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COHERENCE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Coherence {

enum class Mode : std::uint8_t { Reference, New, Verify };

Mode ParseMode(const char* variable, const char* value);
Mode AuditMode();
Mode OrderingMode();
Mode DirectoryMode();
Mode FaultsMode();
Mode InputLeasesMode();
Mode ResidencyMode();
Mode LazyReleaseMode();
void SetModesForTests(Mode audit, Mode ordering, Mode directory);
void SetFaultsModeForTests(Mode faults);
void SetInputLeasesModeForTests(Mode leases);
void SetResidencyModeForTests(Mode residency);
void SetLazyReleaseModeForTests(Mode release);
bool Tracking();
bool Ordering();
bool Canonicalizing();
bool FaultOwnership();
bool InputLeases();
bool Residency();
bool LazyRelease();

using Version = std::uint64_t;

Version NextVersion();
Version CurrentVersion();

struct Interval {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

struct BackingRun {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t canonical = 0;
};

using BackingTranslator = std::size_t (*)(std::uintptr_t address, std::size_t bytes, std::uintptr_t* begins, std::uintptr_t* ends, std::uint64_t* canonicals, std::size_t capacity);

void SetBackingTranslator(BackingTranslator translator);
std::vector<BackingRun> Translate(Interval range);

enum class Producer : std::uint8_t { PointerWriter, ElementWriter, LabelStore, Count };
enum class Source : std::uint8_t { PendingImage, ShadowUnit, ResidentStaging, ReadLease, Count };

const char* ProducerName(Producer producer);
const char* SourceName(Source source);

class ProducerToken {
public:
    ProducerToken() = default;
    ProducerToken(const ProducerToken&) = delete;
    ProducerToken& operator=(const ProducerToken&) = delete;
    ProducerToken(ProducerToken&& other) noexcept;
    ProducerToken& operator=(ProducerToken&& other) noexcept;
    ~ProducerToken();

    Version Id() const { return id; }
    bool Held() const { return id != 0; }
    void Recorded();
    void Complete(std::span<const Interval> written);

private:
    friend ProducerToken Reserve(Producer kind, std::span<const Interval> intervals, bool conservative, std::uint32_t queue);
    explicit ProducerToken(Version id) : id(id) {}
    void release() noexcept;
    Version id = 0;
};

ProducerToken Reserve(Producer kind, std::span<const Interval> intervals, bool conservative, std::uint32_t queue);

struct Writer {
    Interval range;
    Version version = 0;
    Producer kind = Producer::PointerWriter;
    bool conservative = false;
    bool recorded = false;
    bool completed = false;
};

std::optional<Writer> NewerWriter(Interval destination, Version source);
bool CheckPublication(Source source, Version sourceVersion, Interval destination, const char* reason);
bool CheckRead(Source newer, Version newerVersion, Interval range, const char* reader);
void CheckDrained(bool pendingImages, bool liveShadows, const char* where, bool writeBacksOrdered = true);
void CheckReleaseCovered(bool uncoveredImages, bool uncoveredShadows, const char* where);

struct Release {
    std::uint32_t queue = 0;
    Interval label;
    std::uint64_t value = 0;
    std::uint64_t labelStamp = 0;
    Version version = 0;
    std::vector<std::pair<std::uint32_t, Version>> frontier;
};

Release SnapshotRelease(std::uint32_t queue, Interval label, std::uint64_t value, std::uint64_t labelStamp);
void ImportFrontier(std::uint32_t consumerQueue, const Release& release);
std::vector<std::pair<std::uint32_t, Version>> QueueFrontier(std::uint32_t queue);

struct ProtectionBackend {
    std::uint64_t (*protect)(std::uintptr_t address, std::size_t bytes, bool (*resolve)(std::uint64_t token, void* context), void* context, void (*releaseContext)(void* context)) = nullptr;
    void (*release)(std::uint64_t token) = nullptr;
    bool (*open)(std::uint64_t token) = nullptr;
};

using Materializer = void (*)(Interval range);

void SetProtectionBackend(ProtectionBackend backend);
void SetMaterializer(Materializer materializer);
std::uint64_t ProtectDeviceOwned(Interval range, Source source, Version version);
void ReleaseDeviceOwned(std::uint64_t protection);
bool ProtectionLive(std::uint64_t protection);
void NoteLockedFault(Interval range);
void SetLeaseWaiter(Materializer waiter);
using FaultPriority = bool (*)();
void SetFaultPriority(FaultPriority priority);
std::uint64_t AcquireReadLease(Interval range, Version version);
std::uint64_t ProtectResident(Interval range, Version version);
void RetireReadLease(std::uint64_t lease);

struct Counters {
    std::uint64_t reservations = 0;
    std::uint64_t completions = 0;
    std::uint64_t abandoned = 0;
    std::uint64_t publicationsChecked = 0;
    std::uint64_t stalePublications = 0;
    std::uint64_t staleReads = 0;
    std::uint64_t undrainedReleases = 0;
    std::uint64_t remappedCompletions = 0;
    std::uint64_t committedIntervals = 0;
    std::uint64_t evictedIntervals = 0;
    std::uint64_t activeWriters = 0;
    std::uint64_t protections = 0;
    std::uint64_t refusedProtections = 0;
    std::uint64_t partialProtections = 0;
    std::uint64_t faultsResolved = 0;
    std::uint64_t lockedFaults = 0;
    std::uint64_t liveProtections = 0;
    std::uint64_t leases = 0;
    std::uint64_t leaseWaits = 0;
    std::uint64_t uncoveredReleases = 0;
};

Counters CurrentCounters();
std::string Report();
std::vector<std::string> RecentEvents(Interval range, std::size_t limit);
void ResetForTests();

}

#endif

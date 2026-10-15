#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COHERENCE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COHERENCE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::Coherence {

enum class Mode : std::uint8_t { Reference, New, Verify };

Mode ParseMode(const char* variable, const char* value);
Mode AuditMode();
Mode OrderingMode();
Mode DirectoryMode();
void SetModesForTests(Mode audit, Mode ordering, Mode directory);
bool Tracking();
bool Ordering();
bool Canonicalizing();

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

enum class Producer : std::uint8_t { PointerWriter, ElementWriter, Count };
enum class Source : std::uint8_t { PendingImage, ShadowUnit, ResidentStaging, Count };

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
void CheckDrained(bool pendingImages, bool liveShadows, const char* where);

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
};

Counters CurrentCounters();
std::string Report();
std::vector<std::string> RecentEvents(Interval range, std::size_t limit);
void ResetForTests();

}

#endif

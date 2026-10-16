#include "prx/libSceAgcDriver/Execution/include/DrawTrace.hpp"
#if __has_include("prx/libSceAgcDriver/TraceRevision.hpp")
#include "prx/libSceAgcDriver/TraceRevision.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <time.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

namespace AgcDriver::DrawTrace {

namespace {

std::atomic<Id> identifiers{0};
thread_local Session* boundSession = nullptr;
thread_local Clock::time_point (*boundClock)() = nullptr;
thread_local Context currentContext;
thread_local Id currentSpan = 0;
thread_local Id currentThread = 0;
thread_local std::size_t eligibility = 0;
thread_local Operation* currentOperation = nullptr;
thread_local JobScope* currentJob = nullptr;
thread_local std::optional<std::uint64_t> lastPresentOrdinal;

Id allocateId() {
    const auto id = identifiers.fetch_add(1, std::memory_order_relaxed) + 1;
    if (id == 0) throw std::overflow_error("draw trace ID space exhausted");
    return id;
}

Clock::time_point now() {
    return boundClock != nullptr ? boundClock() : Clock::now();
}

std::string quote(std::string_view value) {
    std::string output = "\"";
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            output += '\\';
            output += static_cast<char>(byte);
        } else if (byte < 0x20 || byte >= 0x7f) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
            output += escaped;
        } else output += static_cast<char>(byte);
    }
    return output + '"';
}

std::string idJson(Id id) {
    return id == 0 ? "null" : quote(std::to_string(id));
}

std::string finiteJson(double value) {
    if (!std::isfinite(value)) return "null";
    char text[64];
    const auto result = std::to_chars(text, text + sizeof(text), value, std::chars_format::general,
                                      std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) throw std::runtime_error("cannot encode draw trace number");
    return std::string(text, result.ptr);
}

std::string_view kindName(EventKind kind) {
    switch (kind) {
    case EventKind::Submission: return "submission";
    case EventKind::Segment: return "segment";
    case EventKind::Packet: return "packet";
    case EventKind::Operation: return "operation";
    case EventKind::HostCommand: return "host_command";
    case EventKind::Batch: return "batch";
    case EventKind::Flip: return "flip";
    case EventKind::Present: return "present";
    case EventKind::Link: return "link";
    case EventKind::Job: return "job";
    case EventKind::Span: return "span";
    case EventKind::Boundary: return "boundary";
    case EventKind::Calibration: return "calibration";
    case EventKind::GpuQuery: return "gpu_query";
    case EventKind::Attribute: return "attribute";
    case EventKind::Reference: return "reference";
    case EventKind::Counter: return "counter";
    }
    return "unknown";
}

std::string_view statusName(Status status) {
    switch (status) {
    case Status::Unknown: return "unknown";
    case Status::Complete: return "complete";
    case Status::Skipped: return "skipped";
    case Status::Rejected: return "rejected";
    case Status::Failed: return "failed";
    case Status::Cancelled: return "cancelled";
    }
    return "unknown";
}

std::string_view queryName(QueryStatus status) {
    switch (status) {
    case QueryStatus::Pending: return "pending";
    case QueryStatus::Available: return "available";
    case QueryStatus::Unsupported: return "unsupported";
    case QueryStatus::AllocationFailed: return "allocation_failed";
    case QueryStatus::CapacityDropped: return "capacity_dropped";
    case QueryStatus::ReadFailed: return "read_failed";
    case QueryStatus::InvalidTimestamp: return "invalid_timestamp";
    case QueryStatus::Uncollected: return "uncollected";
    }
    return "uncollected";
}

std::uint64_t nativeThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    const auto id = syscall(SYS_gettid);
    return id > 0 ? static_cast<std::uint64_t>(id) : 0;
#else
    return 0;
#endif
}

const char* nativeThreadSource() {
#ifdef _WIN32
    return "windows_thread_id";
#elif defined(__linux__)
    return "linux_gettid";
#else
    return "unavailable";
#endif
}

CpuSample platformCpu() {
    CpuSample sample;
    sample.nativeThread = nativeThreadId();
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        sample.status = CpuStatus::Failed;
        return sample;
    }
    const auto ticks = [](FILETIME value) { return (std::uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime; };
    const auto kernelTicks = ticks(kernel), userTicks = ticks(user);
    if (kernelTicks > std::numeric_limits<std::uint64_t>::max() - userTicks) {
        sample.status = CpuStatus::Failed;
        return sample;
    }
    sample.ticks = kernelTicks + userTicks;
    sample.frequency = 10000000;
    sample.status = CpuStatus::Measured;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0 || time.tv_sec < 0 || time.tv_nsec < 0
        || static_cast<std::uint64_t>(time.tv_sec) > std::numeric_limits<std::uint64_t>::max() / 1000000000) {
        sample.status = CpuStatus::Failed;
        return sample;
    }
    sample.ticks = static_cast<std::uint64_t>(time.tv_sec) * 1000000000 + static_cast<std::uint64_t>(time.tv_nsec);
    sample.frequency = 1000000000;
    sample.status = sample.nativeThread != 0 ? CpuStatus::Measured : CpuStatus::Unavailable;
#endif
    return sample;
}

CpuStatus cpuOutcome(const CpuSample& begin, const CpuSample& end) {
    if (begin.status != CpuStatus::Measured) return begin.status;
    if (end.status != CpuStatus::Measured) return end.status;
    if (begin.nativeThread == 0 || begin.nativeThread != end.nativeThread) return CpuStatus::ThreadChanged;
    if (begin.frequency == 0 || begin.frequency != end.frequency || end.ticks < begin.ticks) return CpuStatus::Nonmonotonic;
    return CpuStatus::Measured;
}

CycleSample platformCycles() {
    CycleSample sample;
    sample.nativeThread = nativeThreadId();
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        sample.status = CpuStatus::Failed;
        return sample;
    }
    sample.cycles = cycles;
    sample.status = sample.nativeThread != 0 ? CpuStatus::Measured : CpuStatus::Unavailable;
#endif
    return sample;
}

CpuStatus cycleOutcome(const CycleSample& begin, const CycleSample& end) {
    if (begin.status != CpuStatus::Measured) return begin.status;
    if (end.status != CpuStatus::Measured) return end.status;
    if (begin.nativeThread == 0 || begin.nativeThread != end.nativeThread) return CpuStatus::ThreadChanged;
    if (end.cycles < begin.cycles) return CpuStatus::Nonmonotonic;
    return CpuStatus::Measured;
}

const char* cycleSource() {
#ifdef _WIN32
    return "query_thread_cycle_time";
#else
    return "unavailable";
#endif
}

const char* cpuSource() {
#ifdef _WIN32
    return "get_thread_times";
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    return "clock_thread_cputime_id";
#else
    return "unavailable";
#endif
}

const char* cpuName(CpuStatus status) {
    switch (status) {
    case CpuStatus::NotRequested: return "not_requested";
    case CpuStatus::Measured: return "measured";
    case CpuStatus::Unavailable: return "unavailable";
    case CpuStatus::Failed: return "failed";
    case CpuStatus::Nonmonotonic: return "nonmonotonic";
    case CpuStatus::ThreadChanged: return "thread_changed";
    case CpuStatus::Clipped: return "clipped";
    }
    return "unavailable";
}

CalibrationSample nativeAnchor() {
    CalibrationSample sample;
    sample.before = Clock::now();
#ifdef _WIN32
    LARGE_INTEGER ticks{}, frequency{};
    sample.available = QueryPerformanceCounter(&ticks) && QueryPerformanceFrequency(&frequency) && ticks.QuadPart >= 0 && frequency.QuadPart > 0;
    sample.hostTicks = sample.conversionHostTicks = static_cast<std::uint64_t>(ticks.QuadPart);
    sample.hostFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
    sample.hostDomain = "query_performance_counter";
#elif defined(CLOCK_MONOTONIC)
    timespec time{};
    sample.available = clock_gettime(CLOCK_MONOTONIC, &time) == 0 && time.tv_sec >= 0 && time.tv_nsec >= 0;
    sample.hostTicks = sample.conversionHostTicks = static_cast<std::uint64_t>(time.tv_sec) * 1000000000 + static_cast<std::uint64_t>(time.tv_nsec);
    sample.hostFrequency = 1000000000;
    sample.hostDomain = "clock_monotonic";
#endif
    sample.after = Clock::now();
    return sample;
}

void copyText(char* output, std::size_t capacity, const char* text) {
    if (text == nullptr) return;
    std::size_t index = 0;
    for (; index + 1 < capacity && text[index] != '\0'; ++index) output[index] = text[index];
    output[index] = '\0';
}

bool canonicalUuid(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
}

std::string randomUuid() {
    std::random_device source;
    std::array<unsigned char, 16> bytes{};
    for (auto& byte : bytes) byte = static_cast<unsigned char>(source());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 15u) | 64u);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 63u) | 128u);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4u];
        result += hex[bytes[i] & 15u];
    }
    return result;
}

std::string contextJson(const Context& context) {
    return "{\"driver\":" + idJson(context.driver) + ",\"submission\":" + idJson(context.submission)
        + ",\"segment\":" + idJson(context.segment) + ",\"packet\":" + idJson(context.packet)
        + ",\"operation\":" + idJson(context.operation) + ",\"device\":" + idJson(context.device)
        + ",\"batch\":" + idJson(context.batch) + ",\"job\":" + idJson(context.job)
        + ",\"flip\":" + idJson(context.flip) + ",\"present\":" + idJson(context.present)
        + ",\"queue\":" + (context.queue == 0xffffffffu ? "null" : std::to_string(context.queue))
        + ",\"packet_offset\":" + std::to_string(context.packetOffset)
        + ",\"opcode\":" + (context.opcode == 0xffffffffu ? "null" : std::to_string(context.opcode)) + "}";
}

}

struct Session::State {
    static constexpr std::size_t ChunkRecords = 1024;
    enum class Phase { Armed, Recording, Draining, Frozen };
    struct PendingSlots {
        explicit PendingSlots(std::size_t capacity) : capacity(capacity), slots(std::make_unique<std::atomic<Id>[]>(capacity)) {}
        bool insert(Id id) {
            if (id == 0 || id == std::numeric_limits<Id>::max()) return false;
            for (std::size_t i = 0; i < capacity; ++i) {
                auto& slot = slots[(id + i) % capacity];
                auto value = slot.load(std::memory_order_acquire);
                if (value == id) return false;
                if ((value == 0 || value == std::numeric_limits<Id>::max())
                    && slot.compare_exchange_strong(value, id, std::memory_order_acq_rel)) {
                    count.fetch_add(1, std::memory_order_release);
                    return true;
                }
            }
            return false;
        }
        bool contains(Id id) const {
            if (id == 0 || id == std::numeric_limits<Id>::max()) return false;
            for (std::size_t i = 0; i < capacity; ++i) {
                const auto value = slots[(id + i) % capacity].load(std::memory_order_acquire);
                if (value == id) return true;
                if (value == 0) return false;
            }
            return false;
        }
        void erase(Id id) {
            if (id == 0 || id == std::numeric_limits<Id>::max()) return;
            for (std::size_t i = 0; i < capacity; ++i) {
                auto& slot = slots[(id + i) % capacity];
                auto value = slot.load(std::memory_order_acquire);
                if (value == 0) return;
                if (value == id && slot.compare_exchange_strong(value, std::numeric_limits<Id>::max(), std::memory_order_acq_rel)) {
                    count.fetch_sub(1, std::memory_order_release);
                    return;
                }
            }
        }
        bool empty() const { return count.load(std::memory_order_acquire) == 0; }
        std::size_t size() const { return count.load(std::memory_order_acquire); }
        std::size_t capacity;
        std::unique_ptr<std::atomic<Id>[]> slots;
        std::atomic<std::size_t> count{0};
    };
    struct Thread {
        mutable std::mutex mutex;
        Id id = 0;
        std::uint64_t nativeId = 0;
        std::string name;
        Clock::time_point registered{};
        Clock::time_point last{};
        std::uint64_t attempted = 0;
        std::uint64_t lost = 0;
        std::vector<std::unique_ptr<Event[]>> chunks;
        std::size_t recordCount = 0;
        std::array<std::optional<Event>, 32> observations;

        const Event& record(std::size_t index) const { return chunks[index / ChunkRecords][index % ChunkRecords]; }
    };
    struct MetadataEntry {
        Id id = 0;
        std::string kind;
        std::string key;
        std::vector<std::pair<std::string, std::string>> fields;
        std::vector<Id> references;
    };
    explicit State(Options options) : options(std::move(options)), spans(this->options.maximumQueries), jobs(this->options.maximumQueries),
        publishedThreads(std::make_unique<std::atomic<Thread*>[]>(this->options.maximumThreads)),
        queryIndexes(this->options.maximumQueries, std::numeric_limits<std::size_t>::max()) {
        queries.reserve(this->options.maximumQueries);
        boundaries.reserve(this->options.maximumBoundaries);
    }
    mutable std::mutex mutex;
    mutable std::mutex queryMutex;
    Options options;
    std::atomic<Phase> phase{Phase::Armed};
    std::atomic<std::size_t> admissions{0};
    std::atomic<std::size_t> writers{0};
    std::optional<Clock::time_point> first;
    std::optional<Clock::time_point> opened;
    std::optional<Clock::time_point> closed;
    std::optional<Clock::time_point> frozen;
    std::optional<std::uint64_t> openPresent;
    std::optional<std::uint64_t> closePresent;
    std::vector<std::pair<std::uint64_t, Clock::time_point>> boundaries;
    std::vector<std::unique_ptr<Thread>> threads;
    std::vector<MetadataEntry> metadata;
    std::map<std::pair<std::string, std::string>, std::size_t> metadataKeys;
    PendingSlots spans;
    PendingSlots jobs;
    std::unique_ptr<std::atomic<Thread*>[]> publishedThreads;
    std::atomic<std::size_t> publishedCount{0};
    std::vector<Event> queries;
    std::vector<std::size_t> queryIndexes;
    std::string closeReason;
    std::atomic<std::uint64_t> lostEvents{0};
    std::atomic<std::uint64_t> unassignedEvents{0};
    std::atomic<std::uint64_t> lostMetadata{0};
    std::atomic<std::uint64_t> lostQueries{0};
    std::atomic<std::uint64_t> queryFailures{0};
    std::atomic<std::size_t> unfinishedQueries{0};
    bool interrupted = false;
    std::atomic<std::size_t> allocatedBytes{0};
    std::size_t allocatedIndexBytes = 0;
    std::atomic<std::size_t> retainedEvents{0};
    Id identity = allocateId();
    CalibrationSample clockAnchor = nativeAnchor();
    mutable std::mutex journalMutex;
    std::vector<std::size_t> journalCursors;
    std::size_t journalThread = 0;
    std::size_t journalMetadata = 0;
    std::size_t journalBoundary = 0;
    std::size_t journalNextThread = 0;
    std::uint64_t journalSequence = 0;
    std::atomic<std::uint64_t> journalEvents{0};
    std::atomic<std::uint64_t> journalBytes{0};
    std::atomic<bool> journalStopped{false};
    std::atomic<bool> journalFailed{false};
    std::atomic<bool> journalBudget{false};
    bool journalHeader = false;
    bool journalFooter = false;

    Event* query(Id id) {
        for (std::size_t offset = 0; offset < queryIndexes.size(); ++offset) {
            const auto index = queryIndexes[(id + offset) % queryIndexes.size()];
            if (index == std::numeric_limits<std::size_t>::max()) return nullptr;
            if (queries[index].subject == id) return &queries[index];
        }
        return nullptr;
    }

    Thread* thread(Id id) {
        const auto count = publishedCount.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < count; ++index) {
            auto* result = publishedThreads[index].load(std::memory_order_acquire);
            if (result != nullptr && result->id == id) return result;
        }
        return nullptr;
    }

    void unassignedLoss() {
        ++lostEvents;
        ++unassignedEvents;
    }

    bool storeLocked(Thread* found, Event event) {
        ++found->attempted;
        found->last = std::max(found->last, event.begin);
        const auto lose = [&] {
            ++found->lost;
            ++lostEvents;
            return false;
        };
        if (found->recordCount >= options.recordsPerThread) return lose();
        auto retained = retainedEvents.load(std::memory_order_relaxed);
        do {
            if (retained >= options.maximumEvents) return lose();
        } while (!retainedEvents.compare_exchange_weak(retained, retained + 1, std::memory_order_acq_rel));
        if (found->recordCount / ChunkRecords == found->chunks.size()) {
            const auto count = std::min(ChunkRecords, options.recordsPerThread - found->recordCount);
            const auto bytes = count * sizeof(Event);
            auto allocated = allocatedBytes.load(std::memory_order_relaxed);
            do {
                if (allocated > options.maximumBytes || bytes > options.maximumBytes - allocated) {
                    retainedEvents.fetch_sub(1, std::memory_order_release);
                    return lose();
                }
            } while (!allocatedBytes.compare_exchange_weak(allocated, allocated + bytes, std::memory_order_acq_rel));
            try {
                auto chunk = options.allocateChunk != nullptr ? options.allocateChunk(count) : std::make_unique<Event[]>(count);
                if (!chunk) throw std::bad_alloc();
                found->chunks.push_back(std::move(chunk));
            } catch (...) {
                allocatedBytes.fetch_sub(bytes, std::memory_order_release);
                retainedEvents.fetch_sub(1, std::memory_order_release);
                return lose();
            }
        }
        if (event.id == 0) event.id = allocateId();
        if (event.name != nullptr) {
            copyText(event.nameStorage.data(), event.nameStorage.size(), event.name);
            if (std::char_traits<char>::length(event.name) >= event.nameStorage.size()) ++lostMetadata;
            event.name = nullptr;
        }
        if (event.kind == EventKind::Calibration) {
            copyText(event.calibrationDomain.data(), event.calibrationDomain.size(), event.calibration.hostDomain);
            event.calibration.hostDomain = nullptr;
        }
        found->chunks[found->recordCount / ChunkRecords][found->recordCount % ChunkRecords] = event;
        ++found->recordCount;
        return true;
    }

    bool store(Event event) {
        writers.fetch_add(1, std::memory_order_acq_rel);
        struct Finish { std::atomic<std::size_t>& count; ~Finish() { count.fetch_sub(1, std::memory_order_release); } } finish{writers};
        if (phase != Phase::Recording && phase != Phase::Draining) return false;
        auto* found = thread(event.thread);
        if (found == nullptr) {
            unassignedLoss();
            return false;
        }
        std::lock_guard lock(found->mutex);
        if (phase == Phase::Frozen) return false;
        return storeLocked(found, event);
    }
};

Session::Session(Options options) : state(std::make_unique<State>(std::move(options))) {
    const auto& config = state->options;
    if (!canonicalUuid(config.runId) || !canonicalUuid(config.captureId) || config.runId == config.captureId)
        throw std::invalid_argument("draw trace requires distinct canonical run and capture UUIDs");
    if (config.length == 0 || config.recordsPerThread == 0 || config.recordsPerThread > 8388608 || config.maximumThreads == 0
        || config.maximumMetadata == 0 || config.maximumQueries == 0 || config.maximumBoundaries == 0
        || config.maximumEvents == 0 || config.maximumEvents > 8388608 || config.drainTimeout.count() < 0
        || config.maximumJournalBytes == 0 || config.maximumJournalBytes > std::size_t{16} * 1024u * 1024u * 1024u
        || config.journalInterval.count() < 1 || config.journalInterval.count() > 1000)
        throw std::invalid_argument("draw trace capacities and window length must be positive");
}

Session::~Session() = default;

Id Session::RegisterThread(std::string name, Clock::time_point time) {
    std::lock_guard lock(state->mutex);
    if (state->phase == State::Phase::Frozen) return 0;
    if (state->threads.size() >= state->options.maximumThreads) {
        state->unassignedLoss();
        return 0;
    }
    auto thread = std::make_unique<State::Thread>();
    thread->id = allocateId();
    thread->nativeId = nativeThreadId();
    thread->name = std::move(name);
    thread->registered = thread->last = time;
    thread->chunks.reserve((state->options.recordsPerThread - 1) / State::ChunkRecords + 1);
    state->allocatedIndexBytes += thread->chunks.capacity() * sizeof(std::unique_ptr<Event[]>);
    const auto id = thread->id;
    state->publishedThreads[state->threads.size()].store(thread.get(), std::memory_order_release);
    state->threads.push_back(std::move(thread));
    state->publishedCount.store(state->threads.size(), std::memory_order_release);
    return id;
}

CpuSample Session::SampleCpu() const {
    if (!state->options.threadCpu) return CpuSample{0, 0, 0, CpuStatus::NotRequested};
    try { return state->options.sampleCpu != nullptr ? state->options.sampleCpu() : platformCpu(); }
    catch (...) { return CpuSample{0, 0, 0, CpuStatus::Failed}; }
}

CycleSample Session::SampleCycles() const {
    if (!state->options.threadCycles) return CycleSample{0, 0, CpuStatus::NotRequested};
    try { return state->options.sampleCycles != nullptr ? state->options.sampleCycles() : platformCycles(); }
    catch (...) { return CycleSample{0, 0, CpuStatus::Failed}; }
}

void Session::Boundary(std::uint64_t present, Clock::time_point time) {
    std::lock_guard lock(state->mutex);
    if (state->phase == State::Phase::Frozen || state->phase == State::Phase::Draining) return;
    if (!state->first) state->first = time;
    if (state->phase == State::Phase::Armed) {
        const auto elapsed = time - *state->first;
        if (state->options.frames ? present < state->options.start
            : std::chrono::duration<double>(elapsed).count() < static_cast<double>(state->options.start)) return;
        state->opened = time;
        state->openPresent = present;
        state->phase = State::Phase::Recording;
    }
    if (!state->boundaries.empty() && (present <= state->boundaries.back().first || time < state->boundaries.back().second)) {
        state->interrupted = true;
        state->closeReason = "invalid_present_sequence";
        state->closed = time;
        state->phase = State::Phase::Draining;
        return;
    }
    if (state->boundaries.size() >= state->options.maximumBoundaries) {
        state->unassignedLoss();
        state->interrupted = true;
        state->closeReason = "boundary_capacity";
        state->closed = time;
        state->phase = State::Phase::Draining;
        return;
    }
    state->boundaries.emplace_back(present, time);
    if (state->options.frames ? present - *state->openPresent >= state->options.length
        : std::chrono::duration<double>(time - *state->opened).count() >= static_cast<double>(state->options.length)) {
        state->closed = time;
        state->closePresent = present;
        state->closeReason = "window_closed";
        state->phase = State::Phase::Draining;
    }
}

bool Session::Admit(EventKind kind, Id subject, bool continuation) {
    state->admissions.fetch_add(1, std::memory_order_acq_rel);
    struct Finish { std::atomic<std::size_t>& count; ~Finish() { count.fetch_sub(1, std::memory_order_release); } } finish{state->admissions};
    if (subject == 0 || (state->phase != State::Phase::Recording
        && !(continuation && state->phase == State::Phase::Draining))) return false;
    auto& pending = kind == EventKind::Job ? state->jobs : state->spans;
    if (pending.size() >= state->options.maximumQueries) {
        state->unassignedLoss();
        return false;
    }
    if (!pending.insert(subject)) {
        state->unassignedLoss();
        return false;
    }
    return true;
}

void Session::Release(EventKind kind, Id subject) {
    state->admissions.fetch_add(1, std::memory_order_acq_rel);
    struct Finish { std::atomic<std::size_t>& count; ~Finish() { count.fetch_sub(1, std::memory_order_release); } } finish{state->admissions};
    if (state->phase == State::Phase::Frozen) return;
    auto& pending = kind == EventKind::Job ? state->jobs : state->spans;
    pending.erase(subject);
}

bool Session::Store(Event event) {
    return state->store(event);
}

bool Session::Observe(Event event) {
    state->writers.fetch_add(1, std::memory_order_acq_rel);
    struct Finish { std::atomic<std::size_t>& count; ~Finish() { count.fetch_sub(1, std::memory_order_release); } } finish{state->writers};
    if (state->phase != State::Phase::Armed && state->phase != State::Phase::Recording) return false;
    auto* thread = state->thread(event.thread);
    if (thread == nullptr) { state->unassignedLoss(); return false; }
    std::lock_guard lock(thread->mutex);
    if (state->phase != State::Phase::Armed && state->phase != State::Phase::Recording) return false;
    for (auto& observation : thread->observations) {
        if (!observation) { observation = event; return true; }
    }
    state->unassignedLoss();
    return false;
}

void Session::CompleteObservation(Event event) {
    std::lock_guard lifecycle(state->mutex);
    auto* thread = state->thread(event.thread);
    if (thread == nullptr) return;
    std::lock_guard lock(thread->mutex);
    if (state->phase == State::Phase::Frozen) return;
    for (auto& observation : thread->observations) {
        if (observation && observation->subject == event.subject) {
            observation.reset();
            if (!state->opened) return;
            if (event.begin < *state->opened) {
                event.begin = *state->opened;
                event.status = Status::Unknown;
                copyText(event.text.data(), event.text.size(), "clipped_at_window_open");
                if (event.cpuStatus != CpuStatus::NotRequested) event.cpuStatus = CpuStatus::Clipped;
                if (event.cyclesStatus != CpuStatus::NotRequested) event.cyclesStatus = CpuStatus::Clipped;
            }
            if (state->closed && event.end > *state->closed) {
                event.end = *state->closed;
                event.status = Status::Unknown;
                copyText(event.text.data(), event.text.size(), "clipped_at_window_boundary");
                if (event.cpuStatus != CpuStatus::NotRequested) event.cpuStatus = CpuStatus::Clipped;
                if (event.cyclesStatus != CpuStatus::NotRequested) event.cyclesStatus = CpuStatus::Clipped;
            }
            if (event.end < event.begin) return;
            state->storeLocked(thread, event);
            return;
        }
    }
}

void Session::ReportLoss() {
    state->writers.fetch_add(1, std::memory_order_acq_rel);
    struct Finish { std::atomic<std::size_t>& count; ~Finish() { count.fetch_sub(1, std::memory_order_release); } } finish{state->writers};
    if (state->phase != State::Phase::Frozen) state->unassignedLoss();
}

Id Session::Metadata(const char* kind, std::string key, std::vector<std::pair<std::string, std::string>> fields, std::vector<Id> references) {
    std::lock_guard lock(state->mutex);
    if (state->phase == State::Phase::Frozen || kind == nullptr || *kind == '\0' || key.empty()) return 0;
    std::sort(fields.begin(), fields.end());
    if (std::adjacent_find(fields.begin(), fields.end(), [](const auto& a, const auto& b) { return a.first == b.first; }) != fields.end()) {
        ++state->lostMetadata;
        return 0;
    }
    const auto identity = std::make_pair(std::string(kind), key);
    const auto found = state->metadataKeys.find(identity);
    if (found != state->metadataKeys.end()) {
        const auto& entry = state->metadata[found->second];
        if (entry.fields != fields || entry.references != references) {
            ++state->lostMetadata;
            return 0;
        }
        return entry.id;
    }
    if (state->metadata.size() >= state->options.maximumMetadata || fields.size() > 64 || references.size() > 64
        || key.size() > 4096 || std::any_of(fields.begin(), fields.end(), [](const auto& field) {
            return field.first.empty() || field.first.size() > 128 || field.second.size() > 4096;
        })) {
        ++state->lostMetadata;
        return 0;
    }
    const auto id = allocateId();
    state->metadata.push_back({id, kind, std::move(key), std::move(fields), std::move(references)});
    try { state->metadataKeys.emplace(identity, state->metadata.size() - 1); }
    catch (...) { state->metadata.pop_back(); throw; }
    return id;
}

Id Session::RegisterQuery(Id batch, std::uint32_t slot, Id operation, const char* kind, bool shared, Id thread, Clock::time_point time) {
    std::lock_guard lock(state->queryMutex);
    const auto context = Current();
    if (state->phase != State::Phase::Recording && !(state->phase == State::Phase::Draining
        && (state->spans.contains(operation) || (context.job != 0 && state->jobs.contains(context.job))))) return 0;
    if (state->queries.size() >= state->options.maximumQueries) {
        ++state->lostQueries;
        return 0;
    }
    Event event;
    event.subject = allocateId();
    event.kind = EventKind::GpuQuery;
    event.thread = thread;
    event.related = batch;
    event.context = Current();
    event.context.batch = batch;
    event.context.operation = operation;
    event.begin = time;
    event.name = kind;
    event.slot = slot;
    event.shared = shared;
    copyText(event.nameStorage.data(), event.nameStorage.size(), kind);
    event.name = nullptr;
    for (std::size_t offset = 0; offset < state->queryIndexes.size(); ++offset) {
        auto& index = state->queryIndexes[(event.subject + offset) % state->queryIndexes.size()];
        if (index == std::numeric_limits<std::size_t>::max()) {
            index = state->queries.size();
            break;
        }
    }
    state->queries.push_back(event);
    ++state->unfinishedQueries;
    state->store(event);
    return event.subject;
}

void Session::ResolveQuery(Id query, QueryStatus status, double beginNs, double endNs, Id thread, Clock::time_point time) {
    std::lock_guard lock(state->queryMutex);
    if (state->phase == State::Phase::Frozen) return;
    auto* found = state->query(query);
    if (found == nullptr || found->queryStatus != QueryStatus::Pending || status == QueryStatus::Pending) return;
    if (status == QueryStatus::Available && (!std::isfinite(beginNs) || !std::isfinite(endNs) || beginNs < 0 || endNs < beginNs)) status = QueryStatus::InvalidTimestamp;
    auto event = *found;
    event.id = 0;
    event.thread = thread;
    event.begin = time;
    event.queryStatus = status;
    event.first = beginNs;
    event.second = endNs;
    event.fourth = endNs - beginNs;
    found->queryStatus = status;
    found->first = beginNs;
    found->second = endNs;
    --state->unfinishedQueries;
    if (status != QueryStatus::Available) ++state->queryFailures;
    state->store(event);
}

void Session::ResolveQueryTicks(Id query, QueryStatus status, std::uint64_t beginTicks, std::uint64_t endTicks, double periodNs, Id thread, Clock::time_point time, std::uint32_t validBits) {
    std::lock_guard lock(state->queryMutex);
    if (state->phase == State::Phase::Frozen) return;
    auto* found = state->query(query);
    if (found == nullptr || found->queryStatus != QueryStatus::Pending || status == QueryStatus::Pending) return;
    const auto delta = endTicks - beginTicks;
    const double duration = static_cast<double>(delta) * periodNs;
    const double beginNs = static_cast<double>(beginTicks) * periodNs;
    const double endNs = beginNs + duration;
    if (status == QueryStatus::Available && validBits != 64) status = QueryStatus::Unsupported;
    if (status == QueryStatus::Available && (delta >= (std::uint64_t{1} << 63u) || !std::isfinite(periodNs) || periodNs <= 0
        || !std::isfinite(duration) || !std::isfinite(beginNs) || !std::isfinite(endNs))) status = QueryStatus::InvalidTimestamp;
    auto event = *found;
    event.id = 0;
    event.thread = thread;
    event.begin = time;
    event.queryStatus = status;
    event.rawTicks = true;
    event.beginTicks = beginTicks;
    event.endTicks = endTicks;
    event.first = beginNs;
    event.second = endNs;
    event.third = periodNs;
    event.fourth = duration;
    event.value = validBits;
    found->queryStatus = status;
    --state->unfinishedQueries;
    if (status != QueryStatus::Available) ++state->queryFailures;
    state->store(event);
}

void Session::RequestStop(std::string reason, Clock::time_point time) {
    std::lock_guard lock(state->mutex);
    if (state->phase == State::Phase::Frozen) return;
    if (state->phase != State::Phase::Draining) {
        state->closed = time;
        state->closeReason = std::move(reason);
        state->interrupted = true;
        state->phase = State::Phase::Draining;
    }
}

bool Session::Ready(Clock::time_point time) const {
    std::lock_guard lock(state->mutex);
    return state->phase == State::Phase::Draining && state->admissions.load(std::memory_order_acquire) == 0
        && ((state->spans.empty() && state->jobs.empty() && state->unfinishedQueries == 0)
        || (state->closed && time - *state->closed >= state->options.drainTimeout));
}

bool Session::Recording() const {
    return state->phase == State::Phase::Recording;
}

bool Session::Draining() const {
    return state->phase == State::Phase::Draining;
}

bool Session::Frozen() const {
    return state->phase == State::Phase::Frozen;
}

void Session::Freeze(Clock::time_point time) {
    std::lock_guard lock(state->mutex);
    std::lock_guard queries(state->queryMutex);
    if (state->phase == State::Phase::Frozen) return;
    if (state->phase != State::Phase::Draining) {
        state->interrupted = true;
        state->closeReason = "forced_freeze";
        state->closed = time;
    }
    state->frozen = time;
    state->phase = State::Phase::Frozen;
    while (state->admissions.load(std::memory_order_acquire) != 0 || state->writers.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    for (const auto& thread : state->threads) {
        std::lock_guard threadLock(thread->mutex);
        for (auto& observation : thread->observations) {
            if (!observation) continue;
            auto event = *observation;
            event.end = state->closed.value_or(time);
            if (!state->opened || event.end < *state->opened) { observation.reset(); continue; }
            if (event.begin < *state->opened) event.begin = *state->opened;
            if (event.end < event.begin) { observation.reset(); continue; }
            event.status = Status::Unknown;
            copyText(event.text.data(), event.text.size(), "clipped_at_window_close");
            if (event.cpuStatus != CpuStatus::NotRequested) event.cpuStatus = CpuStatus::Clipped;
            if (event.cyclesStatus != CpuStatus::NotRequested) event.cyclesStatus = CpuStatus::Clipped;
            state->storeLocked(thread.get(), event);
            observation.reset();
        }
    }
}

const Options& Session::Configuration() const {
    return state->options;
}

Id Session::Identity() const {
    return state->identity;
}

namespace {

std::string objectJson(std::initializer_list<std::pair<std::string_view, std::string>> fields) {
    std::string output = "{";
    bool comma = false;
    for (const auto& [key, value] : fields) {
        if (comma) output += ',';
        comma = true;
        output += quote(key) + ':' + value;
    }
    return output + '}';
}

std::string arrayJson(const std::vector<std::string>& items) {
    std::string output = "[";
    bool comma = false;
    for (const auto& item : items) {
        if (comma) output += ',';
        comma = true;
        output += item;
    }
    return output + ']';
}

}

namespace {
std::string canonicalEvent(const Event* event, Clock::time_point origin) {
    const auto relative = [&](Clock::time_point time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time - origin).count(); };
    const bool query = event->kind == EventKind::GpuQuery;
    const bool calibrated = event->kind == EventKind::Calibration;
    const bool command = event->kind == EventKind::HostCommand;
    const bool measured = query && event->queryStatus == QueryStatus::Available;
    const bool duration = event->end != Clock::time_point{} && event->end >= event->begin;
    const auto name = event->nameStorage[0] != '\0' ? quote(event->nameStorage.data()) : "null";
    auto data = objectJson({
            {"begin_gpu_ns", measured || calibrated ? finiteJson(event->first) : "null"},
            {"end_gpu_ns", measured ? finiteJson(event->second) : "null"},
            {"deviation_ns", calibrated ? finiteJson(event->second) : "null"},
            {"period_ns", calibrated || (measured && event->rawTicks) ? finiteJson(event->third) : "null"},
            {"valid_bits", calibrated ? std::to_string(event->value) : "null"},
            {"slot", query ? std::to_string(event->slot) : "null"},
            {"shared", query ? (event->shared ? "true" : "false") : "null"},
            {"requested_count", command ? std::to_string(event->slot) : "null"},
            {"count_dynamic", command ? (event->shared ? "true" : "false") : "null"},
            {"begin_gpu_ticks", measured && event->rawTicks ? quote(std::to_string(event->beginTicks)) : "null"},
            {"end_gpu_ticks", measured && event->rawTicks ? quote(std::to_string(event->endTicks)) : "null"},
            {"duration_ns", measured ? finiteJson(event->fourth) : "null"},
            {"duration_origin", measured ? quote(event->rawTicks ? "integer_tick_delta" : "approximate_endpoint_difference") : "null"},
            {"absolute_time_precision", measured || calibrated ? quote("approximate_binary64") : "null"},
            {"timestamp_valid_bits", measured && event->rawTicks ? std::to_string(event->value) : "null"},
            {"timestamp_wraps", measured && event->rawTicks ? (event->endTicks < event->beginTicks ? "true" : "false") : "null"},
            {"thread_cpu_status", quote(cpuName(event->cpuStatus))},
            {"thread_cpu_begin_ticks", event->cpuStatus == CpuStatus::Measured ? quote(std::to_string(event->cpuBegin.ticks)) : "null"},
            {"thread_cpu_end_ticks", event->cpuStatus == CpuStatus::Measured ? quote(std::to_string(event->cpuEnd.ticks)) : "null"},
            {"thread_cpu_frequency", event->cpuStatus == CpuStatus::Measured ? quote(std::to_string(event->cpuBegin.frequency)) : "null"},
            {"thread_cpu_native_id", event->cpuStatus == CpuStatus::Measured ? idJson(event->cpuBegin.nativeThread) : "null"},
            {"thread_cpu_ns", event->cpuStatus == CpuStatus::Measured ? finiteJson(static_cast<double>(event->cpuEnd.ticks - event->cpuBegin.ticks) * (1000000000.0 / static_cast<double>(event->cpuBegin.frequency))) : "null"},
            {"calibration_gpu_ticks", calibrated && event->calibration.available ? quote(std::to_string(event->calibration.gpuTicks)) : "null"},
            {"calibration_host_ticks", calibrated && event->calibration.available ? quote(std::to_string(event->calibration.hostTicks)) : "null"},
            {"calibration_conversion_host_ticks", calibrated && event->calibration.available ? quote(std::to_string(event->calibration.conversionHostTicks)) : "null"},
            {"calibration_host_frequency", calibrated && event->calibration.available ? quote(std::to_string(event->calibration.hostFrequency)) : "null"},
            {"calibration_host_domain", calibrated ? quote(event->calibrationDomain[0] != '\0' ? event->calibrationDomain.data() : "unknown") : "null"},
            {"calibration_steady_before_ns", calibrated && event->calibration.available ? std::to_string(relative(event->calibration.before)) : "null"},
            {"calibration_steady_after_ns", calibrated && event->calibration.available ? std::to_string(relative(event->calibration.after)) : "null"},
            {"availability", event->kind == EventKind::Counter ? quote(event->shared ? queryName(event->queryStatus) : "observed") : "null"},
            {"counter_value", event->kind == EventKind::Counter && (!event->shared || event->queryStatus == QueryStatus::Available) ? quote(std::to_string(event->value)) : "null"},
            {"present_ordinal", event->kind == EventKind::Present && event->shared ? quote(std::to_string(event->value)) : "null"},
            {"present_ordinal_status", event->kind == EventKind::Present ? quote(event->shared ? "observed" : "unavailable") : "null"},
            {"completion_domain", event->kind == EventKind::Batch && std::string_view(event->nameStorage.data()) == "batch_submission"
                    ? quote(event->status == Status::Complete ? "gpu_fence_observed" : event->status == Status::Unknown
                        ? "native_queue_submit_returned" : event->status == Status::Failed ? "native_submit_or_fence_failure" : "cancelled_before_completion")
                    : "null"}
    });
    if (event->kind == EventKind::Span && event->cyclesStatus != CpuStatus::NotRequested) {
        const bool cyclesMeasured = event->cyclesStatus == CpuStatus::Measured;
        const auto cycles = objectJson({
            {"thread_cycles_status", quote(cpuName(event->cyclesStatus))},
            {"thread_cycles_begin", cyclesMeasured ? quote(std::to_string(event->cyclesBegin.cycles)) : "null"},
            {"thread_cycles_end", cyclesMeasured ? quote(std::to_string(event->cyclesEnd.cycles)) : "null"},
            {"thread_cycles_delta", cyclesMeasured ? quote(std::to_string(event->cyclesEnd.cycles - event->cyclesBegin.cycles)) : "null"},
            {"thread_cycles_native_id", cyclesMeasured ? idJson(event->cyclesBegin.nativeThread) : "null"},
            {"thread_cycles_unit", quote("cycles")}, {"thread_cycles_source", quote(cycleSource())}
        });
        data.pop_back();
        data += ',';
        data.append(cycles, 1, cycles.size() - 1);
    }
    return objectJson({
        {"id", idJson(event->id)}, {"kind", quote(kindName(event->kind))}, {"thread_id", idJson(event->thread)},
        {"ts_ns", std::to_string(relative(event->begin))}, {"end_ns", duration ? std::to_string(relative(event->end)) : "null"},
        {"subject", idJson(event->subject)}, {"related", idJson(event->related)}, {"name", name},
        {"text", event->text[0] != '\0' ? quote(event->text.data()) : "null"}, {"value", std::to_string(event->value)},
        {"status", quote(query ? queryName(event->queryStatus) : statusName(event->status))},
        {"context", contextJson(event->context)}, {"parent_span", idJson(event->parentSpan)}, {"data", data}
    });
}
}

std::string Session::Json() const {
    std::ostringstream output;
    WriteJson(output);
    return output.str();
}

bool Session::WriteJournal(std::ostream& output) {
    std::lock_guard journalLock(state->journalMutex);
    if (state->journalFooter || state->journalFailed || !state->options.journal) return true;
    Clock::time_point origin;
    {
        std::lock_guard lock(state->mutex);
        if (!state->opened) return false;
        origin = *state->opened;
    }
    const auto relative = [&](Clock::time_point time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time - origin).count(); };
    const auto emit = [&](const char* kind, const std::string& data) {
        const auto line = objectJson({{"schema_version", "1"}, {"document_type", quote("anyps5.draw-journal")},
            {"run_id", quote(state->options.runId)}, {"capture_id", quote(state->options.captureId)},
            {"pid", std::to_string(state->options.process)}, {"sequence", std::to_string(state->journalSequence)},
            {"kind", quote(kind)}, {"data", data}}) + '\n';
        const auto reserve = std::string_view(kind) == "footer" ? 0 : std::min<std::size_t>(4096, state->options.maximumJournalBytes / 4);
        const auto bytes = state->journalBytes.load();
        if (bytes > state->options.maximumJournalBytes || line.size() > state->options.maximumJournalBytes - bytes
            || reserve > state->options.maximumJournalBytes - bytes - line.size()) {
            state->journalBudget = true;
            state->journalStopped = true;
            return false;
        }
        output.write(line.data(), static_cast<std::streamsize>(line.size()));
        output.flush();
        if (!output) {
            state->journalFailed = true;
            state->journalStopped = true;
            return false;
        }
        state->journalBytes.fetch_add(line.size());
        ++state->journalSequence;
        return true;
    };
    try {
        if (!state->journalHeader) {
            state->journalHeader = emit("header", objectJson({{"time_unit", quote("ns")}, {"clock_domain", quote("steady_clock")},
                {"origin_steady_ns", quote(std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(origin.time_since_epoch()).count()))},
                {"unit", quote(state->options.frames ? "frames" : "seconds")}, {"start", std::to_string(state->options.start)},
                {"length", std::to_string(state->options.length)}, {"maximum_bytes", std::to_string(state->options.maximumJournalBytes)},
                {"batch_events", "4096"}, {"durability", quote("flushed_process_stream_not_power_loss_durable")},
                {"thread_cpu_requested", state->options.threadCpu ? "true" : "false"},
                {"thread_cycles_requested", state->options.threadCycles ? "true" : "false"},
                {"thread_cycles_unit", quote("cycles")}, {"thread_cycles_source", quote(cycleSource())},
                {"perfetto_view_requested", state->options.perfettoView ? "true" : "false"},
                {"native_anchor", objectJson({{"domain", quote(state->clockAnchor.hostDomain)},
                    {"ticks", state->clockAnchor.available ? quote(std::to_string(state->clockAnchor.hostTicks)) : "null"},
                    {"frequency", state->clockAnchor.available ? quote(std::to_string(state->clockAnchor.hostFrequency)) : "null"},
                    {"steady_before_ns", std::to_string(relative(state->clockAnchor.before))},
                    {"steady_after_ns", std::to_string(relative(state->clockAnchor.after))},
                    {"status", quote(state->clockAnchor.available ? "available" : "unavailable")}})}
            }));
        }
        Id registeredThread = 0;
        std::uint64_t registeredNative = 0;
        std::string registeredName;
        Clock::time_point registeredAt{};
        std::optional<State::MetadataEntry> metadata;
        std::optional<std::pair<std::uint64_t, Clock::time_point>> boundary;
        std::vector<const Event*> batch;
        batch.reserve(4096);
        Id selectedThread = 0;
        std::size_t selectedIndex = 0, firstIndex = 0;
        bool caughtUp = true;
        bool frozenSnapshot = false;
        if (!state->journalStopped) {
            std::lock_guard lock(state->mutex);
            const auto count = state->threads.size();
            frozenSnapshot = state->phase == State::Phase::Frozen;
            state->journalCursors.resize(count, 0);
            if (state->journalThread < count) {
                const auto& thread = *state->threads[state->journalThread++];
                registeredThread = thread.id;
                registeredNative = thread.nativeId;
                registeredName = thread.name;
                registeredAt = thread.registered;
            }
            if (state->journalMetadata < state->metadata.size()) {
                metadata = state->metadata[state->journalMetadata++];
            }
            if (state->journalBoundary < state->boundaries.size()) {
                boundary = state->boundaries[state->journalBoundary++];
            }
            caughtUp = state->journalThread == count && state->journalMetadata == state->metadata.size()
                && state->journalBoundary == state->boundaries.size();
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto index = (state->journalNextThread + offset) % count;
                if (index >= state->journalThread) continue;
                const auto& thread = *state->threads[index];
                std::lock_guard threadLock(thread.mutex);
                const auto first = state->journalCursors[index];
                if (first == thread.recordCount) continue;
                caughtUp = false;
                if (!batch.empty()) continue;
                selectedIndex = index;
                firstIndex = first;
                selectedThread = thread.id;
                const auto end = first + std::min<std::size_t>(4096, thread.recordCount - first);
                for (auto current = first; current < end; ++current) batch.push_back(&thread.record(current));
            }
            if (count != 0) state->journalNextThread = (selectedIndex + 1) % count;
        }
        if (registeredThread != 0 && !emit("thread", objectJson({{"thread_id", idJson(registeredThread)}, {"name", quote(registeredName)},
            {"native_thread_id", idJson(registeredNative)}, {"native_thread_id_source", quote(registeredNative != 0 ? nativeThreadSource() : "unavailable")},
            {"registered_ns", std::to_string(relative(registeredAt))}}))) state->journalStopped = true;
        if (!state->journalStopped && metadata) {
            std::string fields = "{";
            for (const auto& [key, value] : metadata->fields) {
                if (fields.size() != 1) fields += ',';
                fields += quote(key) + ':' + quote(value);
            }
            fields += '}';
            std::vector<std::string> references;
            for (Id id : metadata->references) references.push_back(idJson(id));
            if (!emit("dictionary", objectJson({{"id", idJson(metadata->id)}, {"kind", quote(metadata->kind)}, {"key", quote(metadata->key)},
                {"fields", fields}, {"references", arrayJson(references)}}))) state->journalStopped = true;
        }
        if (!state->journalStopped && boundary && !emit("boundary", objectJson({{"present", quote(std::to_string(boundary->first))},
            {"ts_ns", std::to_string(relative(boundary->second))}}))) state->journalStopped = true;
        if (!state->journalStopped && !batch.empty()) {
            std::string data = "{\"thread_id\":" + idJson(selectedThread) + ",\"first_index\":" + std::to_string(firstIndex)
                + ",\"count\":" + std::to_string(batch.size()) + ",\"events\":[";
            bool comma = false;
            for (const auto* event : batch) {
                if (comma) data += ',';
                comma = true;
                data += canonicalEvent(event, origin);
            }
            data += "]}";
            if (emit("events", data)) {
                state->journalCursors[selectedIndex] += batch.size();
                state->journalEvents += batch.size();
            }
        }
        if (!state->journalStopped) emit("checkpoint", objectJson({{"emitted_events", std::to_string(state->journalEvents)},
            {"losses", objectJson({{"events", std::to_string(state->lostEvents.load())}, {"metadata", std::to_string(state->lostMetadata.load())},
                {"gpu_queries", std::to_string(state->lostQueries.load())}})},
            {"pending_spans", std::to_string(state->spans.size())}, {"pending_jobs", std::to_string(state->jobs.size())},
            {"pending_gpu_queries", std::to_string(state->unfinishedQueries.load())},
            {"state", quote(Frozen() ? "frozen" : Draining() ? "draining" : "recording")}, {"complete", "false"}}));
        if (state->journalStopped || (frozenSnapshot && caughtUp)) {
            emit("footer", objectJson({{"emitted_events", std::to_string(state->journalEvents)},
                {"stop_reason", quote(state->journalFailed ? "io_failed" : state->journalBudget ? "byte_budget" : "complete")},
                {"complete", "false"}, {"final_sidecar_required", "true"}}));
            state->journalFooter = true;
            state->journalStopped = true;
            return true;
        }
        return caughtUp;
    } catch (...) {
        state->journalFailed = true;
        state->journalStopped = true;
        return true;
    }
}

void Session::FailJournal() {
    state->journalFailed = true;
    state->journalStopped = true;
}

bool Session::JournalFinished() const { return state->journalStopped.load(); }

void Session::WriteJson(std::ostream& output) const {
    {
        std::lock_guard lock(state->mutex);
        if (state->phase != State::Phase::Frozen) throw std::logic_error("draw trace must be frozen before serialization");
    }
    const auto origin = state->opened.value_or(state->first.value_or(state->closed.value_or(Clock::time_point{})));
    const auto relative = [&](Clock::time_point time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time - origin).count(); };
    const auto nullableTime = [&](const std::optional<Clock::time_point>& time) { return time ? std::to_string(relative(*time)) : "null"; };
    const bool exactBoundaries = state->openPresent && state->closePresent && (!state->options.frames
        || (*state->openPresent == state->options.start && *state->closePresent - *state->openPresent == state->options.length
            && state->boundaries.size() - 1 == state->options.length));
    const bool complete = exactBoundaries && !state->interrupted && state->spans.empty() && state->jobs.empty()
        && state->unfinishedQueries == 0 && state->lostEvents == 0 && state->lostMetadata == 0
        && state->lostQueries == 0 && state->queryFailures == 0;
    const auto completion = quote(complete ? "complete" : "partial");
    std::vector<const Event*> events;
    events.reserve(state->retainedEvents.load());
    for (const auto& pointer : state->threads) {
        const auto& thread = *pointer;
        for (std::size_t index = 0; index < thread.recordCount; ++index) events.push_back(&thread.record(index));
    }
    std::sort(events.begin(), events.end(), [](const Event* a, const Event* b) { return a->id < b->id; });
    const auto perfettoEvent = [&](const Event* event) {
        const bool duration = event->end != Clock::time_point{} && event->end >= event->begin;
        const auto args = objectJson({{"event_id", idJson(event->id)}, {"subject", idJson(event->subject)}, {"kind", quote(kindName(event->kind))}});
        const auto eventName = event->nameStorage[0] != '\0' ? std::string_view(event->nameStorage.data()) : kindName(event->kind);
        if (duration) {
            return objectJson({{"name", quote(eventName)}, {"ph", quote("X")},
                {"pid", std::to_string(state->options.process)}, {"tid", std::to_string(event->thread)},
                {"ts", finiteJson(static_cast<double>(relative(event->begin)) / 1000.0)},
                {"dur", finiteJson(std::chrono::duration<double, std::micro>(event->end - event->begin).count())}, {"args", args}});
        }
        return objectJson({{"name", quote(eventName)}, {"ph", quote("i")},
                {"pid", std::to_string(state->options.process)}, {"tid", std::to_string(event->thread)},
                {"ts", finiteJson(static_cast<double>(relative(event->begin)) / 1000.0)}, {"s", quote("t")}, {"args", args}});
    };
    const auto header = objectJson({
        {"schema_version", "1"}, {"document_type", quote("anyps5.draw-trace")},
        {"run_id", quote(state->options.runId)}, {"capture_id", quote(state->options.captureId)},
        {"producer", objectJson({{"pid", std::to_string(state->options.process)}, {"revision", quote(state->options.revision)},
            {"dirty", state->options.dirty ? (*state->options.dirty ? "true" : "false") : "null"},
            {"coverage", quote("selected instrumented paths only")}})},
        {"clock", objectJson({
            {"domain", quote("steady_clock")}, {"time_unit", quote("ns")},
            {"origin_steady_ns", quote(std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(origin.time_since_epoch()).count()))},
            {"native_anchor", objectJson({{"domain", quote(state->clockAnchor.hostDomain)},
                {"ticks", state->clockAnchor.available ? quote(std::to_string(state->clockAnchor.hostTicks)) : "null"},
                {"frequency", state->clockAnchor.available ? quote(std::to_string(state->clockAnchor.hostFrequency)) : "null"},
                {"steady_before_ns", std::to_string(relative(state->clockAnchor.before))},
                {"steady_after_ns", std::to_string(relative(state->clockAnchor.after))},
                {"status", quote(state->clockAnchor.available ? "available" : "unavailable")}
            })}
        })},
        {"capture", objectJson({
            {"unit", quote(state->options.frames ? "frames" : "seconds")}, {"start", std::to_string(state->options.start)},
            {"length", std::to_string(state->options.length)}, {"state", completion},
            {"trace", quote("aps5-trace-" + state->options.revision + "-" + std::to_string(state->options.process) + ".json.gz")},
            {"open_present", state->openPresent ? quote(std::to_string(*state->openPresent)) : "null"},
            {"close_present", state->closePresent ? quote(std::to_string(*state->closePresent)) : "null"},
            {"open_ns", nullableTime(state->opened)}, {"close_ns", nullableTime(state->closed)},
            {"drain_deadline_ms", std::to_string(state->options.drainTimeout.count())}, {"close_reason", quote(state->closeReason)},
            {"pending_spans", std::to_string(state->spans.size())}, {"pending_jobs", std::to_string(state->jobs.size())},
            {"pending_gpu_queries", std::to_string(state->unfinishedQueries)}
        })}
    });
    output.write(header.data(), static_cast<std::streamsize>(header.size() - 1));
    output << ",\"boundaries\":[";
    bool comma = false;
    const auto separate = [&] { if (comma) output << ','; comma = true; };
    for (const auto& [present, time] : state->boundaries) {
        separate();
        output << objectJson({{"present", quote(std::to_string(present))}, {"ts_ns", std::to_string(relative(time))}});
    }
    output << "],\"threads\":[";
    comma = false;
    for (const auto& pointer : state->threads) {
        const auto& thread = *pointer;
        separate();
        output << objectJson({
            {"thread_id", idJson(thread.id)}, {"name", quote(thread.name)}, {"attempted", std::to_string(thread.attempted)},
            {"native_thread_id", idJson(thread.nativeId)}, {"native_thread_id_source", quote(thread.nativeId != 0 ? nativeThreadSource() : "unavailable")},
            {"emitted", std::to_string(thread.recordCount)}, {"lost", std::to_string(thread.lost)},
            {"registered_ns", std::to_string(relative(thread.registered))}, {"last_event_ns", std::to_string(relative(thread.last))},
            {"acknowledged", state->spans.empty() && state->jobs.empty() ? "true" : "false"}
        });
    }
    output << "],\"dictionaries\":[";
    comma = false;
    for (const auto& entry : state->metadata) {
        separate();
        output << "{\"id\":" << idJson(entry.id) << ",\"kind\":" << quote(entry.kind)
               << ",\"key\":" << quote(entry.key) << ",\"fields\":{";
        bool fieldComma = false;
        for (const auto& [key, value] : entry.fields) {
            if (fieldComma) output << ',';
            fieldComma = true;
            output << quote(key) << ':' << quote(value);
        }
        output << "},\"references\":[";
        fieldComma = false;
        for (Id id : entry.references) {
            if (fieldComma) output << ',';
            fieldComma = true;
            output << idJson(id);
        }
        output << "]}";
    }
    output << "],\"events\":[";
    comma = false;
    for (const auto* event : events) { separate(); output << canonicalEvent(event, origin); }
    output << ']';
    if (state->options.perfettoView) {
        output << ",\"traceEvents\":[";
        comma = false;
        for (const auto& pointer : state->threads) {
            separate();
            output << objectJson({{"name", quote("thread_name")}, {"ph", quote("M")},
                {"pid", std::to_string(state->options.process)}, {"tid", std::to_string(pointer->id)},
                {"args", objectJson({{"name", quote(pointer->name)}})}});
        }
        for (const auto* event : events) { separate(); output << perfettoEvent(event); }
        output << ']';
    }
    output << ',';
    const auto footer = objectJson({
        {"losses", objectJson({{"events", std::to_string(state->lostEvents.load())}, {"unassigned_events", std::to_string(state->unassignedEvents.load())},
            {"metadata", std::to_string(state->lostMetadata.load())}, {"gpu_queries", std::to_string(state->lostQueries.load())}})},
        {"coverage", objectJson({
            {"scope", quote("selected instrumented paths only")}, {"full_process_coverage", "false"},
            {"query_failures", std::to_string(state->queryFailures)}, {"records_per_thread", std::to_string(state->options.recordsPerThread)},
            {"records_per_thread_scope", quote("per_thread")}, {"maximum_events_scope", quote("all_threads")},
            {"maximum_threads", std::to_string(state->options.maximumThreads)}, {"maximum_queries", std::to_string(state->options.maximumQueries)},
            {"maximum_metadata", std::to_string(state->options.maximumMetadata)}, {"maximum_record_bytes", std::to_string(state->options.maximumBytes)},
            {"maximum_boundaries", std::to_string(state->options.maximumBoundaries)},
            {"allocated_record_bytes", std::to_string(state->allocatedBytes.load())},
            {"maximum_events", std::to_string(state->options.maximumEvents)},
            {"chunk_records", std::to_string(State::ChunkRecords)},
            {"allocated_index_bytes", std::to_string(state->allocatedIndexBytes)},
            {"writer_index_bytes", std::to_string(events.capacity() * sizeof(const Event*))},
            {"allocated_query_bytes", std::to_string(state->queries.capacity() * sizeof(Event))},
            {"budget_scope", quote("stored_event_chunk_payloads")},
            {"event_record_size_bytes", std::to_string(sizeof(Event))},
            {"thread_cpu_requested", state->options.threadCpu ? "true" : "false"},
            {"thread_cpu_source", quote(cpuSource())},
            {"thread_cpu_scope", quote("inclusive_selected_scope_native_thread_user_plus_kernel")},
            {"thread_cpu_counter_unit", quote(std::string_view(cpuSource()) == "get_thread_times" ? "100ns_ticks"
                : std::string_view(cpuSource()) == "clock_thread_cputime_id" ? "ns_ticks" : "unavailable")},
            {"thread_cpu_resolution_ns", "null"},
            {"thread_cpu_units_are_resolution", "false"},
            {"thread_cycles_requested", state->options.threadCycles ? "true" : "false"},
            {"thread_cycles_source", quote(cycleSource())}, {"thread_cycles_unit", quote("cycles")},
            {"thread_cycles_scope", quote("inclusive_selected_scope_native_thread_user_plus_kernel_including_profiler")},
            {"thread_cycles_time_conversion", quote("unavailable")}, {"thread_cycles_resolution", quote("unknown")},
            {"perfetto_view_included", state->options.perfettoView ? "true" : "false"},
            {"clock_alignment", quote("raw_anchors_only_unverified")},
            {"capabilities", objectJson({{"selected_cpu_scopes", "true"}, {"native_thread_ids", quote(nativeThreadSource())},
                {"thread_cpu", quote(state->options.threadCpu ? "requested" : "not_requested")},
                {"thread_cycles", quote(state->options.threadCycles ? "requested" : "not_requested")},
                {"guest_function_coverage", quote("unavailable")}, {"gpu_shader_stage_cost", quote("unavailable")},
                {"async_external_origins", quote("explicit_references_may_remain_unknown")}})},
            {"journal", objectJson({{"requested", state->options.journal ? "true" : "false"},
                {"bytes", std::to_string(state->journalBytes.load())}, {"maximum_bytes", std::to_string(state->options.maximumJournalBytes)},
                {"interval_ms", std::to_string(state->options.journalInterval.count())},
                {"failed", state->journalFailed ? "true" : "false"}, {"byte_budget_exhausted", state->journalBudget ? "true" : "false"},
                {"committed_events", std::to_string(state->journalEvents)},
                {"recovery_complete_capture", "false"}, {"power_loss_durable", "false"}})}
        })},
        {"limitations", arrayJson({
            quote("Only explicitly instrumented paths and registered threads are observed; full process and function coverage is unknown."),
            quote("CPU elapsed durations are wall time. Optional native thread CPU counters are inclusive selected-scope user plus kernel time, with unknown resolution; they do not separate blocking and preemption."),
            quote("Optional native thread cycle counts are inclusive user plus kernel work on selected scopes, include profiler cost, and are not converted to elapsed time or exclusive uninstrumented CPU cost."),
            quote("Canonical events are the evidence ledger. The redundant Perfetto view is omitted unless explicitly requested; its absence is not lost evidence."),
            quote("The record memory budget covers stored event chunk payloads only. Per-thread record limits and the global retained-event limit are distinct; indexes, queries, metadata and whole-process allocations are outside that payload budget."),
            quote("Graph references and queue order do not prove exclusive frame ownership or a critical path."),
            quote("Shader stage identities associated with a whole draw do not measure individual shader-stage execution."),
            quote("Unavailable, dropped or unresolved GPU queries are unknown work, not zero duration."),
            quote("Runtime source-to-binary binding and observed loaded modules are not established by this trace.")
        })},
        {"completion", objectJson({{"state", completion}, {"event_count", std::to_string(events.size())}, {"output_complete", "true"}})}
    });
    output.write(footer.data() + 1, static_cast<std::streamsize>(footer.size() - 1));
    output << '\n';
}

namespace {

std::uint64_t parseNumber(std::string_view text, const char* name) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument(std::string(name) + " must be an unsigned integer");
    return value;
}

Options environmentOptions() {
    Options options;
    const char* run = std::getenv("APS5_CAPTURE_RUN_ID");
    if (run == nullptr || !canonicalUuid(run)) throw std::invalid_argument("APS5_TRACE_DETAIL=draw requires canonical APS5_CAPTURE_RUN_ID");
    options.runId = run;
    options.captureId = randomUuid();
#ifdef APS5_TRACE_COMMIT
    options.revision = APS5_TRACE_COMMIT;
#endif
#ifdef APS5_TRACE_DIRTY
    if (APS5_TRACE_DIRTY >= 0) options.dirty = APS5_TRACE_DIRTY != 0;
#endif
#ifdef _WIN32
    options.process = static_cast<std::uint64_t>(_getpid());
#else
    options.process = static_cast<std::uint64_t>(getpid());
#endif
    const char* frames = std::getenv("APS5_TRACE_FRAMES");
    const char* seconds = std::getenv("APS5_TRACE_SECONDS");
    if ((frames == nullptr) == (seconds == nullptr)) throw std::invalid_argument("draw detail requires exactly one explicit trace window");
    options.frames = frames != nullptr;
    const std::string_view spec = options.frames ? frames : seconds;
    const auto colon = spec.find(':');
    if (colon == std::string_view::npos) throw std::invalid_argument("draw detail window must be start:length");
    options.start = parseNumber(spec.substr(0, colon), "draw window start");
    options.length = parseNumber(spec.substr(colon + 1), "draw window length");
    if (const char* capacity = std::getenv("APS5_DRAW_TRACE_EVENTS")) {
        const auto value = parseNumber(capacity, "APS5_DRAW_TRACE_EVENTS");
        if (value == 0 || value > 8388608) throw std::invalid_argument("APS5_DRAW_TRACE_EVENTS must be between 1 and 8388608");
        options.recordsPerThread = static_cast<std::size_t>(value);
    }
    if (const char* capacity = std::getenv("APS5_DRAW_TRACE_METADATA")) {
        const auto value = parseNumber(capacity, "APS5_DRAW_TRACE_METADATA");
        if (value == 0 || value > 262144) throw std::invalid_argument("APS5_DRAW_TRACE_METADATA must be between 1 and 262144");
        options.maximumMetadata = static_cast<std::size_t>(value);
    }
    if (const char* capacity = std::getenv("APS5_DRAW_TRACE_QUERIES")) {
        const auto value = parseNumber(capacity, "APS5_DRAW_TRACE_QUERIES");
        if (value == 0 || value > 1048576) throw std::invalid_argument("APS5_DRAW_TRACE_QUERIES must be between 1 and 1048576");
        options.maximumQueries = static_cast<std::size_t>(value);
    }
    if (const char* capacity = std::getenv("APS5_DRAW_TRACE_MEMORY_MB")) {
        const auto value = parseNumber(capacity, "APS5_DRAW_TRACE_MEMORY_MB");
        constexpr std::size_t megabyte = 1024u * 1024u;
        if (value < 64 || value > 8192 || value > std::numeric_limits<std::size_t>::max() / megabyte)
            throw std::invalid_argument("APS5_DRAW_TRACE_MEMORY_MB must be between 64 and 8192 and fit the host size type");
        options.maximumBytes = static_cast<std::size_t>(value) * megabyte;
    }
    const auto flag = [](const char* name) {
        const char* value = std::getenv(name);
        if (value == nullptr) return false;
        if (std::string_view(value) != "0" && std::string_view(value) != "1") throw std::invalid_argument(std::string(name) + " must be 0 or 1");
        return std::string_view(value) == "1";
    };
    options.threadCpu = flag("APS5_DRAW_TRACE_THREAD_CPU");
    options.threadCycles = flag("APS5_DRAW_TRACE_THREAD_CYCLES");
    options.perfettoView = flag("APS5_DRAW_TRACE_PERFETTO");
    options.journal = flag("APS5_DRAW_TRACE_JOURNAL");
    return options;
}

struct Global {
    Global() : session(environmentOptions()) {}
    Session session;
    std::mutex mutex;
    std::thread writer;
    std::thread journalWriter;
    std::string failure;
    bool started = false;
};

Global& global() {
    static auto* value = new Global();
    return *value;
}

Session& activeSession() {
    return boundSession != nullptr ? *boundSession : global().session;
}

Id threadId() {
    if (currentThread == 0) {
        try { currentThread = activeSession().RegisterThread("instrumented thread", now()); }
        catch (...) { activeSession().ReportLoss(); }
    }
    return currentThread;
}

Event makeEvent(EventKind kind, Id subject, Id related, const char* name = nullptr) {
    Event event;
    event.kind = kind;
    event.subject = subject;
    event.related = related;
    event.context = currentContext;
    event.parentSpan = currentSpan;
    event.thread = threadId();
    event.begin = now();
    event.name = name;
    return event;
}

void publish(Global& owner) {
    const auto& options = owner.session.Configuration();
    const char* configured = std::getenv("APS5_TRACE_DIR");
    const auto directory = std::filesystem::path(configured != nullptr ? configured : ".");
    const auto name = "aps5-draw-" + options.runId + "-" + std::to_string(options.process) + ".json";
    const auto destination = directory / name;
    const auto temporary = directory / ("." + name + "." + options.captureId + ".tmp");
    std::filesystem::create_directories(directory);
    try {
        {
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(temporary, std::ios::binary | std::ios::trunc);
            owner.session.WriteJson(file);
            file.flush();
            file.close();
        }
        std::filesystem::create_hard_link(temporary, destination);
        std::filesystem::remove(temporary);
        std::fprintf(stderr, "[draw-trace] wrote %s\n", name.c_str());
        std::fflush(stderr);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void startJournal() {
    if (boundSession != nullptr) return;
    auto& owner = global();
    if (!owner.session.Configuration().journal) return;
    std::lock_guard lock(owner.mutex);
    if (owner.journalWriter.joinable() || owner.started) return;
    try {
        owner.journalWriter = std::thread([&owner] {
            try {
                const auto& options = owner.session.Configuration();
                const char* configured = std::getenv("APS5_TRACE_DIR");
                const auto directory = std::filesystem::path(configured != nullptr ? configured : ".");
                const auto name = "aps5-draw-" + options.runId + "-" + std::to_string(options.process) + ".journal.jsonl";
                std::filesystem::create_directories(directory);
                const auto temporary = directory / ("." + name + "." + options.captureId + ".tmp");
                const auto destination = directory / name;
                {
                    std::ofstream create;
                    create.exceptions(std::ios::failbit | std::ios::badbit);
                    create.open(temporary, std::ios::binary | std::ios::trunc);
                    create.close();
                }
                std::filesystem::create_hard_link(temporary, destination);
                std::filesystem::remove(temporary);
                std::ofstream file;
                file.exceptions(std::ios::failbit | std::ios::badbit);
                file.open(destination, std::ios::binary | std::ios::app);
                for (;;) {
                    const bool caughtUp = owner.session.WriteJournal(file);
                    if (owner.session.JournalFinished()) break;
                    if (caughtUp) std::this_thread::sleep_for(options.journalInterval);
                    else std::this_thread::yield();
                }
                file.close();
            } catch (...) {
                owner.session.FailJournal();
            }
        });
    } catch (...) { owner.session.FailJournal(); }
}

void startWriter() {
    if (boundSession != nullptr) return;
    auto& owner = global();
    std::lock_guard lock(owner.mutex);
    if (owner.started) return;
    owner.writer = std::thread([&owner] {
        try {
            while (!owner.session.Ready(Clock::now())) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            owner.session.Freeze(Clock::now());
            if (owner.journalWriter.joinable()) owner.journalWriter.join();
            publish(owner);
        } catch (const std::exception& error) {
            std::lock_guard failureLock(owner.mutex);
            owner.failure = error.what();
            std::fprintf(stderr, "[draw-trace] output failed: %s\n", owner.failure.c_str());
            std::fflush(stderr);
        }
    });
    owner.started = true;
}

}

SessionScope::SessionScope(Session& session, Clock::time_point (*clock)())
    : previous(boundSession), previousClock(boundClock), previousContext(currentContext),
      previousSpan(currentSpan), previousThread(currentThread), previousEligibility(eligibility) {
    boundSession = &session;
    previousOperation = currentOperation;
    previousJob = currentJob;
    previousPresentOrdinal = lastPresentOrdinal;
    lastPresentOrdinal.reset();
    currentOperation = nullptr;
    currentJob = nullptr;
    boundClock = clock;
    currentContext = {};
    currentSpan = 0;
    currentThread = 0;
    eligibility = 0;
}

SessionScope::~SessionScope() {
    boundSession = previous;
    boundClock = previousClock;
    currentContext = previousContext;
    currentSpan = previousSpan;
    currentThread = previousThread;
    eligibility = previousEligibility;
    currentOperation = previousOperation;
    currentJob = previousJob;
    lastPresentOrdinal = previousPresentOrdinal;
}

bool Enabled() {
    if (boundSession != nullptr) return true;
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_TRACE_DETAIL");
        if (value == nullptr) return false;
        if (std::string_view(value) != "draw") throw std::invalid_argument("APS5_TRACE_DETAIL must be draw when set");
        return true;
    }();
    return enabled;
}

bool Configured() { return Enabled(); }
bool Recording() { return Enabled() && activeSession().Recording(); }
bool Eligible() { return Enabled() && (activeSession().Recording() || (eligibility != 0 && activeSession().Draining())); }
Id MetadataEpoch() { return Enabled() ? activeSession().Identity() : 0; }
std::string_view CaptureId() { return Enabled() ? std::string_view(activeSession().Configuration().captureId) : std::string_view{}; }
Id NextId() { return Enabled() ? allocateId() : 0; }
Context Current() { return currentContext; }
Context CaptureContext() { return Current(); }
std::optional<std::uint64_t> LastPresentOrdinal() { return lastPresentOrdinal; }

void BindThread(const char* name) {
    if (!Enabled() || currentThread != 0) return;
    try { currentThread = activeSession().RegisterThread(name != nullptr ? name : "instrumented thread", now()); }
    catch (...) { activeSession().ReportLoss(); }
}

void ReportObservationLoss() noexcept {
    try { if (Enabled()) activeSession().ReportLoss(); } catch (...) {}
}

void Record(EventKind kind, Id subject, Id related, std::uint64_t value, Status status) {
    if (!Eligible()) return;
    auto event = makeEvent(kind, subject, related);
    event.value = value;
    event.status = status;
    activeSession().Store(event);
}

void RecordHostCommand(Id command, Id operation, const char* kind, std::uint64_t expansionIndex, std::uint32_t requestedCount, bool dynamicCount, Status status) {
    if (!Eligible()) return;
    auto event = makeEvent(EventKind::HostCommand, command, operation, kind);
    event.value = expansionIndex;
    event.slot = requestedCount;
    event.shared = dynamicCount;
    event.status = status;
    activeSession().Store(event);
}

void RecordOperation(Id operation, const char* kind, Status status) {
    if (!Eligible()) return;
    auto event = makeEvent(EventKind::Operation, operation, currentContext.packet, kind);
    event.status = status;
    activeSession().Store(event);
}

void RecordPresent(Id present, Id flip, std::optional<std::uint64_t> ordinal, Status status) {
    if (!Eligible()) return;
    auto event = makeEvent(EventKind::Present, present, flip);
    event.value = ordinal.value_or(0);
    event.shared = ordinal.has_value();
    event.status = status;
    activeSession().Store(event);
}

void RecordCounter(const char* name, std::uint64_t value, const char* unit, Id subject) {
    if (!Eligible()) return;
    if (subject == 0) subject = currentContext.operation != 0 ? currentContext.operation
        : currentContext.packet != 0 ? currentContext.packet : currentContext.job;
    auto event = makeEvent(EventKind::Counter, subject, 0, name);
    event.value = value;
    event.status = Status::Complete;
    copyText(event.text.data(), event.text.size(), unit);
    activeSession().Store(event);
}

void RecordHardwareCounter(Id command, const char* name, QueryStatus status, std::uint64_t value, const char* unit) noexcept {
    try {
        if (!Enabled() || (!activeSession().Recording() && !activeSession().Draining())) return;
        auto event = makeEvent(EventKind::Counter, command, 0, name);
        event.shared = true;
        event.queryStatus = status;
        event.value = status == QueryStatus::Available ? value : 0;
        event.status = status == QueryStatus::Available ? Status::Complete : Status::Unknown;
        copyText(event.text.data(), event.text.size(), unit);
        activeSession().Store(event);
    } catch (...) { ReportObservationLoss(); }
}

void Link(Id from, Id to, const char* relation) {
    if (!Eligible() || from == 0 || to == 0) return;
    auto event = makeEvent(EventKind::Link, from, to, relation);
    activeSession().Store(event);
}

void BoundaryReference(Id subject, const char* kind, const char* reason) {
    if (!Eligible() || subject == 0) return;
    auto event = makeEvent(EventKind::Reference, subject, 0, kind);
    copyText(event.text.data(), event.text.size(), reason);
    activeSession().Store(event);
}

void DeclareIdentity(Id subject, const char* kind) {
    if (!Eligible() || subject == 0) return;
    auto event = makeEvent(EventKind::Reference, subject, 0, kind);
    event.status = Status::Complete;
    copyText(event.text.data(), event.text.size(), "identity_only_no_lifecycle");
    activeSession().Store(event);
}

void Note(const char* name, std::uint64_t value) {
    if (!Eligible()) return;
    auto event = makeEvent(EventKind::Attribute, currentSpan != 0 ? currentSpan : currentContext.operation != 0 ? currentContext.operation : currentContext.packet, 0, name);
    event.value = value;
    activeSession().Store(event);
}

void Note(const char* name, const char* value) {
    if (!Eligible()) return;
    auto event = makeEvent(EventKind::Attribute, currentSpan != 0 ? currentSpan : currentContext.operation != 0 ? currentContext.operation : currentContext.packet, 0, name);
    copyText(event.text.data(), event.text.size(), value);
    activeSession().Store(event);
}

Id RegisterMetadata(const char* kind, std::string key, std::vector<std::pair<std::string, std::string>> fields, std::vector<Id> references) {
    if (!Enabled()) return 0;
    return activeSession().Metadata(kind, std::move(key), std::move(fields), std::move(references));
}

void AssociateMetadata(Id operation, Id metadata, const char* role) { Link(operation, metadata, role); }

Id DeclareBatch(Id device, Id recorder, std::uint64_t serial) {
    if (!Enabled()) return 0;
    const auto id = NextId();
    if (Eligible()) {
        auto event = makeEvent(EventKind::Batch, id, recorder, "recording_batch");
        event.context.device = device;
        event.context.batch = id;
        event.value = serial;
        activeSession().Store(event);
    }
    return id;
}

void RecordBatch(Id batch, Id queue, std::uint64_t serial, Status status) {
    if (!Enabled() || (!activeSession().Recording() && !activeSession().Draining())) return;
    if (queue != 0) {
        auto identity = makeEvent(EventKind::Reference, queue, 0, "native_queue");
        identity.status = Status::Complete;
        copyText(identity.text.data(), identity.text.size(), "identity_only_no_lifecycle");
        activeSession().Store(identity);
    }
    auto event = makeEvent(EventKind::Batch, batch, queue, "batch_submission");
    event.context.batch = batch;
    event.value = serial;
    event.status = status;
    activeSession().Store(event);
}

Id RegisterGpuQuery(Id batch, std::uint32_t slot, Id operation, const char* kind, bool shared) {
    if (!Eligible()) return 0;
    return activeSession().RegisterQuery(batch, slot, operation, kind, shared, threadId(), now());
}

void ResolveGpuQuery(Id query, QueryStatus status, double beginNs, double endNs) noexcept {
    try {
        if (!Enabled() || query == 0) return;
        activeSession().ResolveQuery(query, status, beginNs, endNs, threadId(), now());
    } catch (...) {
        try { activeSession().ReportLoss(); } catch (...) {}
    }
}

void ResolveGpuQueryTicks(Id query, QueryStatus status, std::uint64_t beginTicks, std::uint64_t endTicks, double periodNs, std::uint32_t validBits) noexcept {
    try {
        if (!Enabled() || query == 0) return;
        activeSession().ResolveQueryTicks(query, status, beginTicks, endTicks, periodNs, threadId(), now(), validBits);
    } catch (...) {
        try { activeSession().ReportLoss(); } catch (...) {}
    }
}

void Calibrate(Id device, double gpuNs, Clock::time_point cpu, double deviationNs, double periodNs, std::uint32_t validBits) {
    if (!Enabled() || (!activeSession().Recording() && !activeSession().Draining())) return;
    if (device != 0) {
        auto identity = makeEvent(EventKind::Reference, device, 0, "device");
        identity.status = Status::Complete;
        copyText(identity.text.data(), identity.text.size(), "identity_only_no_lifecycle");
        activeSession().Store(identity);
    }
    auto event = makeEvent(EventKind::Calibration, device, 0, "gpu_clock_calibration");
    event.context.device = device;
    event.begin = cpu;
    event.first = gpuNs;
    event.second = deviationNs;
    event.third = periodNs;
    event.value = validBits;
    event.status = std::isfinite(gpuNs) && gpuNs >= 0 && std::isfinite(deviationNs) && deviationNs >= 0
        && std::isfinite(periodNs) && periodNs > 0 && validBits > 0 ? Status::Complete : Status::Failed;
    activeSession().Store(event);
}

std::size_t GpuQueryCapacity() { return 512; }

void Calibrate(Id device, const CalibrationSample& sample) {
    if (!Enabled() || (!activeSession().Recording() && !activeSession().Draining())) return;
    if (device != 0) {
        auto identity = makeEvent(EventKind::Reference, device, 0, "device");
        identity.status = Status::Complete;
        copyText(identity.text.data(), identity.text.size(), "identity_only_no_lifecycle");
        activeSession().Store(identity);
    }
    auto event = makeEvent(EventKind::Calibration, device, 0, "gpu_clock_calibration");
    event.context.device = device;
    event.calibration = sample;
    event.value = sample.validBits;
    event.first = static_cast<double>(sample.gpuTicks) * sample.periodNs;
    event.second = sample.deviationNs;
    event.third = sample.periodNs;
    const bool knownDomain = sample.hostDomain != nullptr && (std::string_view(sample.hostDomain) == "query_performance_counter"
        || std::string_view(sample.hostDomain) == "clock_monotonic");
    event.status = sample.available && sample.hostFrequency > 0 && sample.after >= sample.before
        && std::isfinite(sample.deviationNs) && sample.deviationNs >= 0 && std::isfinite(sample.periodNs)
        && sample.periodNs > 0 && sample.validBits > 0 && sample.validBits <= 64 && knownDomain ? Status::Complete : Status::Failed;
    if (event.status == Status::Failed) {
        event.calibration.available = false;
        event.value = 0;
        event.first = event.second = event.third = std::numeric_limits<double>::quiet_NaN();
        copyText(event.text.data(), event.text.size(), "unavailable_or_invalid_raw_calibration");
    }
    activeSession().Store(event);
}

void PresentBoundary(std::uint64_t present, Clock::time_point time, bool finalize) {
    if (!Enabled()) return;
    lastPresentOrdinal = present;
    activeSession().Boundary(present, time);
    if (activeSession().Recording()) startJournal();
    if (finalize) FinalizeBoundary();
}

void FinalizeBoundary() {
    if (Enabled() && activeSession().Draining()) startWriter();
}

void Stop(const char* reason) {
    if (!Enabled()) return;
    activeSession().RequestStop(reason != nullptr ? reason : "shutdown", now());
    startWriter();
}

void Flush() {
    if (!Enabled()) return;
    if (boundSession != nullptr) { boundSession->Freeze(now()); return; }
    Stop("shutdown");
    auto& owner = global();
    std::thread writer;
    {
        std::lock_guard lock(owner.mutex);
        if (owner.writer.joinable()) writer = std::move(owner.writer);
    }
    if (writer.joinable()) writer.join();
    std::lock_guard lock(owner.mutex);
    if (!owner.failure.empty()) throw std::runtime_error("draw trace output failed: " + owner.failure);
}

ContextScope::ContextScope(Context context) {
    if (!Enabled()) return;
    previous = currentContext;
    currentContext = context;
}

ContextScope::~ContextScope() {
    if (previous) currentContext = *previous;
}

Span::Span(const char* name, bool waitForDrain) : drain(waitForDrain) {
    if (!Enabled() || (drain && !Eligible())) return;
    const auto id = NextId();
    if (drain && !activeSession().Admit(EventKind::Span, id, eligibility != 0)) return;
    event = makeEvent(EventKind::Span, id, 0, name);
    event->cpuBegin = activeSession().SampleCpu();
    event->cpuStatus = event->cpuBegin.status;
    markedCpu = event->cpuBegin;
    event->cyclesBegin = activeSession().SampleCycles();
    event->cyclesStatus = event->cyclesBegin.status;
    markedCycles = event->cyclesBegin;
    if (!drain && !activeSession().Observe(*event)) { event.reset(); return; }
    event->status = Status::Complete;
    previous = currentSpan;
    currentSpan = id;
    marked = event->begin;
    exceptions = std::uncaught_exceptions();
    admitted = drain;
    if (admitted) ++eligibility;
}

Span::~Span() noexcept { Finish(); }

extern "C" void AgcDrawTraceSpanBegin_nid_no_patch(Span* storage, const char* name, bool drain) {
    new (storage) Span(name, drain);
}

extern "C" void AgcDrawTraceSpanMark_nid_no_patch(Span* storage, const char* stage) {
    std::launder(storage)->Mark(stage);
}

extern "C" void AgcDrawTraceSpanFinish_nid_no_patch(Span* storage) noexcept {
    std::launder(storage)->Finish();
}

extern "C" void AgcDrawTraceSpanEnd_nid_no_patch(Span* storage) noexcept {
    std::launder(storage)->~Span();
}

Id Span::GetId() const { return event ? event->subject : 0; }
void Span::Outcome(Status status) { if (event) event->status = status; }

void Span::Mark(const char* stage) {
    if (!event || finished || !drain) return;
    auto phase = *event;
    phase.subject = NextId();
    phase.parentSpan = event->subject;
    phase.name = stage;
    phase.begin = marked;
    phase.end = now();
    phase.cpuBegin = markedCpu;
    phase.cpuEnd = activeSession().SampleCpu();
    phase.cpuStatus = cpuOutcome(phase.cpuBegin, phase.cpuEnd);
    markedCpu = phase.cpuEnd;
    phase.cyclesBegin = markedCycles;
    phase.cyclesEnd = activeSession().SampleCycles();
    phase.cyclesStatus = cycleOutcome(phase.cyclesBegin, phase.cyclesEnd);
    markedCycles = phase.cyclesEnd;
    marked = phase.end;
    activeSession().Store(phase);
}

void Span::Finish() noexcept {
    if (!event || finished) return;
    finished = true;
    try {
        event->end = now();
        event->cpuEnd = activeSession().SampleCpu();
        event->cpuStatus = cpuOutcome(event->cpuBegin, event->cpuEnd);
        event->cyclesEnd = activeSession().SampleCycles();
        event->cyclesStatus = cycleOutcome(event->cyclesBegin, event->cyclesEnd);
        if (std::uncaught_exceptions() > exceptions) event->status = Status::Failed;
        if (drain) {
            activeSession().Store(*event);
            activeSession().Release(EventKind::Span, event->subject);
        } else activeSession().CompleteObservation(*event);
    } catch (...) {}
    currentSpan = previous;
    if (admitted && eligibility != 0) --eligibility;
}

Operation::Operation(const char* name) : kind(name) {
    if (!Enabled()) return;
    previous = currentContext;
    previousOperation = currentOperation;
    currentOperation = this;
    id = NextId();
    currentContext.operation = id;
    exceptions = std::uncaught_exceptions();
    if (Eligible()) {
        admitted = activeSession().Admit(EventKind::Operation, id, eligibility != 0);
        if (admitted) {
            ++eligibility;
            auto event = makeEvent(EventKind::Operation, id, currentContext.packet, kind);
            activeSession().Store(event);
            const auto parent = previous->operation != 0 ? previous->operation : currentContext.packet;
            if (parent != 0) Link(parent, id, "contains");
        }
    }
}

Operation::~Operation() noexcept {
    if (!previous) return;
    try {
        if (admitted) {
            auto event = makeEvent(EventKind::Operation, id, currentContext.packet, kind);
            event.status = std::uncaught_exceptions() > exceptions ? Status::Failed : status;
            activeSession().Store(event);
            activeSession().Release(EventKind::Operation, id);
        }
    } catch (...) {}
    if (admitted && eligibility != 0) --eligibility;
    currentContext = *previous;
    currentOperation = previousOperation;
}

Id Operation::GetId() const { return id; }
void Operation::Outcome(Status value) { status = value; }

JobToken ScheduleJob(const char* name) {
    JobToken token;
    if (!Enabled()) return token;
    token.id = NextId();
    token.parentSpan = currentSpan;
    token.context = currentContext;
    token.context.job = token.id;
    token.name = name;
    token.admitted = Eligible() && activeSession().Admit(EventKind::Job, token.id, eligibility != 0);
    if (token.admitted) {
        auto event = makeEvent(EventKind::Job, token.id, token.parentSpan, name);
        event.context = token.context;
        activeSession().Store(event);
        if (token.parentSpan != 0) Link(token.parentSpan, token.id, "schedules_job");
        if (currentContext.job != 0) Link(currentContext.job, token.id, "schedules_job");
    }
    return token;
}

void CancelJob(const JobToken& token, Status status) {
    if (!Enabled() || !token.admitted) return;
    auto event = makeEvent(EventKind::Job, token.id, token.parentSpan, token.name);
    event.context = token.context;
    event.status = status;
    activeSession().Store(event);
    activeSession().Release(EventKind::Job, token.id);
}

JobScope::JobScope(JobToken value) : token(value), previous(currentContext), previousSpan(currentSpan) {
    if (!Enabled()) return;
    previousJob = currentJob;
    currentJob = this;
    currentContext = token.context;
    begin = now();
    if (!token.admitted && token.id != 0 && activeSession().Recording()) {
        token.admitted = activeSession().Admit(EventKind::Job, token.id);
        if (token.admitted) BoundaryReference(token.id, "job", "scheduled_before_window_worker_observed");
    }
    exceptions = std::uncaught_exceptions();
    if (token.admitted) ++eligibility;
}

JobScope::~JobScope() noexcept {
    if (!Enabled()) return;
    try {
        if (token.admitted) {
            auto event = makeEvent(EventKind::Job, token.id, token.parentSpan, token.name);
            event.begin = begin;
            event.end = now();
            event.status = std::uncaught_exceptions() > exceptions ? Status::Failed : status;
            activeSession().Store(event);
            activeSession().Release(EventKind::Job, token.id);
        }
    } catch (...) {}
    if (token.admitted && eligibility != 0) --eligibility;
    currentContext = previous;
    currentSpan = previousSpan;
    currentJob = previousJob;
}

void JobScope::Outcome(Status value) { status = value; }
Id JobScope::GetId() const { return token.id; }
void SetOperationOutcome(Status status) { if (currentOperation != nullptr && currentOperation->GetId() == currentContext.operation) currentOperation->Outcome(status); }
void SetJobOutcome(Status status) { if (currentJob != nullptr && currentJob->GetId() == currentContext.job) currentJob->Outcome(status); }

}

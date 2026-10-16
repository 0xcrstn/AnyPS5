#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRAWTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRAWTRACE_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AgcDriver::DrawTrace {

class Operation;
class JobScope;
using Id = std::uint64_t;
using Clock = std::chrono::steady_clock;

enum class Status { Unknown, Complete, Skipped, Rejected, Failed, Cancelled };
enum class QueryStatus { Pending, Available, Unsupported, AllocationFailed, CapacityDropped, ReadFailed, InvalidTimestamp, Uncollected };
enum class EventKind { Submission, Segment, Packet, Operation, HostCommand, Batch, Flip, Present, Link, Job, Span, Boundary, Calibration, GpuQuery, Attribute, Reference, Counter };

enum class CpuStatus { NotRequested, Measured, Unavailable, Failed, Nonmonotonic, ThreadChanged, Clipped };

struct CpuSample {
    std::uint64_t nativeThread = 0;
    std::uint64_t ticks = 0;
    std::uint64_t frequency = 0;
    CpuStatus status = CpuStatus::Unavailable;
};

struct CycleSample {
    std::uint64_t nativeThread = 0;
    std::uint64_t cycles = 0;
    CpuStatus status = CpuStatus::Unavailable;
};

struct CalibrationSample {
    std::uint64_t gpuTicks = 0;
    std::uint64_t hostTicks = 0;
    std::uint64_t conversionHostTicks = 0;
    std::uint64_t hostFrequency = 0;
    const char* hostDomain = "unknown";
    Clock::time_point before{};
    Clock::time_point after{};
    double deviationNs = 0;
    double periodNs = 0;
    std::uint32_t validBits = 0;
    bool available = false;
};

struct Context {
    Id driver = 0;
    Id submission = 0;
    Id segment = 0;
    Id packet = 0;
    Id operation = 0;
    Id device = 0;
    Id batch = 0;
    Id job = 0;
    Id flip = 0;
    Id present = 0;
    std::uint32_t queue = 0xffffffffu;
    std::uint64_t packetOffset = 0;
    std::uint32_t opcode = 0xffffffffu;
};

struct Event {
    Id id = 0;
    EventKind kind = EventKind::Attribute;
    Id thread = 0;
    Id subject = 0;
    Id related = 0;
    Id parentSpan = 0;
    Context context;
    Clock::time_point begin{};
    Clock::time_point end{};
    const char* name = nullptr;
    std::array<char, 96> nameStorage{};
    std::array<char, 128> text{};
    std::uint64_t value = 0;
    Status status = Status::Unknown;
    QueryStatus queryStatus = QueryStatus::Pending;
    double first = 0;
    double second = 0;
    double third = 0;
    double fourth = 0;
    std::uint32_t slot = 0;
    bool shared = false;
    bool rawTicks = false;
    std::uint64_t beginTicks = 0;
    std::uint64_t endTicks = 0;
    CpuSample cpuBegin;
    CpuSample cpuEnd;
    CpuStatus cpuStatus = CpuStatus::NotRequested;
    CycleSample cyclesBegin;
    CycleSample cyclesEnd;
    CpuStatus cyclesStatus = CpuStatus::NotRequested;
    CalibrationSample calibration;
    std::array<char, 32> calibrationDomain{};
};

struct Options {
    std::string runId;
    std::string captureId;
    std::string revision = "unknown";
    std::optional<bool> dirty;
    std::uint64_t process = 0;
    bool frames = true;
    std::uint64_t start = 0;
    std::uint64_t length = 1;
    std::size_t recordsPerThread = 65536;
    std::size_t maximumThreads = 64;
    std::size_t maximumMetadata = 8192;
    std::size_t maximumQueries = 65536;
    std::size_t maximumBoundaries = 65536;
    std::size_t maximumEvents = 8388608;
    std::size_t maximumBytes = 512u * 1024u * 1024u;
    std::chrono::milliseconds drainTimeout{2000};
    std::unique_ptr<Event[]> (*allocateChunk)(std::size_t) = nullptr;
    bool threadCpu = false;
    CpuSample (*sampleCpu)() = nullptr;
    bool threadCycles = false;
    CycleSample (*sampleCycles)() = nullptr;
    bool perfettoView = false;
    bool journal = false;
    std::size_t maximumJournalBytes = std::size_t{16} * 1024u * 1024u * 1024u;
    std::chrono::milliseconds journalInterval{100};
};

class Session {
public:
    explicit Session(Options options);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Id RegisterThread(std::string name, Clock::time_point now);
    void Boundary(std::uint64_t present, Clock::time_point now);
    bool Admit(EventKind kind, Id subject, bool continuation = false);
    void Release(EventKind kind, Id subject);
    bool Store(Event event);
    bool Observe(Event event);
    void CompleteObservation(Event event);
    void ReportLoss();
    Id Metadata(const char* kind, std::string key, std::vector<std::pair<std::string, std::string>> fields = {}, std::vector<Id> references = {});
    Id RegisterQuery(Id batch, std::uint32_t slot, Id operation, const char* kind, bool shared, Id thread, Clock::time_point now);
    void ResolveQuery(Id query, QueryStatus status, double beginNs, double endNs, Id thread, Clock::time_point now);
    void ResolveQueryTicks(Id query, QueryStatus status, std::uint64_t beginTicks, std::uint64_t endTicks, double periodNs, Id thread, Clock::time_point now, std::uint32_t validBits = 64);
    void RequestStop(std::string reason, Clock::time_point now);
    bool Ready(Clock::time_point now) const;
    bool Recording() const;
    bool Draining() const;
    bool Frozen() const;
    void Freeze(Clock::time_point now);
    std::string Json() const;
    void WriteJson(std::ostream& output) const;
    bool WriteJournal(std::ostream& output);
    bool JournalFinished() const;
    void FailJournal();
    CpuSample SampleCpu() const;
    CycleSample SampleCycles() const;
    const Options& Configuration() const;
    Id Identity() const;

private:
    struct State;
    std::unique_ptr<State> state;
};

class SessionScope {
public:
    explicit SessionScope(Session& session, Clock::time_point (*clock)() = nullptr);
    ~SessionScope();
    SessionScope(const SessionScope&) = delete;
    SessionScope& operator=(const SessionScope&) = delete;

private:
    Session* previous = nullptr;
    Clock::time_point (*previousClock)() = nullptr;
    Context previousContext;
    Id previousSpan = 0;
    Id previousThread = 0;
    std::size_t previousEligibility = 0;
    Operation* previousOperation = nullptr;
    JobScope* previousJob = nullptr;
    std::optional<std::uint64_t> previousPresentOrdinal;
};

bool Enabled();
bool Configured();
bool Recording();
bool Eligible();
Id MetadataEpoch();
std::string_view CaptureId();
Id NextId();
Context Current();
Context CaptureContext();
std::optional<std::uint64_t> LastPresentOrdinal();
void BindThread(const char* name);
void ReportObservationLoss() noexcept;
void Record(EventKind kind, Id subject = 0, Id related = 0, std::uint64_t value = 0, Status status = Status::Unknown);
void RecordHostCommand(Id command, Id operation, const char* kind, std::uint64_t expansionIndex, std::uint32_t requestedCount, bool dynamicCount, Status status);
void RecordOperation(Id operation, const char* kind, Status status);
void RecordPresent(Id present, Id flip, std::optional<std::uint64_t> ordinal, Status status = Status::Unknown);
void RecordCounter(const char* name, std::uint64_t value, const char* unit, Id subject = 0);
void RecordHardwareCounter(Id command, const char* name, QueryStatus status, std::uint64_t value, const char* unit) noexcept;
void Link(Id from, Id to, const char* relation);
void BoundaryReference(Id subject, const char* kind, const char* reason = "outside_window_or_uninstrumented");
void DeclareIdentity(Id subject, const char* kind);
void Note(const char* name, std::uint64_t value);
void Note(const char* name, const char* value);
Id RegisterMetadata(const char* kind, std::string key, std::vector<std::pair<std::string, std::string>> fields = {}, std::vector<Id> references = {});
void AssociateMetadata(Id operation, Id metadata, const char* role);
Id DeclareBatch(Id device, Id recorder, std::uint64_t serial);
void RecordBatch(Id batch, Id queue, std::uint64_t serial, Status status);
Id RegisterGpuQuery(Id batch, std::uint32_t slot, Id operation, const char* kind, bool shared = false);
void ResolveGpuQuery(Id query, QueryStatus status, double beginNs = 0, double endNs = 0) noexcept;
void ResolveGpuQueryTicks(Id query, QueryStatus status, std::uint64_t beginTicks, std::uint64_t endTicks, double periodNs, std::uint32_t validBits = 64) noexcept;
void Calibrate(Id device, double gpuNs, Clock::time_point cpu, double deviationNs, double periodNs, std::uint32_t validBits);
void Calibrate(Id device, const CalibrationSample& sample);
std::size_t GpuQueryCapacity();
void PresentBoundary(std::uint64_t present, Clock::time_point now, bool finalize = true);
void FinalizeBoundary();
void Stop(const char* reason = "shutdown");
void Flush();

class ContextScope {
public:
    explicit ContextScope(Context context);
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;

private:
    std::optional<Context> previous;
};

class Span {
public:
    explicit Span(const char* name, bool drain = true);
    ~Span() noexcept;
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    Id GetId() const;
    void Outcome(Status status);
    void Mark(const char* stage);
    void Finish() noexcept;

private:
    std::optional<Event> event;
    Clock::time_point marked{};
    Id previous = 0;
    int exceptions = 0;
    bool admitted = false;
    bool finished = false;
    bool drain = true;
    CpuSample markedCpu;
    CycleSample markedCycles;
};

extern "C" void AgcDrawTraceSpanBegin_nid_no_patch(Span* storage, const char* name, bool drain);
extern "C" void AgcDrawTraceSpanMark_nid_no_patch(Span* storage, const char* stage);
extern "C" void AgcDrawTraceSpanFinish_nid_no_patch(Span* storage) noexcept;
extern "C" void AgcDrawTraceSpanEnd_nid_no_patch(Span* storage) noexcept;

class HostSpan {
public:
    explicit HostSpan(const char* name, bool drain = true) {
        AgcDrawTraceSpanBegin_nid_no_patch(reinterpret_cast<Span*>(storage), name, drain);
    }
    ~HostSpan() noexcept { AgcDrawTraceSpanEnd_nid_no_patch(reinterpret_cast<Span*>(storage)); }
    HostSpan(const HostSpan&) = delete;
    HostSpan& operator=(const HostSpan&) = delete;
    void Mark(const char* stage) { AgcDrawTraceSpanMark_nid_no_patch(reinterpret_cast<Span*>(storage), stage); }
    void Finish() noexcept { AgcDrawTraceSpanFinish_nid_no_patch(reinterpret_cast<Span*>(storage)); }

private:
    alignas(Span) std::byte storage[sizeof(Span)];
};

class Operation {
public:
    explicit Operation(const char* kind);
    ~Operation() noexcept;
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Id GetId() const;
    void Outcome(Status status);

private:
    std::optional<Context> previous;
    Id id = 0;
    const char* kind = nullptr;
    Status status = Status::Complete;
    int exceptions = 0;
    bool admitted = false;
    Operation* previousOperation = nullptr;
};

struct JobToken {
    Id id = 0;
    Id parentSpan = 0;
    Context context;
    const char* name = nullptr;
    bool admitted = false;
};

JobToken ScheduleJob(const char* name);
void CancelJob(const JobToken& token, Status status = Status::Cancelled);
void SetOperationOutcome(Status status);
void SetJobOutcome(Status status);

class JobScope {
public:
    explicit JobScope(JobToken token);
    ~JobScope() noexcept;
    JobScope(const JobScope&) = delete;
    JobScope& operator=(const JobScope&) = delete;
    void Outcome(Status status);
    Id GetId() const;

private:
    JobToken token;
    Context previous;
    Id previousSpan = 0;
    Clock::time_point begin{};
    Status status = Status::Complete;
    int exceptions = 0;
    JobScope* previousJob = nullptr;
};

using SpanScope = Span;
using OperationScope = Operation;

}

#endif

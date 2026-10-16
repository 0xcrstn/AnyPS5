#include "prx/libSceAgcDriver/Execution/include/DrawTrace.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {

using namespace AgcDriver::DrawTrace;
using namespace std::chrono_literals;
thread_local Clock::time_point now = Clock::time_point{} + 1s;

Clock::time_point Time() { return now; }

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Options Configuration() {
    Options options;
    options.runId = "11111111-1111-4111-8111-111111111111";
    options.captureId = "22222222-2222-4222-8222-222222222222";
    options.process = 1234;
    options.revision = "aaaaaaaaaaaa";
    options.dirty = false;
    options.start = 10;
    options.length = 2;
    options.recordsPerThread = 64;
    options.maximumThreads = 8;
    options.maximumMetadata = 64;
    options.maximumQueries = 64;
    options.maximumBytes = 16u * 1024u * 1024u;
    return options;
}

std::string Compact(const std::string& value) {
    std::string result;
    bool quoted = false;
    bool escaped = false;
    for (const unsigned char item : value) {
        if (quoted || !std::isspace(item)) result += static_cast<char>(item);
        if (item == '"' && !escaped) quoted = !quoted;
        escaped = item == '\\' && !escaped;
    }
    return result;
}

std::string EventText(const std::string& json, const std::string& name) {
    const auto marker = json.rfind("\"name\":\"" + name + "\"", json.find("\"traceEvents\":"));
    Check(marker != std::string::npos, "expected event is missing");
    const auto begin = json.rfind("{\"id\":", marker);
    Check(begin != std::string::npos, "event envelope is missing");
    auto depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (auto index = begin; index < json.size(); ++index) {
        const auto item = json[index];
        if (!quoted && item == '{') ++depth;
        if (!quoted && item == '}' && --depth == 0) return json.substr(begin, index - begin + 1);
        if (item == '"' && !escaped) quoted = !quoted;
        escaped = item == '\\' && !escaped;
    }
    throw std::runtime_error("event envelope is unclosed");
}

void BoundariesAndLateQueries() {
    Session session(Configuration());
    Check(!session.Recording(), "recording started before the opening present");
    session.Boundary(9, Time());
    Check(!session.Recording(), "pre-window boundary opened recording");
    session.Boundary(10, Time());
    Check(session.Recording(), "opening present did not open recording");
    const auto thread = session.RegisterThread("synthetic gpu collector", Time());
    const auto query = session.RegisterQuery(91, 0, 92, "draw", false, thread, Time());
    Check(query != 0, "query was not admitted");
    now += 20ms;
    session.Boundary(11, Time());
    now += 20ms;
    session.Boundary(12, Time());
    Check(!session.Recording() && session.Draining(), "closing present did not start draining");
    Check(!session.Ready(Time()), "pending query was discarded at window close");
    Check(!session.Admit(EventKind::Span, 93), "unrelated work was admitted after close");
    Check(session.RegisterQuery(94, 1, 0, "unrelated late draw", false, thread, Time()) == 0,
          "zero operation identity admitted unrelated query after close");
    session.ResolveQuery(query, QueryStatus::Available, 100, 130, thread, Time());
    Check(session.Ready(Time()), "resolved query did not release drain");
    session.Freeze(Time());
    Check(session.Frozen(), "ready session did not freeze");
    const auto json = Compact(session.Json());
    Check(json.find("\"close_present\":\"12\"") != std::string::npos, "closing boundary was not retained");
    Check(json.find("\"status\":\"available\"") != std::string::npos, "late query result was lost");
    Check(json.find("\"begin_gpu_ns\":100") != std::string::npos, "GPU duration endpoints changed");
}

void NestedScopesAndExceptions() {
    Session session(Configuration());
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    Context context;
    context.submission = NextId();
    context.queue = 3;
    const auto submission = context.submission;
    Id outer = 0;
    Id operation = 0;
    {
        ContextScope contextScope(context);
        Span parent("parent");
        outer = parent.GetId();
        Operation draw("draw");
        operation = draw.GetId();
        Check(Current().operation == operation && Current().submission == submission, "operation dropped submission context");
        try {
            Span child("exception child");
            now += 7us;
            throw std::runtime_error("synthetic failure");
        } catch (const std::runtime_error&) {}
        now += 5us;
    }
    Check(Current().submission == 0 && Current().operation == 0, "scope context leaked");
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "exception unwinding leaked a pending span");
    session.Freeze(Time());
    const auto child = EventText(Compact(session.Json()), "exception child");
    Check(child.find("\"status\":\"failed\"") != std::string::npos, "exception span claimed success");
    Check(child.find("\"parent_span\":\"" + std::to_string(outer) + "\"") != std::string::npos, "nested span lost parent identity");
    Check(child.find("\"operation\":\"" + std::to_string(operation) + "\"") != std::string::npos, "span lost operation identity");
}

void AsyncWorkSurvivesClosure() {
    Session session(Configuration());
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    JobToken token;
    Id operation = 0;
    {
        Operation draw("draw");
        operation = draw.GetId();
        Span prepare("schedule preparation");
        token = ScheduleJob("asynchronous preparation");
        Check(token.admitted && token.context.operation == operation && token.parentSpan == prepare.GetId(), "scheduled job lost origin");
    }
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(!session.Ready(Time()), "scheduled job was discarded before worker execution");
    const auto workerStart = Time() + 1us;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            now = workerStart;
            SessionScope workerBinding(session, Time);
            JobScope work(token);
            Check(Current().operation == operation && Current().job == token.id, "worker lost scheduling context");
            Span stage("worker preparation");
            now += 10us;
        } catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    if (failure) std::rethrow_exception(failure);
    Check(session.Ready(Time()), "finished worker did not release drain");
    session.Freeze(Time());
    const auto stage = EventText(Compact(session.Json()), "worker preparation");
    Check(stage.find("\"job\":\"" + std::to_string(token.id) + "\"") != std::string::npos, "worker stage lost job identity");
    Check(stage.find("\"operation\":\"" + std::to_string(operation) + "\"") != std::string::npos, "worker stage lost operation identity");
}

void DeferredSameThreadJobsUseExecutionParents() {
    Session session(Configuration());
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    JobToken detached;
    JobToken nested;
    Id origin = 0;
    Id execution = 0;
    {
        Span schedule("same-thread scheduling origin");
        origin = schedule.GetId();
        detached = ScheduleJob("same-thread detached job");
        nested = ScheduleJob("same-thread nested job");
        now += 1us;
    }
    now += 10us;
    {
        JobScope job(detached);
        Span work("same-thread detached work");
        now += 2us;
    }
    {
        Span caller("same-thread execution caller");
        execution = caller.GetId();
        {
            JobScope job(nested);
            Span work("same-thread nested work");
            now += 2us;
        }
        Span restored("same-thread restored caller");
        now += 1us;
    }
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "same-thread jobs retained pending observations");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(EventText(json, "same-thread detached work").find("\"parent_span\":null") != std::string::npos,
          "deferred same-thread work inherited a completed scheduling span");
    const auto executionParent = "\"parent_span\":\"" + std::to_string(execution) + "\"";
    Check(EventText(json, "same-thread nested work").find(executionParent) != std::string::npos,
          "deferred job discarded its active execution parent");
    Check(EventText(json, "same-thread restored caller").find(executionParent) != std::string::npos,
          "deferred job did not restore its execution span stack");
    const auto schedulingLink = "\"subject\":\"" + std::to_string(origin) + "\",\"related\":\"" + std::to_string(detached.id) + "\",\"name\":\"schedules_job\"";
    Check(json.find(schedulingLink) != std::string::npos, "deferred job lost its scheduling dependency");
}

void InlineJobsPreserveLexicalParents() {
    Session session(Configuration());
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    Id parent = 0;
    {
        Span caller("inline scheduling caller");
        parent = caller.GetId();
        const auto token = ScheduleJob("inline preparation job");
        {
            JobScope job(token);
            Span work("inline preparation work");
            now += 2us;
        }
        Span restored("inline restored caller");
        now += 1us;
    }
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "inline job retained pending observations");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    const auto lexicalParent = "\"parent_span\":\"" + std::to_string(parent) + "\"";
    Check(EventText(json, "inline preparation work").find(lexicalParent) != std::string::npos,
          "inline job lost its active lexical parent");
    Check(EventText(json, "inline restored caller").find(lexicalParent) != std::string::npos,
          "inline job did not restore its lexical span stack");
}

void CancellationAndNestedBindings() {
    Session outer(Configuration());
    Session inner(Configuration());
    SessionScope binding(outer, Time);
    outer.Boundary(10, Time());
    Context context;
    context.submission = NextId();
    ContextScope scoped(context);
    Span parent("outer parent");
    {
        SessionScope nested(inner, Time);
        inner.Boundary(10, Time());
        Check(Current().submission == 0, "inner session inherited outer context");
        Span independent("inner independent");
    }
    Check(Current().submission == context.submission, "nested binding did not restore outer context");
    const auto token = ScheduleJob("cancelled preparation");
    CancelJob(token);
    parent.Finish();
    outer.Boundary(11, Time());
    outer.Boundary(12, Time());
    Check(outer.Ready(Time()), "cancelled job held drain open");
    outer.Freeze(Time());
    inner.Boundary(11, Time());
    inner.Boundary(12, Time());
    inner.Freeze(Time());
    const auto event = EventText(Compact(inner.Json()), "inner independent");
    Check(event.find("\"parent_span\":null") != std::string::npos, "nested session retained a foreign parent span");
    Check(Compact(outer.Json()).find("\"status\":\"cancelled\"") != std::string::npos, "cancelled work claimed completion");
}

void CapacityAndDeadlineRemainVisible() {
    auto options = Configuration();
    options.recordsPerThread = 1;
    options.maximumMetadata = 1;
    options.maximumQueries = 1;
    Session session(options);
    session.Boundary(10, Time());
    const auto thread = session.RegisterThread("bounded producer", Time());
    Event event;
    event.kind = EventKind::Span;
    event.thread = thread;
    event.name = "bounded span";
    event.begin = Time();
    event.end = Time() + 1us;
    event.status = Status::Complete;
    Check(session.Store(event), "first event was unexpectedly rejected");
    Check(!session.Store(event), "event storage exceeded capacity");
    Check(session.Metadata("shader", "first") != 0, "first dictionary entry rejected");
    Check(session.Metadata("shader", "second") == 0, "dictionary exceeded capacity");
    Check(session.RegisterQuery(90, 0, 91, "draw", false, thread, Time()) != 0, "first query rejected");
    Check(session.RegisterQuery(90, 1, 92, "draw", false, thread, Time()) == 0, "query capacity exceeded");
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(!session.Ready(Time()), "unresolved query did not hold drain");
    now += options.drainTimeout;
    Check(session.Ready(Time()), "bounded drain never reached deadline");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(json.find("\"state\":\"partial\"") != std::string::npos, "lossy capture claimed complete");
    Check(json.find("\"pending_gpu_queries\":1") != std::string::npos, "unresolved query disappeared at deadline");
    Check(json.find("\"metadata\":1") != std::string::npos, "dictionary loss disappeared");
    Check(json.find("\"gpu_queries\":1") != std::string::npos, "query capacity loss disappeared");
}

void QueryFailuresAreNotDurations() {
    for (const auto& [status, name] : {std::pair{QueryStatus::Unsupported, "unsupported"},
                                     std::pair{QueryStatus::AllocationFailed, "allocation_failed"},
                                     std::pair{QueryStatus::ReadFailed, "read_failed"},
                                     std::pair{QueryStatus::InvalidTimestamp, "invalid_timestamp"}}) {
        Session session(Configuration());
        session.Boundary(10, Time());
        const auto thread = session.RegisterThread("query collector", Time());
        const auto query = session.RegisterQuery(90, 0, 91, "draw", false, thread, Time());
        session.ResolveQuery(query, status, 0, 0, thread, Time());
        session.Boundary(11, Time());
        session.Boundary(12, Time());
        Check(session.Ready(Time()), "resolved failure remained pending forever");
        session.Freeze(Time());
        const auto json = Compact(session.Json());
        const auto terminal = EventText(json, "draw");
        Check(terminal.find("\"status\":\"" + std::string(name) + "\"") != std::string::npos, "query failure outcome disappeared");
        Check(terminal.find("\"begin_gpu_ns\":null") != std::string::npos
            && terminal.find("\"end_gpu_ns\":null") != std::string::npos, "failed query fabricated timestamp measurements");
        Check(json.find("\"query_failures\":1") != std::string::npos, "resolved query failure was not counted");
        Check(json.find("\"state\":\"partial\"") != std::string::npos, "failed query claimed complete capture");
    }
}

void DictionaryConflictsAndInterruptedWindows() {
    Session session(Configuration());
    const auto shader = session.Metadata("shader", "stable", {{"stage", "fragment"}});
    Check(shader != 0, "metadata registration failed before window");
    Check(session.Metadata("shader", "stable", {{"stage", "fragment"}}) == shader, "stable metadata failed to deduplicate");
    Check(session.Metadata("shader", "stable", {{"stage", "vertex"}}) == 0, "conflicting dictionary fields reused an identity");
    const auto pipeline = session.Metadata("pipeline", "stable pipeline", {}, {shader});
    Check(pipeline != 0, "pipeline reference registration failed");
    Check(session.Metadata("pipeline", "stable pipeline", {}, {}) == 0, "conflicting dictionary references reused an identity");
    session.Boundary(10, Time());
    session.RequestStop("synthetic interruption", Time());
    Check(!session.Recording() && session.Ready(Time()), "stop did not make empty drain ready");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(json.find("\"state\":\"partial\"") != std::string::npos, "interrupted short window claimed complete");
    Check(json.find("\"close_reason\":\"synthetic interruption\"") != std::string::npos, "stop reason lost");
    Check(json.find("\"metadata\":2") != std::string::npos, "conflicting dictionary losses not counted");
}

void QuerySlotsAreNamespacedByBatch() {
    Session session(Configuration());
    session.Boundary(10, Time());
    const auto thread = session.RegisterThread("collector", Time());
    const auto first = session.RegisterQuery(90, 0, 92, "draw", false, thread, Time());
    const auto second = session.RegisterQuery(91, 0, 93, "draw", false, thread, Time());
    Check(first != 0 && second != 0 && first != second, "reused query slot aliased another batch");
    session.ResolveQuery(first, QueryStatus::Available, 100, 120, thread, Time());
    session.ResolveQuery(second, QueryStatus::Available, 300, 350, thread, Time());
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "independent query completion leaked pending count");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(json.find("\"end_gpu_ns\":120") != std::string::npos && json.find("\"end_gpu_ns\":350") != std::string::npos, "batch query results overwrote each other");
}

void EqualAndInvalidTimestamps() {
    Session session(Configuration());
    session.Boundary(10, Time());
    const auto thread = session.RegisterThread("timestamp collector", Time());
    const auto zero = session.RegisterQuery(90, 0, 92, "measured zero", false, thread, Time());
    session.ResolveQuery(zero, QueryStatus::Available, 100, 100, thread, Time());
    const auto reversed = session.RegisterQuery(90, 1, 93, "reversed", false, thread, Time());
    session.ResolveQuery(reversed, QueryStatus::Available, 101, 100, thread, Time());
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    const auto firstZero = json.find("\"name\":\"measured zero\"");
    const auto lastZero = json.find("\"name\":\"measured zero\"", firstZero + 1);
    Check(lastZero != std::string::npos, "measured zero result disappeared");
    Check(json.find("\"status\":\"available\"", lastZero) < json.find("\"name\":\"reversed\"", lastZero), "equal available timestamps were rejected");
    Check(json.find("\"status\":\"invalid_timestamp\"") != std::string::npos, "reversed timestamps claimed available");
}

void FrozenSnapshotsRejectConcurrentLateStores() {
    Session session(Configuration());
    session.Boundary(10, Time());
    const auto thread = session.RegisterThread("late producer", Time());
    session.RequestStop("synthetic concurrent stop", Time());
    std::atomic<bool> frozen{false};
    std::atomic<bool> accepted{true};
    std::thread writer([&] {
        while (!frozen.load(std::memory_order_acquire)) std::this_thread::yield();
        Event event;
        event.kind = EventKind::Span;
        event.thread = thread;
        event.name = "after freeze";
        event.begin = Clock::time_point{} + 2s;
        event.end = event.begin + 1us;
        accepted.store(session.Store(event), std::memory_order_release);
        session.Release(EventKind::Span, 123);
    });
    session.Freeze(Time());
    const auto snapshot = session.Json();
    frozen.store(true, std::memory_order_release);
    writer.join();
    Check(!accepted.load(std::memory_order_acquire), "frozen session admitted a late store");
    session.ReportLoss();
    Event observation;
    observation.thread = 999;
    observation.subject = 1000;
    Check(!session.Observe(observation), "frozen session admitted a late observation");
    Check(session.Json() == snapshot, "late producer mutated frozen evidence");
}

void RegistrationAndAdmissionLossesAreVisible() {
    auto options = Configuration();
    options.maximumThreads = 1;
    options.maximumQueries = 1;
    Session session(options);
    session.Boundary(10, Time());
    Check(session.RegisterThread("registered producer", Time()) != 0, "first thread registration failed");
    Check(session.RegisterThread("unregistered producer", Time()) == 0, "thread cap was ignored");
    Check(session.Admit(EventKind::Span, 100), "first span admission failed");
    Check(!session.Admit(EventKind::Span, 101), "pending span capacity was ignored");
    session.Release(EventKind::Span, 100);
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "rejected admission leaked pending work");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(json.find("\"losses\":{\"events\":2") != std::string::npos, "unassigned registration/admission losses disappeared");
    Check(json.find("\"unassigned_events\":2") != std::string::npos, "losses without a registered producer were misattributed to a thread");
    Check(json.find("\"state\":\"partial\"") != std::string::npos, "lossy registration claimed complete capture");
}

void MemoryBudgetRejectsExtraThread() {
    auto options = Configuration();
    options.recordsPerThread = 1;
    options.maximumBytes = sizeof(Event);
    Session session(options);
    session.Boundary(10, Time());
    const auto thread = session.RegisterThread("last fitting producer", Time());
    Check(thread != 0, "exactly fitting thread buffer was rejected");
    const auto cold = session.RegisterThread("cold producer", Time());
    Check(cold != 0, "cold thread registration reserved event payload");
    Event event;
    event.thread = thread;
    event.name = "admitted producer still records";
    event.begin = Time();
    Check(session.Store(event), "memory rejection broke a previously admitted producer");
    event.thread = cold;
    Check(!session.Store(event), "event chunks exceeded the total memory budget");
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "memory rejection held the drain open");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    const auto bytes = std::to_string(options.maximumBytes);
    Check(json.find("\"maximum_record_bytes\":" + bytes + ",") != std::string::npos, "effective memory budget was not recorded");
    Check(json.find("\"allocated_record_bytes\":" + bytes) != std::string::npos, "allocated buffer bytes were not recorded");
    Check(json.find("\"unassigned_events\":0") != std::string::npos, "chunk memory refusal was misattributed to registration");
    Check(json.find("\"losses\":{\"events\":1,") != std::string::npos, "chunk memory refusal disappeared");
    Check(json.find("\"state\":\"partial\"") != std::string::npos, "memory registration loss claimed complete capture");
}

std::atomic<std::size_t> chunkAllocationCalls{0};

std::unique_ptr<Event[]> FailSecondChunk(std::size_t count) {
    if (chunkAllocationCalls.fetch_add(1) == 1) throw std::bad_alloc();
    return std::make_unique<Event[]>(count);
}

void ChunkGrowthAndAllocationRollback() {
    auto options = Configuration();
    options.recordsPerThread = 1025;
    options.maximumEvents = 1025;
    options.maximumBytes = 1025 * sizeof(Event);
    options.allocateChunk = FailSecondChunk;
    chunkAllocationCalls = 0;
    Session session(options);
    session.Boundary(10, Time());
    Event event;
    event.thread = session.RegisterThread("chunk growth", Time());
    event.begin = Time();
    for (std::size_t index = 0; index < 1024; ++index) Check(session.Store(event), "first chunk lost a record");
    Check(!session.Store(event), "injected allocation failure was ignored");
    Check(session.Store(event), "allocation failure leaked payload charge or retained-event admission");
    Check(!session.Store(event), "hard per-thread record ceiling was exceeded");
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(chunkAllocationCalls == 3, "chunk allocation happened per event or failed to retry");
    Check(json.find("\"event_count\":1025,") != std::string::npos, "chunk growth lost accepted records");
    Check(json.find("\"allocated_record_bytes\":" + std::to_string(options.maximumBytes) + ",") != std::string::npos,
          "partial final chunk or allocation rollback was not charged exactly");
    Check(json.find("\"losses\":{\"events\":2,") != std::string::npos, "chunk failure and hard-ceiling losses were not exact");
}

void TinyBudgetRefusesChunkWithoutAllocation() {
    auto options = Configuration();
    options.recordsPerThread = 2048;
    options.maximumBytes = sizeof(Event);
    Session session(options);
    session.Boundary(10, Time());
    Event event;
    event.thread = session.RegisterThread("tiny budget", Time());
    Check(event.thread != 0, "tiny payload budget prevented cold thread registration");
    event.begin = Time();
    Check(!session.Store(event), "fixed chunk exceeded a sub-chunk budget");
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(json.find("\"allocated_record_bytes\":0,") != std::string::npos, "refused chunk retained a payload charge");
    Check(json.find("\"event_count\":0,") != std::string::npos, "refused chunk fabricated a record");
    Check(json.find("\"losses\":{\"events\":1,") != std::string::npos, "tiny-budget loss disappeared");
}

void ConcurrentGlobalRecordCeiling() {
    auto options = Configuration();
    options.recordsPerThread = 2048;
    options.maximumEvents = 1500;
    options.maximumBytes = 4096 * sizeof(Event);
    Session session(options);
    session.Boundary(10, Time());
    const auto first = session.RegisterThread("first global claimant", Time());
    const auto second = session.RegisterThread("second global claimant", Time());
    std::atomic<std::size_t> accepted{0};
    const auto produce = [&](Id thread) {
        Event event;
        event.thread = thread;
        event.begin = Clock::time_point{} + 2s;
        for (std::size_t index = 0; index < 1200; ++index) if (session.Store(event)) ++accepted;
    };
    std::thread worker(produce, first);
    produce(second);
    worker.join();
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(accepted == 1500, "concurrent global record admission exceeded or lost capacity");
    Check(json.find("\"event_count\":1500,") != std::string::npos, "global admission count differs from retained records");
    Check(json.find("\"losses\":{\"events\":900,") != std::string::npos, "concurrent rejected records were not counted exactly");
}

class CountingOutput : public std::streambuf {
public:
    std::size_t bytes = 0;
    std::size_t largestWrite = 0;

protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        bytes += static_cast<std::size_t>(count);
        largestWrite = std::max(largestWrite, static_cast<std::size_t>(count));
        return count;
    }
    int_type overflow(int_type character) override {
        if (!traits_type::eq_int_type(character, traits_type::eof())) ++bytes;
        return traits_type::not_eof(character);
    }
};

void StreamedOutputHasBoundedWrites() {
    auto options = Configuration();
    options.recordsPerThread = 2048;
    Session session(options);
    session.Boundary(10, Time());
    Event event;
    event.thread = session.RegisterThread("streamed records", Time());
    event.begin = Time();
    for (std::size_t index = 0; index < 2048; ++index) Check(session.Store(event), "stream fixture lost records");
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    CountingOutput buffer;
    std::ostream output(&buffer);
    session.WriteJson(output);
    Check(buffer.bytes > 100000 && buffer.largestWrite < 16384, "writer buffered a full serialized event collection");
    Check(buffer.bytes == session.Json().size(), "streamed and small-fixture JSON encoders differ");
}

void IntegerTickDurationsRetainPrecisionAndWrap() {
    Session session(Configuration());
    session.Boundary(10, Time());
    const auto thread = session.RegisterThread("precise query collector", Time());
    const auto one = session.RegisterQuery(90, 0, 91, "one tick", false, thread, Time());
    const std::uint64_t large = 1792104620000000000ull;
    session.ResolveQueryTicks(one, QueryStatus::Available, large, large + 1, 1, thread, Time());
    const auto wrap = session.RegisterQuery(90, 1, 92, "wrapped ticks", false, thread, Time());
    session.ResolveQueryTicks(wrap, QueryStatus::Available, std::numeric_limits<std::uint64_t>::max() - 1, 1, 0.5, thread, Time());
    const auto invalid = session.RegisterQuery(90, 2, 93, "reversed ticks", false, thread, Time());
    session.ResolveQueryTicks(invalid, QueryStatus::Available, 140, 100, 1, thread, Time());
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    const auto measured = EventText(json, "one tick");
    Check(measured.find("\"duration_ns\":1,") != std::string::npos, "absolute binary64 rounding erased an integer tick duration");
    Check(measured.find("\"begin_gpu_ticks\":\"1792104620000000000\"") != std::string::npos
        && measured.find("\"end_gpu_ticks\":\"1792104620000000001\"") != std::string::npos, "raw integer tick endpoints lost precision");
    const auto wrapped = EventText(json, "wrapped ticks");
    Check(wrapped.find("\"duration_ns\":1.5,") != std::string::npos
        && wrapped.find("\"timestamp_wraps\":true") != std::string::npos, "modular timestamp wrap was not represented precisely");
    Check(EventText(json, "reversed ticks").find("\"status\":\"invalid_timestamp\"") != std::string::npos,
          "ambiguous reversed ticks claimed a duration");
}

void CancellingChildWorkDoesNotCancelParentOperation() {
    Session session(Configuration());
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    {
        Operation draw("draw");
        const auto preparation = ScheduleJob("optional shader preparation");
        CancelJob(preparation);
    }
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "cancelled child work leaked drain membership");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    const auto operation = json.find("\"kind\":\"operation\"");
    Check(operation != std::string::npos, "parent operation record disappeared");
    const auto end = json.find("\"traceEvents\":", operation);
    const auto terminal = json.find("\"kind\":\"operation\"", operation + 1);
    Check(terminal < end, "parent terminal operation outcome disappeared");
    Check(json.find("\"kind\":\"operation\"", terminal + 1) >= end, "child cancellation fabricated another parent operation outcome");
    Check(EventText(json, "draw").find("\"status\":\"complete\"") != std::string::npos, "cancelled child poisoned successful parent");
}

void FreezeSynchronizesAnActiveWriter() {
    auto options = Configuration();
    options.recordsPerThread = 512;
    Session session(options);
    session.Boundary(10, Time());
    const auto thread = session.RegisterThread("concurrent producer", Time());
    std::atomic<bool> firstStored{false};
    std::atomic<std::size_t> accepted{0};
    std::thread writer([&] {
        for (std::size_t index = 0; index < 256; ++index) {
            Event event;
            event.kind = EventKind::Span;
            event.thread = thread;
            event.subject = 1000 + index;
            event.name = "concurrent span";
            event.begin = Clock::time_point{} + 2s;
            event.end = event.begin + 1us;
            if (session.Store(event)) accepted.fetch_add(1, std::memory_order_relaxed);
            firstStored.store(true, std::memory_order_release);
        }
    });
    while (!firstStored.load(std::memory_order_acquire)) std::this_thread::yield();
    session.RequestStop("concurrent stop", Time());
    session.Freeze(Time());
    const auto snapshot = session.Json();
    writer.join();
    Check(session.Json() == snapshot, "active writer changed frozen evidence");
    const auto expected = "\"event_count\":" + std::to_string(accepted.load(std::memory_order_relaxed)) + ",";
    Check(Compact(snapshot).find(expected) != std::string::npos, "freeze lost or duplicated an accepted event");
}

void NestedSessionOutcomeHooksCannotAlterOuterOwners() {
    Session outer(Configuration());
    Session inner(Configuration());
    SessionScope binding(outer, Time);
    outer.Boundary(10, Time());
    {
        Operation draw("outer draw");
        const auto token = ScheduleJob("outer preparation");
        JobScope job(token);
        {
            SessionScope nested(inner, Time);
            inner.Boundary(10, Time());
            SetOperationOutcome(Status::Failed);
            SetJobOutcome(Status::Failed);
        }
    }
    outer.Boundary(11, Time());
    outer.Boundary(12, Time());
    Check(outer.Ready(Time()), "nested outcome hooks leaked outer pending work");
    outer.Freeze(Time());
    const auto json = Compact(outer.Json());
    Check(json.find("\"status\":\"failed\"") == std::string::npos, "inner session outcome hook altered outer owner");
    Check(EventText(json, "outer draw").find("\"status\":\"complete\"") != std::string::npos, "outer operation outcome was not restored");
}

void IdleObservationClipsWithoutHoldingDrain() {
    Session session(Configuration());
    SessionScope binding(session, Time);
    Span idle("synthetic idle wait", false);
    Check(idle.GetId() != 0, "armed idle observation was not registered");
    now += 1us;
    session.Boundary(10, Time());
    now += 20ms;
    session.Boundary(11, Time());
    now += 20ms;
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "idle observation held asynchronous drain open");
    session.Freeze(Time());
    const auto snapshot = session.Json();
    const auto event = EventText(Compact(snapshot), "synthetic idle wait");
    Check(event.find("clipped_at_window_close") != std::string::npos, "unfinished idle observation disappeared");
    Check(event.find("\"status\":\"unknown\"") != std::string::npos, "clipped observation claimed completed work");
    idle.Finish();
    Check(session.Json() == snapshot, "finishing clipped idle observation mutated frozen evidence");
}

void HostSpanBridgePreservesLifetime() {
    Session session(Configuration());
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    {
        HostSpan span("host bridge");
        now += 2us;
        span.Mark("host bridge phase");
        now += 3us;
        span.Finish();
        span.Finish();
        now += 10us;
    }
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    Check(session.Ready(Time()), "host bridge destruction leaked a pending span");
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    const auto span = EventText(json, "host bridge");
    const auto phase = EventText(json, "host bridge phase");
    Check(span.find("\"ts_ns\":0,") != std::string::npos && span.find("\"end_ns\":5000,") != std::string::npos,
          "host bridge destructor changed the explicitly finished interval");
    Check(phase.find("\"end_ns\":2000,") != std::string::npos, "host bridge lost its phase interval");
    Check(json.find("\"event_count\":2,") != std::string::npos, "host bridge duplicated or omitted span records");
}

void DisabledHostSpanDoesNotInitializeCapture() {
    Check(std::getenv("APS5_TRACE_DETAIL") == nullptr && std::getenv("APS5_CAPTURE_RUN_ID") == nullptr
        && std::getenv("APS5_TRACE_FRAMES") == nullptr && std::getenv("APS5_TRACE_SECONDS") == nullptr,
        "disabled bridge regression requires an isolated capture environment");
    Check(!Configured() && NextId() == 0, "disabled trace allocated an identity");
    {
        HostSpan span("disabled host bridge");
        span.Mark("disabled phase");
        span.Finish();
        span.Finish();
    }
    Check(NextId() == 0 && Current().operation == 0 && Current().job == 0, "disabled bridge changed capture state");
}

thread_local CpuSample cpuSample{71, 100, 10000000, CpuStatus::Measured};
CpuSample SyntheticCpu() { return cpuSample; }
thread_local CycleSample cycleSample{71, 100, CpuStatus::Measured};
thread_local std::uint64_t cycleSamples = 0;
thread_local std::uint64_t cpuSamples = 0;
CycleSample SyntheticCycles() { ++cycleSamples; return cycleSample; }
CpuSample CountedCpu() { ++cpuSamples; return cpuSample; }
CycleSample FailingCycles() { throw std::runtime_error("synthetic cycle counter failure"); }

void ThreadCyclesRemainRawInclusiveCounters() {
    auto options = Configuration();
    options.threadCycles = true;
    options.sampleCycles = SyntheticCycles;
    options.sampleCpu = CountedCpu;
    cpuSamples = cycleSamples = 0;
    cycleSample = {71, std::uint64_t{1} << 60, CpuStatus::Measured};
    Session session(options);
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    { Span zero("observed cycle zero"); }
    {
        Span span("measured cycle scope");
        now += 5ms;
        ++cycleSample.cycles;
        span.Mark("measured cycle phase");
        now += 5ms;
        cycleSample.cycles += 62;
    }
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    const auto span = EventText(json, "measured cycle scope");
    const auto phase = EventText(json, "measured cycle phase");
    Check(span.find("\"thread_cycles_status\":\"measured\"") != std::string::npos, "cycle sample was not measured");
    Check(span.find("\"thread_cycles_begin\":\"1152921504606846976\"") != std::string::npos
        && span.find("\"thread_cycles_end\":\"1152921504606847039\"") != std::string::npos
        && span.find("\"thread_cycles_delta\":\"63\"") != std::string::npos, "integer cycle endpoints or delta lost precision");
    Check(phase.find("\"thread_cycles_delta\":\"1\"") != std::string::npos, "phase cycles did not preserve their selected interval");
    Check(EventText(json, "observed cycle zero").find("\"thread_cycles_delta\":\"0\"") != std::string::npos,
          "observed cycle zero became unavailable");
    Check(span.find("\"thread_cpu_status\":\"not_requested\"") != std::string::npos && cpuSamples == 0,
          "cycle mode implicitly enabled expensive thread time sampling");
    Check(cycleSamples == 5, "selected span cycle sampling did not match begin, phase and end boundaries");
    Check(span.find("\"thread_cycles_ns\"") == std::string::npos
        && json.find("\"thread_cycles_time_conversion\":\"unavailable\"") != std::string::npos,
          "cycle samples claimed an exact time conversion");
    Check(span.find("\"end_ns\":10000000") != std::string::npos, "cycle delta replaced elapsed wall time");
}

void CycleFailuresAndClippingRemainUnavailable() {
    auto options = Configuration();
    options.threadCycles = true;
    options.sampleCycles = SyntheticCycles;
    Session session(options);
    SessionScope binding(session, Time);
    cycleSample = {71, 100, CpuStatus::Measured};
    Span idle("clipped cycle wait", false);
    session.Boundary(10, Time());
    { Span span("nonmonotonic cycles"); --cycleSample.cycles; }
    { Span span("changed cycle thread"); ++cycleSample.nativeThread; }
    cycleSample.status = CpuStatus::Failed;
    { Span span("failed cycles"); }
    cycleSample.status = CpuStatus::Unavailable;
    { Span span("unsupported cycles"); }
    now += 1us;
    session.Boundary(11, Time());
    now += 1us;
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    for (const auto& [name, status] : std::initializer_list<std::pair<const char*, const char*>>{
        {"nonmonotonic cycles", "nonmonotonic"}, {"changed cycle thread", "thread_changed"},
        {"failed cycles", "failed"}, {"unsupported cycles", "unavailable"}, {"clipped cycle wait", "clipped"}}) {
        const auto event = EventText(json, name);
        Check(event.find(std::string("\"thread_cycles_status\":\"") + status + '"') != std::string::npos,
              "unavailable cycle reason was erased");
        Check(event.find("\"thread_cycles_begin\":null,\"thread_cycles_end\":null,\"thread_cycles_delta\":null,\"thread_cycles_native_id\":null")
              != std::string::npos, "unavailable cycles became measured values");
    }
    auto failingOptions = Configuration();
    failingOptions.threadCycles = true;
    failingOptions.sampleCycles = FailingCycles;
    Session failed(failingOptions);
    Check(failed.SampleCycles().status == CpuStatus::Failed, "counter exception escaped or became a measured zero");
}

void DisabledCycleModeDoesNotSample() {
    auto options = Configuration();
    options.sampleCycles = SyntheticCycles;
    cycleSamples = 0;
    Session session(options);
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    { Span span("cycle mode off"); span.Mark("off cycle phase"); }
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(cycleSamples == 0 && session.SampleCycles().status == CpuStatus::NotRequested,
          "disabled cycle mode invoked a counter");
    Check(EventText(json, "cycle mode off").find("\"thread_cycles_status\"") == std::string::npos,
          "disabled cycle mode expanded every canonical record");
    Check(json.find("\"thread_cycles_requested\":false") != std::string::npos, "disabled cycle capability was not explicit");
}

void PerfettoViewIsOptionalWithoutEvidenceLoss() {
    std::string canonical;
    for (const bool view : {false, true}) {
        auto options = Configuration();
        options.perfettoView = view;
        Session session(options);
        session.Boundary(10, Time());
        const auto thread = session.RegisterThread("view fixture", Time());
        Event event;
        event.id = 91;
        event.subject = 92;
        event.kind = EventKind::Attribute;
        event.thread = thread;
        event.name = "preserved canonical evidence";
        event.value = 123;
        event.begin = Time();
        Check(session.Store(event), "optional-view fixture event was lost");
        session.Boundary(11, Time());
        session.Boundary(12, Time());
        session.Freeze(Time());
        const auto json = Compact(session.Json());
        auto record = EventText(json, "preserved canonical evidence");
        const auto threadField = "\"thread_id\":\"" + std::to_string(thread) + '"';
        const auto field = record.find(threadField);
        Check(field != std::string::npos, "canonical thread binding missing");
        record.replace(field, threadField.size(), "\"thread_id\":\"synthetic\"");
        if (view) Check(record == canonical, "compatibility view changed canonical evidence");
        else canonical = record;
        Check((json.find("\"traceEvents\":[") != std::string::npos) == view, "Perfetto view request was ignored");
        Check(json.find(std::string("\"perfetto_view_included\":") + (view ? "true" : "false")) != std::string::npos,
              "view inclusion metadata contradicts emitted output");
        Check(json.find("\"event_count\":1") != std::string::npos && json.find("\"lost\":0") != std::string::npos,
              "omitted redundant view became canonical loss");
        Check(json.find("\"records_per_thread_scope\":\"per_thread\"") != std::string::npos
            && json.find("\"maximum_events_scope\":\"all_threads\"") != std::string::npos
            && json.find("\"budget_scope\":\"stored_event_chunk_payloads\"") != std::string::npos,
              "capacity or memory budget scope was ambiguous");
    }
}

void ThreadCpuRemainsDistinctFromElapsed() {
    auto options = Configuration();
    options.threadCpu = true;
    options.sampleCpu = SyntheticCpu;
    cpuSample = {71, 100, 10000000, CpuStatus::Measured};
    Session session(options);
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    { Span zero("quantized cpu zero"); }
    {
        Span span("measured cpu scope");
        now += 5ms;
        cpuSample.ticks += 3;
        span.Mark("measured cpu phase");
        now += 5ms;
    }
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    const auto span = EventText(json, "measured cpu scope");
    const auto phase = EventText(json, "measured cpu phase");
    Check(EventText(json, "quantized cpu zero").find("\"thread_cpu_ns\":0") != std::string::npos, "observed quantized zero became unavailable");
    Check(span.find("\"thread_cpu_status\":\"measured\"") != std::string::npos, "measured CPU sample was lost");
    Check(span.find("\"thread_cpu_ns\":300") != std::string::npos, "CPU duration did not use integer counter delta");
    Check(span.find("\"end_ns\":10000000") != std::string::npos, "CPU duration replaced elapsed wall duration");
    Check(phase.find("\"thread_cpu_ns\":300") != std::string::npos, "phase CPU counter did not preserve its own interval");
    Check(json.find("\"thread_cpu_resolution_ns\":null") != std::string::npos, "CPU counter units were claimed as measured resolution");
}

void CpuFailuresAndClippingAreExplicit() {
    auto options = Configuration();
    options.threadCpu = true;
    options.sampleCpu = SyntheticCpu;
    Session session(options);
    SessionScope binding(session, Time);
    cpuSample = {71, 100, 10000000, CpuStatus::Measured};
    Span idle("clipped cpu wait", false);
    session.Boundary(10, Time());
    {
        Span backwards("nonmonotonic cpu");
        --cpuSample.ticks;
    }
    {
        Span changed("changed cpu thread");
        ++cpuSample.nativeThread;
    }
    cpuSample.status = CpuStatus::Failed;
    { Span failed("failed cpu sample"); }
    now += 1us;
    session.Boundary(11, Time());
    now += 1us;
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    for (const auto& [name, status] : std::initializer_list<std::pair<const char*, const char*>>{
        {"nonmonotonic cpu", "nonmonotonic"}, {"changed cpu thread", "thread_changed"},
        {"failed cpu sample", "failed"}, {"clipped cpu wait", "clipped"}}) {
        const auto event = EventText(json, name);
        Check(event.find(std::string("\"thread_cpu_status\":\"") + status + '"') != std::string::npos, "CPU failure reason was erased");
        Check(event.find("\"thread_cpu_ns\":null") != std::string::npos, "unavailable CPU sample became a measured zero");
    }
}

void NativeClockAndCounterAvailabilityAreDeclared() {
    Session session(Configuration());
    SessionScope binding(session, Time);
    session.Boundary(10, Time());
    RecordCounter("observed copy", 0, "bytes", 91);
    RecordHardwareCounter(91, "unavailable hardware", QueryStatus::Unsupported, 123, "invocations");
    CalibrationSample sample;
    sample.available = true;
    sample.gpuTicks = 1792104620000000000;
    sample.hostTicks = 9001;
    sample.conversionHostTicks = 9010;
    sample.hostFrequency = 10000000;
    sample.hostDomain = "query_performance_counter";
    sample.before = Time();
    sample.after = Time() + 2us;
    sample.deviationNs = 17;
    sample.periodNs = 1;
    sample.validBits = 64;
    Calibrate(91, sample);
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(json.find("\"native_thread_id\":\"") != std::string::npos || json.find("\"native_thread_id_source\":\"unavailable\"") != std::string::npos,
          "native OS thread ID or explicit unavailability was not exported");
    Check(json.find("\"native_anchor\":{") != std::string::npos && json.find("\"origin_steady_ns\":\"") != std::string::npos,
          "native clock domain or steady origin was omitted");
    Check(EventText(json, "observed copy").find("\"counter_value\":\"0\"") != std::string::npos, "observed zero was not preserved");
    Check(EventText(json, "unavailable hardware").find("\"counter_value\":null") != std::string::npos, "unsupported hardware counter became zero");
    const auto calibration = EventText(json, "gpu_clock_calibration");
    Check(calibration.find("\"calibration_gpu_ticks\":\"1792104620000000000\"") != std::string::npos, "raw GPU clock ticks lost precision");
    Check(calibration.find("\"calibration_conversion_host_ticks\":\"9010\"") != std::string::npos, "host conversion counter was omitted");
    Check(calibration.find("\"deviation_ns\":17") != std::string::npos, "raw Vulkan max deviation was silently combined with bracket uncertainty");
}

void JournalCommitsBoundedRecoverablePrefixes() {
    auto options = Configuration();
    options.journal = true;
    options.recordsPerThread = 8192;
    Session session(options);
    const auto thread = session.RegisterThread("journal producer", Time());
    session.Boundary(10, Time());
    for (int index = 0; index < 4097; ++index) {
        Event event;
        event.thread = thread;
        event.begin = Time();
        event.value = index;
        Check(session.Store(event), "journal fixture could not retain its records");
    }
    std::ostringstream output;
    Check(!session.WriteJournal(output), "journal claimed caught-up while a second event chunk remained");
    const auto prefix = output.str();
    Check(!prefix.empty() && prefix.back() == '\n', "journal prefix lacked newline commit");
    Check(prefix.find("\"count\":4096,") != std::string::npos, "journal batch was not bounded to 4096 records");
    Check(prefix.find("\"kind\":\"footer\"") == std::string::npos, "live prefix claimed finalized output");
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    for (int pass = 0; pass < 10 && !session.WriteJournal(output); ++pass) {}
    const auto journal = output.str();
    Check(journal.find("\"emitted_events\":4097") != std::string::npos, "journal omitted retained records across chunk boundaries");
    Check(journal.find("\"kind\":\"footer\"") != std::string::npos, "journal never committed a final footer");
    Check(journal.find("\"complete\":true") == std::string::npos, "journal alone claimed a complete capture");
    std::istringstream lines(journal);
    std::string line;
    std::uint64_t sequence = 0;
    std::size_t retained = 0;
    while (std::getline(lines, line)) {
        Check(line.find("\"sequence\":" + std::to_string(sequence++) + ',') != std::string::npos,
              "journal sequence repeated or skipped a committed record");
        std::size_t position = 0;
        while ((position = line.find("\"kind\":\"attribute\"", position)) != std::string::npos) {
            ++retained;
            ++position;
        }
    }
    Check(retained == 4097, "journal payload omitted or duplicated canonical event rows");
    const auto before = output.str();
    session.WriteJournal(output);
    Check(output.str() == before, "journal appended after its committed footer");
}

void JournalBudgetAndIoFailureDoNotLoseTraceRecords() {
    for (const bool failIo : {false, true}) {
        auto options = Configuration();
        options.journal = true;
        options.maximumJournalBytes = 2048;
        Session session(options);
        SessionScope binding(session, Time);
        session.Boundary(10, Time());
        { Span span("journal failure survives"); now += 1us; }
        std::ostringstream output;
        if (failIo) output.setstate(std::ios::badbit);
        session.WriteJournal(output);
        session.Boundary(11, Time());
        session.Boundary(12, Time());
        session.Freeze(Time());
        for (int pass = 0; pass < 10 && !session.WriteJournal(output); ++pass) {}
        const auto json = Compact(session.Json());
        Check(json.find(failIo ? "\"failed\":true" : "\"byte_budget_exhausted\":true") != std::string::npos,
              "journal failure was not exported");
        Check(output.str().size() <= options.maximumJournalBytes, "journal exceeded its hard byte budget");
        Check(EventText(json, "journal failure survives").find("\"end_ns\":1000") != std::string::npos,
              "journal failure changed authoritative retained trace data");
        Check(json.find("\"losses\":{\"events\":0,") != std::string::npos, "journal failure fabricated hot-path event loss");
    }
}

void JournalSnapshotsCanRunBesideProducers() {
    auto options = Configuration();
    options.journal = true;
    options.recordsPerThread = 8192;
    Session session(options);
    session.RegisterThread("empty journal predecessor", Time());
    session.Boundary(10, Time());
    std::atomic<bool> finished{false};
    std::atomic<std::size_t> accepted{0};
    std::thread producer([&] {
        const auto thread = session.RegisterThread("concurrent journal producer", Time());
        for (int index = 0; index < 5000; ++index) {
            Event event;
            event.thread = thread;
            event.begin = Time();
            event.value = index;
            if (session.Store(event)) ++accepted;
        }
        finished.store(true, std::memory_order_release);
    });
    std::ostringstream output;
    while (!finished.load(std::memory_order_acquire)) session.WriteJournal(output);
    producer.join();
    session.Boundary(11, Time());
    session.Boundary(12, Time());
    session.Freeze(Time());
    for (int pass = 0; pass < 20 && !session.WriteJournal(output); ++pass) {}
    Check(accepted == 5000, "journal snapshots interfered with producer admission");
    const auto text = output.str();
    Check(text.find("\"emitted_events\":5000") != std::string::npos, "concurrent journal lost or duplicated accepted events");
    Check(text.find("\"kind\":\"footer\"") != std::string::npos, "concurrent journal failed to finish after freeze");
    std::istringstream lines(text);
    std::vector<std::string> definitions;
    std::string line;
    while (std::getline(lines, line)) {
        const bool definition = line.find("\"kind\":\"thread\"") != std::string::npos;
        const bool events = line.find("\"kind\":\"events\"") != std::string::npos;
        if (!definition && !events) continue;
        const auto start = line.find("\"thread_id\":\"") + std::string_view("\"thread_id\":\"").size();
        const auto id = line.substr(start, line.find('"', start) - start);
        if (definition) definitions.push_back(id);
        else Check(std::find(definitions.begin(), definitions.end(), id) != definitions.end(),
                   "journal emitted a hot thread batch before its immutable thread definition");
    }
    const auto snapshot = session.Json();
    session.WriteJournal(output);
    Check(session.Json() == snapshot, "completed journal changed immutable trace evidence");
}

void PresentOrdinalBindingIsExplicitAndSessionLocal() {
    auto options = Configuration();
    options.start = 0;
    options.length = 1;
    Session session(options);
    SessionScope binding(session, Time);
    Check(!LastPresentOrdinal(), "new session inherited an unrelated present ordinal");
    PresentBoundary(0, Time());
    Check(LastPresentOrdinal() == 0, "observed zero ordinal became unavailable");
    RecordPresent(91, 92, LastPresentOrdinal(), Status::Complete);
    {
        Session nested(options);
        SessionScope inner(nested, Time);
        Check(!LastPresentOrdinal(), "nested session inherited outer present ordinal");
        PresentBoundary(7, Time());
        Check(LastPresentOrdinal() == 7, "nested session failed to bind its own ordinal");
    }
    Check(LastPresentOrdinal() == 0, "nested session altered the outer ordinal");
    RecordPresent(93, 92, std::nullopt, Status::Failed);
    PresentBoundary(1, Time());
    session.Freeze(Time());
    const auto json = Compact(session.Json());
    Check(json.find("\"present_ordinal\":\"0\",\"present_ordinal_status\":\"observed\"") != std::string::npos,
          "present ordinal was not explicitly distinguished from entity ID");
    Check(json.find("\"present_ordinal\":null,\"present_ordinal_status\":\"unavailable\"") != std::string::npos,
          "unobserved present ordinal became zero");
}

void InvalidRawCalibrationCannotClaimAvailableEvidence() {
    for (const bool invalidDomain : {false, true}) {
        Session session(Configuration());
        SessionScope binding(session, Time);
        session.Boundary(10, Time());
        CalibrationSample sample;
        sample.available = true;
        sample.gpuTicks = 100;
        sample.hostTicks = 200;
        sample.conversionHostTicks = 201;
        sample.hostFrequency = 10000000;
        sample.hostDomain = invalidDomain ? "unrecognized_native_clock" : "query_performance_counter";
        sample.before = sample.after = Time();
        sample.periodNs = 1;
        sample.validBits = invalidDomain ? 64 : 65;
        Calibrate(91, sample);
        session.Boundary(11, Time());
        session.Boundary(12, Time());
        session.Freeze(Time());
        const auto calibration = EventText(Compact(session.Json()), "gpu_clock_calibration");
        Check(calibration.find("\"status\":\"failed\"") != std::string::npos, "invalid raw calibration claimed complete evidence");
        Check(calibration.find("\"calibration_gpu_ticks\":null") != std::string::npos
            && calibration.find("\"period_ns\":null") != std::string::npos
            && calibration.find("\"valid_bits\":0") != std::string::npos,
            "failed raw calibration retained invalid measured timing fields");
    }
}

}

int main() {
    try {
        BoundariesAndLateQueries();
        NestedScopesAndExceptions();
        AsyncWorkSurvivesClosure();
        DeferredSameThreadJobsUseExecutionParents();
        InlineJobsPreserveLexicalParents();
        CancellationAndNestedBindings();
        CapacityAndDeadlineRemainVisible();
        QueryFailuresAreNotDurations();
        DictionaryConflictsAndInterruptedWindows();
        QuerySlotsAreNamespacedByBatch();
        EqualAndInvalidTimestamps();
        FrozenSnapshotsRejectConcurrentLateStores();
        RegistrationAndAdmissionLossesAreVisible();
        MemoryBudgetRejectsExtraThread();
        ChunkGrowthAndAllocationRollback();
        TinyBudgetRefusesChunkWithoutAllocation();
        ConcurrentGlobalRecordCeiling();
        StreamedOutputHasBoundedWrites();
        IntegerTickDurationsRetainPrecisionAndWrap();
        CancellingChildWorkDoesNotCancelParentOperation();
        FreezeSynchronizesAnActiveWriter();
        NestedSessionOutcomeHooksCannotAlterOuterOwners();
        IdleObservationClipsWithoutHoldingDrain();
        HostSpanBridgePreservesLifetime();
        DisabledHostSpanDoesNotInitializeCapture();
        ThreadCpuRemainsDistinctFromElapsed();
        ThreadCyclesRemainRawInclusiveCounters();
        CycleFailuresAndClippingRemainUnavailable();
        DisabledCycleModeDoesNotSample();
        PerfettoViewIsOptionalWithoutEvidenceLoss();
        CpuFailuresAndClippingAreExplicit();
        NativeClockAndCounterAvailabilityAreDeclared();
        JournalCommitsBoundedRecoverablePrefixes();
        JournalBudgetAndIoFailureDoNotLoseTraceRecords();
        JournalSnapshotsCanRunBesideProducers();
        PresentOrdinalBindingIsExplicitAndSessionLocal();
        InvalidRawCalibrationCannotClaimAvailableEvidence();
        std::cout << "Draw trace lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

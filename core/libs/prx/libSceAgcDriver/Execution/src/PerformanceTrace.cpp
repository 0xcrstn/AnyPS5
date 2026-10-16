#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/DrawTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/TraceFile.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/TraceRevision.hpp"
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace AgcDriver {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t GpuRangeCapacity = TraceThread::DefaultCapacity;

struct Session {
    std::mutex mutex;
    std::vector<std::unique_ptr<TraceThread>> threads;
    bool started = false;
    std::optional<TraceWindow> window;
    bool windowOpen = false;
    std::vector<std::vector<TraceThread::Total>> windowBase;
    std::optional<TraceInterval> interval;
    bool intervalOpen = false;
    std::uint64_t intervalFirstPresent = 0;
    double intervalGpuBusy = 0;
    std::vector<std::vector<TraceThread::Total>> intervalBase;
    std::vector<Clock::time_point> intervalPresents;
    Clock::time_point first{};
    std::uint64_t present = 0;
    std::uint64_t firstPresent = 0;
    double gpuBusyAtOpen = 0;
    std::optional<TraceClockSample> gpuClock;
    std::vector<Clock::time_point> presents;
};

Session& session() {
    static auto& state = *new Session();
    return state;
}

std::unique_ptr<ProfileOutput>& writer() {
    static std::unique_ptr<ProfileOutput> output;
    return output;
}

std::unique_ptr<ProfileOutput>& summaryWriter() {
    static std::unique_ptr<ProfileOutput> output;
    return output;
}

std::filesystem::path traceDirectory() {
    const char* directory = std::getenv("APS5_TRACE_DIR");
    return std::filesystem::path(directory != nullptr ? directory : ".");
}

const std::string& workloadRunId() {
    static const std::string id = [] {
        const auto* run = std::getenv("APS5_CAPTURE_RUN_ID");
        return run != nullptr ? std::string(run) : std::string();
    }();
    return id;
}

std::uint64_t processId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string operatingSystem() {
#ifdef _WIN32
    using QueryVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const auto query = reinterpret_cast<QueryVersion>(reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (query == nullptr || query(&version) != 0) throw std::runtime_error("RtlGetVersion failed");
    return "Windows " + std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + "." + std::to_string(version.dwBuildNumber);
#else
    utsname name{};
    if (::uname(&name) != 0) throw std::system_error(errno, std::generic_category(), "uname");
    return std::string(name.sysname) + " " + name.release;
#endif
}

std::vector<std::pair<std::string, std::string>> environment() {
#ifdef _WIN32
    char** entries = _environ;
#else
    char** entries = environ;
#endif
    std::vector<std::pair<std::string, std::string>> result;
    for (; entries != nullptr && *entries != nullptr; ++entries) {
        const std::string_view entry = *entries;
        const auto equals = entry.find('=');
        if (!entry.starts_with("APS5_") || equals == std::string_view::npos) continue;
        std::string value(entry.substr(equals + 1));
        if (value.find_first_of("/\\") != std::string::npos) value = "<path>";
        result.emplace_back(std::string(entry.substr(0, equals)), std::move(value));
    }
    std::sort(result.begin(), result.end());
    return result;
}

TraceSummary identity(std::uint32_t width, std::uint32_t height, const VulkanDevice* device) {
    TraceSummary summary;
    summary.commit = APS5_TRACE_COMMIT;
    if (APS5_TRACE_DIRTY >= 0) summary.dirty = APS5_TRACE_DIRTY != 0;
    summary.environment = environment();
    summary.gpu = device != nullptr ? device->DeviceName() : std::string();
    summary.driver = device != nullptr ? device->DriverInfo() : std::string();
    summary.os = operatingSystem();
    summary.width = width;
    summary.height = height;
    return summary;
}

std::vector<std::vector<TraceThread::Total>> totalsOf(const std::vector<std::unique_ptr<TraceThread>>& threads) {
    std::vector<std::vector<TraceThread::Total>> result;
    for (const auto& thread : threads) result.push_back(thread->Collect().totals);
    return result;
}

double seconds(Clock::duration elapsed) {
    return std::chrono::duration<double>(elapsed).count();
}

std::string workloadJson(const MeasurementWorkload::Snapshot& snapshot, const TraceSummary& summary, std::uint64_t process, std::string_view run) {
    std::string text = "{\"schema_version\":1,\"document_type\":\"anyps5.measurement-workload\",\"run_id\":";
    TraceFormat::AppendString(text, run);
    text += ",\"producer\":{\"pid\":" + std::to_string(process) + ",\"revision\":";
    TraceFormat::AppendString(text, summary.commit);
    text += ",\"dirty\":" + (summary.dirty ? std::string(*summary.dirty ? "true" : "false") : "null") + "},\"trace\":";
    TraceFormat::AppendString(text, summary.trace);
    text += ",\"clock\":{\"domain\":\"steady_clock\",\"unit\":\"ns\",\"origin\":\"implementation_defined_steady_epoch\"},\"capture\":{\"spec\":";
    TraceFormat::AppendString(text, summary.window);
    text += ",\"open_present\":" + std::to_string(snapshot.openPresent) + ",\"close_present\":" + std::to_string(snapshot.closePresent)
        + ",\"open_ns\":" + std::to_string(snapshot.openNs) + ",\"close_ns\":" + std::to_string(snapshot.closeNs) + "}";
    const auto counts = [&](const MeasurementWorkload::Counts& values) {
        text += '{';
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0) text += ',';
            TraceFormat::AppendString(text, MeasurementWorkload::Names[index]);
            text += ':' + std::to_string(values[index]);
        }
        text += '}';
    };
    const auto pending = [&](const MeasurementWorkload::Pending& values) {
        text += '[' + std::to_string(values[0]) + ',' + std::to_string(values[1]) + ',' + std::to_string(values[2]) + ']';
    };
    text += ",\"counts\":";
    counts(snapshot.counts);
    text += ",\"frames\":[";
    for (std::size_t index = 0; index < snapshot.frames.size(); ++index) {
        const auto& row = snapshot.frames[index];
        if (index != 0) text += ',';
        text += "{\"present\":" + std::to_string(row.present) + ",\"begin_ns\":" + std::to_string(row.beginNs)
            + ",\"end_ns\":" + std::to_string(row.endNs) + ",\"counts\":";
        counts(row.counts);
        text += ",\"pending_at_begin\":";
        pending(row.pendingAtBegin);
        text += ",\"pending_at_end\":";
        pending(row.pendingAtEnd);
        text += '}';
    }
    text += "],\"coverage\":{\"closed\":" + std::string(snapshot.closed ? "true" : "false")
        + ",\"maximum_intervals\":65536,\"lost_intervals\":" + std::to_string(snapshot.lost)
        + ",\"overflow\":" + (snapshot.overflow ? "true" : "false")
        + ",\"lifetime_error\":" + (snapshot.lifetimeError ? "true" : "false") + ",\"pending_at_open\":";
    pending(snapshot.pendingAtOpen);
    text += ",\"pending_at_close\":";
    pending(snapshot.pendingAtClose);
    text += "},\"scope\":\"selected host-observed request, command and deferred-job paths\",\"limitations\":[\"Counts are not GPU-executed indirect work, scene equivalence or rendering correctness.\",\"Present buckets describe host observation order, not GPU frame ownership.\",\"Instrumentation overhead is unmeasured.\"]}";
    return text;
}

}

MeasurementWorkload* PerformanceTrace::Workload() noexcept {
    static MeasurementWorkload* ledger = []() noexcept -> MeasurementWorkload* {
        const auto* enabled = std::getenv("APS5_TRACE_WORKLOAD");
        if (enabled == nullptr || std::string_view(enabled) == "0") return nullptr;
        try {
            if (std::string_view(enabled) != "1") throw std::invalid_argument("APS5_TRACE_WORKLOAD must be 0 or 1");
            const auto* frames = std::getenv("APS5_TRACE_FRAMES");
            const auto window = TraceWindow::Parse(frames, std::getenv("APS5_TRACE_SECONDS"));
            if (frames == nullptr || !window) throw std::invalid_argument("measurement workload requires a frame window");
            const std::string_view spec(frames);
            std::size_t length = 0;
            const auto part = spec.substr(spec.find(':') + 1);
            const auto [last, error] = std::from_chars(part.data(), part.data() + part.size(), length);
            if (error != std::errc{} || last != part.data() + part.size()) throw std::invalid_argument("measurement workload length invalid");
            const std::string_view id = workloadRunId();
            if (id.size() != 36) throw std::invalid_argument("measurement workload requires a canonical run UUID");
            for (std::size_t index = 0; index < id.size(); ++index) {
                const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
                if (dash ? id[index] != '-' : !((id[index] >= '0' && id[index] <= '9') || (id[index] >= 'a' && id[index] <= 'f')))
                    throw std::invalid_argument("measurement workload requires a canonical run UUID");
            }
            return new MeasurementWorkload(length);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[trace] measurement workload unavailable: %s\n", error.what());
            return nullptr;
        }
    }();
    return ledger;
}

void PerformanceTrace::BindThread(const char* name, std::size_t capacity) {
    DrawTrace::BindThread(name);
    if (!Configured() || local != nullptr) return;
    const bool windowed = std::getenv("APS5_TRACE_FRAMES") != nullptr || std::getenv("APS5_TRACE_SECONDS") != nullptr;
    if (windowed && capacity == TraceThread::DefaultCapacity) capacity = TraceThread::ParseCapacity(std::getenv("APS5_TRACE_EVENTS"));
    auto bound = std::make_unique<TraceThread>(name, presentCount, windowed ? capacity : 0, buffering);
    auto& state = session();
    std::lock_guard lock(state.mutex);
    local = bound.get();
    state.threads.push_back(std::move(bound));
}

void PerformanceTrace::OpenInterval(Clock::time_point now, std::uint64_t present) {
    auto& state = session();
    state.interval->Restart(now - state.first);
    state.intervalOpen = true;
    state.intervalFirstPresent = present;
    state.intervalGpuBusy = VulkanDevice::PresentCounts().gpuBusyMs;
    state.intervalPresents.assign(1, now);
    if (summaryWriter()) return;
    summaryWriter() = std::make_unique<ProfileOutput>([directory = traceDirectory()](std::string_view text) {
        try {
            std::filesystem::create_directories(directory);
            TraceFile::AppendLine(directory / "aps5-summary.jsonl", text);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[trace] appending to aps5-summary.jsonl failed: %s\n", error.what());
            std::fflush(stderr);
            throw;
        }
    }, std::numeric_limits<std::size_t>::max());
    ProfilePrint_nid_no_patch("[trace] summarizing %s from present %llu\n", state.interval->Describe().c_str(), static_cast<unsigned long long>(present));
}

void PerformanceTrace::FlushInterval(Clock::time_point now, std::uint32_t width, std::uint32_t height, const VulkanDevice* device) {
    auto& state = session();
    state.intervalBase.resize(state.threads.size());
    std::vector<TraceThread::Snapshot> snapshots;
    for (std::size_t index = 0; index < state.threads.size(); ++index) {
        auto snapshot = state.threads[index]->Collect();
        auto totals = std::move(snapshot.totals);
        snapshot.slices = {};
        snapshot.totals = TraceFormat::Since(totals, state.intervalBase[index]);
        state.intervalBase[index] = std::move(totals);
        snapshots.push_back(std::move(snapshot));
    }
    auto summary = identity(width, height, device);
    summary.window = state.interval->Describe();
    summary.firstPresent = state.intervalFirstPresent;
    summary.gpuBusyMs = VulkanDevice::PresentCounts().gpuBusyMs - state.intervalGpuBusy;
    summary.interval = std::make_pair(seconds(state.intervalPresents.front() - state.first), seconds(now - state.first));
    summaryWriter()->Write(TraceFormat::Summary(summary, snapshots, state.intervalPresents));
}

void PerformanceTrace::Present(std::uint32_t width, std::uint32_t height, const VulkanDevice* device) {
    const auto now = Clock::now();
    BindThread("presenter");
    auto& state = session();
    std::lock_guard lock(state.mutex);
    if (!state.started) {
        state.window = TraceWindow::Parse(std::getenv("APS5_TRACE_FRAMES"), std::getenv("APS5_TRACE_SECONDS"));
        state.interval = TraceInterval::Parse(std::getenv("APS5_TRACE_SUMMARY_EVERY"));
        state.first = now;
        state.started = true;
    }
    const auto present = state.present++;
    DrawTrace::PresentBoundary(present, now, false);
    const auto edge = state.window ? state.window->Present(present, now - state.first) : TraceWindow::Edge::None;
    if (edge != TraceWindow::Edge::Close) DrawTrace::FinalizeBoundary();
    if (state.interval) {
        if (!state.intervalOpen) {
            OpenInterval(now, present);
        } else {
            state.intervalPresents.push_back(now);
            if (state.interval->Due(now - state.first)) {
                FlushInterval(now, width, height, device);
                OpenInterval(now, present);
            }
        }
    }
    if (edge == TraceWindow::Edge::Open) {
        if (auto* ledger = Workload()) ledger->Open(present, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
        state.windowOpen = true;
        state.windowBase = totalsOf(state.threads);
        state.firstPresent = present;
        state.gpuBusyAtOpen = VulkanDevice::PresentCounts().gpuBusyMs;
        TraceClockSample clock;
        std::uint64_t deviation = 0;
        const bool detail = DrawTrace::Enabled();
        DrawTrace::CalibrationSample sample;
        const bool calibrated = device != nullptr && device->CalibrateClock(clock.gpuNs, clock.cpu, detail ? &deviation : nullptr, detail ? &sample : nullptr);
        if (calibrated) state.gpuClock = clock;
        if (detail) DrawTrace::Calibrate(device != nullptr ? device->TraceId() : 0, sample);
        {
            std::lock_guard gpu(gpuMutex);
            gpuRanges.clear();
            gpuLost = 0;
            gpuRanges.reserve(GpuRangeCapacity);
        }
        state.presents.push_back(now);
        presentCount.fetch_add(1, std::memory_order_relaxed);
        buffering.store(true, std::memory_order_relaxed);
        recording.store(true, std::memory_order_relaxed);
        ProfilePrint_nid_no_patch("[trace] recording %s from present %llu%s\n", state.window->Describe().c_str(), static_cast<unsigned long long>(present), state.gpuClock ? "" : "; no GPU track: the device has no calibrated timestamps");
        return;
    }
    if (!state.windowOpen) {
        if (!state.interval) return;
        presentCount.fetch_add(1, std::memory_order_relaxed);
        recording.store(true, std::memory_order_relaxed);
        return;
    }
    state.presents.push_back(now);
    if (auto* ledger = Workload()) ledger->Boundary(present, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()), edge == TraceWindow::Edge::Close);
    presentCount.fetch_add(1, std::memory_order_relaxed);
    if (edge != TraceWindow::Edge::Close) return;
    state.windowOpen = false;
    buffering.store(false, std::memory_order_relaxed);
    recording.store(state.interval.has_value(), std::memory_order_relaxed);

    std::vector<TraceThread::Snapshot> snapshots;
    std::size_t presenter = 0;
    state.windowBase.resize(state.threads.size());
    for (std::size_t index = 0; index < state.threads.size(); ++index) {
        if (state.threads[index].get() == local) presenter = index;
        snapshots.push_back(state.threads[index]->Collect());
        snapshots.back().totals = TraceFormat::Since(snapshots.back().totals, state.windowBase[index]);
    }
    std::vector<TraceThread::Slice> gpuSlices;
    TraceClockSample clock;
    std::uint64_t deviation = 0;
    const bool detail = DrawTrace::Enabled();
    DrawTrace::CalibrationSample sample;
    const bool calibrated = (state.gpuClock.has_value() || detail) && device != nullptr
        && device->CalibrateClock(clock.gpuNs, clock.cpu, detail ? &deviation : nullptr, detail ? &sample : nullptr);
    if (detail) DrawTrace::Calibrate(device != nullptr ? device->TraceId() : 0, sample);
    DrawTrace::FinalizeBoundary();
    if (state.gpuClock && calibrated) {
        std::lock_guard gpu(gpuMutex);
        gpuSlices = TraceFormat::GpuSlices(gpuRanges, *state.gpuClock, clock, state.presents.front());
        snapshots.push_back({"gpu", gpuSlices, {}, gpuLost != 0, gpuLost});
    }
    const auto process = processId();
    auto summary = identity(width, height, device);
    summary.window = state.window->Describe();
    summary.firstPresent = state.firstPresent;
    summary.gpuBusyMs = VulkanDevice::PresentCounts().gpuBusyMs - state.gpuBusyAtOpen;
    summary.trace = "aps5-trace-" + summary.commit + "-" + std::to_string(process) + ".json.gz";
    std::string workload;
    std::string workloadName;
    if (auto* ledger = Workload()) {
        const auto& run = workloadRunId();
        workloadName = "aps5-workload-" + run + "-" + std::to_string(process) + ".json.gz";
        workload = workloadJson(ledger->Collect(), summary, process, run);
    }
    auto events = TraceFormat::Events(process, state.presents.front(), snapshots, presenter, state.presents, state.firstPresent);
    auto& output = writer();
    output = std::make_unique<ProfileOutput>([directory = traceDirectory(), name = summary.trace, line = TraceFormat::Summary(summary, snapshots, state.presents), workloadName = std::move(workloadName), workload = std::move(workload)](std::string_view text) {
        try {
            std::filesystem::create_directories(directory);
            TraceFile::WriteGzip(directory / name, text);
            if (!workload.empty()) {
                const auto final = directory / workloadName;
                const auto temporary = directory / (workloadName + ".tmp");
                if (std::filesystem::exists(final) || std::filesystem::exists(temporary)) throw std::runtime_error("measurement workload output already exists");
                TraceFile::WriteGzip(temporary, workload);
                std::filesystem::rename(temporary, final);
            }
            TraceFile::AppendLine(directory / "aps5-summary.jsonl", line);
            std::fprintf(stderr, "[trace] wrote %s and its line in aps5-summary.jsonl\n", name.c_str());
            std::fflush(stderr);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[trace] writing %s failed: %s\n", name.c_str(), error.what());
            std::fflush(stderr);
            throw;
        }
    }, std::numeric_limits<std::size_t>::max());
    ProfilePrint_nid_no_patch("[trace] closed at present %llu, writing %s (%zu bytes of JSON)\n", static_cast<unsigned long long>(present), summary.trace.c_str(), events.size());
    output->Write(std::move(events));
}

void PerformanceTrace::StopOutput() {
    if (writer()) writer()->Stop();
    if (summaryWriter()) summaryWriter()->Stop();
    DrawTrace::Stop("shutdown");
    DrawTrace::Flush();
}

}

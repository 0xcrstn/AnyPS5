#include "CompileTrace.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <ctime>
#include <fstream>
#include <unistd.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace ShaderCompileTrace {

namespace {

struct Sink {
    std::mutex mutex;
    std::FILE* file = nullptr;
    std::string* testing = nullptr;
    Clock::time_point origin = Clock::now();
};

Sink& sink() {
    static auto* instance = new Sink();
    return *instance;
}

std::atomic<std::uint64_t> nextId{0};
std::atomic<bool> testingEnabled{false};

thread_local Event* innermost = nullptr;

std::FILE* openConfigured() {
    const char* value = std::getenv("APS5_COMPILE_TRACE");
    if (value == nullptr || *value == '\0' || std::string_view(value) == "0") return nullptr;
    std::string path(value);
    if (path == "1") {
#ifdef _WIN32
        path = "compile-trace-" + std::to_string(GetCurrentProcessId()) + ".jsonl";
#else
        path = "compile-trace-" + std::to_string(getpid()) + ".jsonl";
#endif
    }
    std::FILE* file = std::fopen(path.c_str(), "ab");
    if (file == nullptr) throw std::runtime_error("APS5_COMPILE_TRACE cannot open " + path);
    std::fprintf(stderr, "[compile-trace] writing %s\n", path.c_str());
    return file;
}

bool configured() {
    static const bool enabled = [] {
        auto& target = sink();
        target.file = openConfigured();
        return target.file != nullptr;
    }();
    return enabled;
}

void appendEscaped(std::string& out, std::string_view text) {
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            out += '\\';
            out += character;
        } else if (byte < 0x20u) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
            out += escaped;
        } else {
            out += character;
        }
    }
}

std::uint64_t threadNumber() {
    static std::atomic<std::uint64_t> next{0};
    thread_local const std::uint64_t number = ++next;
    return number;
}

void write(const std::string& line) {
    auto& target = sink();
    std::lock_guard lock(target.mutex);
    if (target.testing != nullptr) {
        *target.testing += line;
        return;
    }
    if (target.file == nullptr) return;
    std::fwrite(line.data(), 1, line.size(), target.file);
    std::fflush(target.file);
}

double microseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}

double mebibytes(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

}

bool Enabled() noexcept {
    if (testingEnabled.load(std::memory_order_relaxed)) return true;
    try {
        return configured();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[compile-trace] %s
", error.what());
        std::terminate();
    }
}

std::uint64_t HashBytes(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::uint64_t ThreadCpuNanoseconds() noexcept {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) == 0) return 0;
    const auto value = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime; };
    return (value(kernel) + value(user)) * 100u;
#else
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return 0;
    return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(time.tv_nsec);
#endif
}

ProcessCommit SampleProcessCommit() noexcept {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) == 0) return {};
    return {counters.PrivateUsage, counters.PeakPagefileUsage};
#else
    std::ifstream status("/proc/self/status");
    std::string key;
    ProcessCommit commit;
    while (status >> key) {
        std::uint64_t kib = 0;
        if (key == "VmRSS:" && status >> kib) commit.current = kib * 1024u;
        else if (key == "VmHWM:" && status >> kib) commit.peak = kib * 1024u;
    }
    return commit;
#endif
}

std::string Render(std::string_view phase, std::uint64_t id, std::uint64_t parent, std::uint64_t thread, double startUs, double queueUs, bool queued, double wallUs, double cpuUs, const ProcessCommit& before, const ProcessCommit& after, bool failed, std::string_view fields) {
    std::string line;
    line.reserve(256 + fields.size());
    char number[512];
    line += "{\"phase\":\"";
    appendEscaped(line, phase);
    std::snprintf(number, sizeof(number), "\",\"id\":%llu,\"parent\":%llu,\"thread\":%llu,\"start_us\":%.1f", static_cast<unsigned long long>(id), static_cast<unsigned long long>(parent), static_cast<unsigned long long>(thread), startUs);
    line += number;
    if (queued) {
        std::snprintf(number, sizeof(number), ",\"queue_us\":%.1f", queueUs);
        line += number;
    }
    const double peakRise = after.peak > before.peak ? mebibytes(after.peak - before.peak) : 0.0;
    const double commitDelta = after.current >= before.current ? mebibytes(after.current - before.current) : -mebibytes(before.current - after.current);
    std::snprintf(number, sizeof(number), ",\"wall_us\":%.1f,\"cpu_us\":%.1f,\"commit_mib\":%.1f,\"commit_delta_mib\":%.1f,\"process_peak_rise_mib\":%.1f", wallUs, cpuUs, mebibytes(after.current), commitDelta, peakRise);
    line += number;
    if (failed) line += ",\"failed\":true";
    line += fields;
    line += "}\n";
    return line;
}

void SetSinkForTesting(std::string* target) noexcept {
    auto& destination = sink();
    std::lock_guard lock(destination.mutex);
    destination.testing = target;
    testingEnabled.store(target != nullptr, std::memory_order_relaxed);
}

Event::Event(std::string_view name) noexcept {
    if (!Enabled()) return;
    try {
        phase.assign(name);
    } catch (...) {
        return;
    }
    active = true;
    exceptions = std::uncaught_exceptions();
    id = ++nextId;
    outer = innermost;
    parent = outer != nullptr ? outer->id : 0;
    innermost = this;
    commitStart = SampleProcessCommit();
    cpuStart = ThreadCpuNanoseconds();
    start = Clock::now();
}

Event::~Event() {
    if (!active) return;
    const auto end = Clock::now();
    const auto cpuEnd = ThreadCpuNanoseconds();
    const auto commitEnd = SampleProcessCommit();
    innermost = outer;
    const bool unwinding = std::uncaught_exceptions() > exceptions;
    try {
        const auto& target = sink();
        const auto line = Render(phase, id, parent, threadNumber(), microseconds(start - target.origin), queued ? microseconds(start - enqueued) : 0.0, queued, microseconds(end - start), static_cast<double>(cpuEnd - cpuStart) / 1000.0, commitStart, commitEnd, failed || unwinding, fields);
        write(line);
    } catch (...) {
    }
}

Event& Event::Queued(Clock::time_point time) noexcept {
    if (!active) return *this;
    enqueued = time;
    queued = true;
    return *this;
}

void Event::field(std::string_view name) {
    fields += ",\"";
    appendEscaped(fields, name);
    fields += "\":";
}

Event& Event::Hex(std::string_view name, std::uint64_t value) {
    if (!active) return *this;
    char text[24];
    std::snprintf(text, sizeof(text), "\"%016llx\"", static_cast<unsigned long long>(value));
    field(name);
    fields += text;
    return *this;
}

Event& Event::Number(std::string_view name, std::uint64_t value) {
    if (!active) return *this;
    field(name);
    fields += std::to_string(value);
    return *this;
}

Event& Event::Real(std::string_view name, double value) {
    if (!active) return *this;
    char text[40];
    std::snprintf(text, sizeof(text), "%.3f", value);
    field(name);
    fields += text;
    return *this;
}

Event& Event::Text(std::string_view name, std::string_view value) {
    if (!active) return *this;
    field(name);
    fields += '"';
    appendEscaped(fields, value);
    fields += '"';
    return *this;
}

Event& Event::Flag(std::string_view name, bool value) {
    if (!active) return *this;
    field(name);
    fields += value ? "true" : "false";
    return *this;
}

void Event::Fail(std::string_view reason) {
    if (!active) return;
    failed = true;
    Text("failure", reason);
}

}

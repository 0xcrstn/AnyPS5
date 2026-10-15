#ifndef CORE_SHADER_RECOMPILER_COMPILETRACE_HPP
#define CORE_SHADER_RECOMPILER_COMPILETRACE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ShaderCompileTrace {

using Clock = std::chrono::steady_clock;

bool Enabled() noexcept;

std::uint64_t HashBytes(std::span<const std::byte> bytes) noexcept;

template<typename T>
std::uint64_t HashOf(std::span<const T> values) noexcept {
    return HashBytes(std::as_bytes(values));
}

std::uint64_t ThreadCpuNanoseconds() noexcept;

struct ProcessCommit {
    std::uint64_t current = 0;
    std::uint64_t peak = 0;
};

ProcessCommit SampleProcessCommit() noexcept;

class Event {
public:
    explicit Event(std::string_view phase) noexcept;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    ~Event();

    bool Active() const noexcept { return active; }
    std::uint64_t Id() const noexcept { return id; }
    Event& Queued(Clock::time_point enqueued) noexcept;
    Event& Hex(std::string_view name, std::uint64_t value);
    Event& Number(std::string_view name, std::uint64_t value);
    Event& Real(std::string_view name, double value);
    Event& Text(std::string_view name, std::string_view value);
    Event& Flag(std::string_view name, bool value);
    void Fail(std::string_view reason);

private:
    void field(std::string_view name);

    bool active = false;
    bool failed = false;
    int exceptions = 0;
    std::uint64_t id = 0;
    std::uint64_t parent = 0;
    Event* outer = nullptr;
    std::string phase;
    std::string fields;
    Clock::time_point start{};
    Clock::time_point enqueued{};
    bool queued = false;
    std::uint64_t cpuStart = 0;
    ProcessCommit commitStart{};
};

std::string Render(std::string_view phase, std::uint64_t id, std::uint64_t parent, std::uint64_t thread, double startUs, double queueUs, bool queued, double wallUs, double cpuUs, const ProcessCommit& before, const ProcessCommit& after, bool failed, std::string_view fields);

void SetSinkForTesting(std::string* sink) noexcept;

}

#endif

#include "CompileTrace.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        Require(end != std::string::npos, "trace line is not terminated");
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::string Value(const std::string& line, const std::string& name) {
    const auto key = "\"" + name + "\":";
    const auto at = line.find(key);
    if (at == std::string::npos) return {};
    const auto begin = at + key.size();
    auto end = begin;
    if (line[begin] == '"') {
        end = line.find('"', begin + 1);
        return line.substr(begin + 1, end - begin - 1);
    }
    while (end < line.size() && line[end] != ',' && line[end] != '}') ++end;
    return line.substr(begin, end - begin);
}

void Disabled() {
    ShaderCompileTrace::SetSinkForTesting(nullptr);
    ShaderCompileTrace::Event event("disabled");
    Require(!event.Active(), "an event is active without APS5_COMPILE_TRACE");
    event.Number("ignored", 1);
}

void Nesting() {
    std::string sink;
    ShaderCompileTrace::SetSinkForTesting(&sink);
    std::uint64_t outerId = 0;
    {
        ShaderCompileTrace::Event outer("outer");
        outerId = outer.Id();
        outer.Hex("key", 0xabcdu).Text("name", "a\"b\\c\n");
        {
            ShaderCompileTrace::Event inner("inner");
            inner.Number("words", 42).Flag("hit", true).Real("ratio", 0.5);
        }
    }
    ShaderCompileTrace::SetSinkForTesting(nullptr);
    const auto lines = Lines(sink);
    Require(lines.size() == 2, "nested events did not write two lines");
    Require(Value(lines[0], "phase") == "inner" && Value(lines[1], "phase") == "outer", "events are not written innermost first");
    Require(Value(lines[0], "parent") == std::to_string(outerId), "inner event does not name its parent");
    Require(Value(lines[1], "parent") == "0", "outer event has a parent");
    Require(Value(lines[0], "words") == "42" && Value(lines[0], "hit") == "true" && Value(lines[0], "ratio") == "0.500", "numeric fields are wrong");
    Require(Value(lines[1], "key") == "000000000000abcd", "hex field is wrong");
    Require(lines[1].find("\"name\":\"a\\\"b\\\\c\\u000a\"") != std::string::npos, "text field is not escaped");
    Require(!Value(lines[0], "wall_us").empty() && !Value(lines[0], "cpu_us").empty() && !Value(lines[0], "commit_mib").empty(), "timing or memory fields are missing");
    Require(Value(lines[0], "queue_us").empty(), "an unqueued event reports a queue delay");
}

void QueueDelay() {
    std::string sink;
    ShaderCompileTrace::SetSinkForTesting(&sink);
    {
        const auto enqueued = ShaderCompileTrace::Clock::now() - std::chrono::milliseconds(20);
        ShaderCompileTrace::Event event("queued");
        event.Queued(enqueued);
    }
    ShaderCompileTrace::SetSinkForTesting(nullptr);
    const auto lines = Lines(sink);
    Require(lines.size() == 1, "queued event was not written");
    Require(std::stod(Value(lines[0], "queue_us")) >= 20000.0, "queue delay is shorter than the wait before the start");
}

void Failure() {
    std::string sink;
    ShaderCompileTrace::SetSinkForTesting(&sink);
    try {
        ShaderCompileTrace::Event event("throws");
        throw std::runtime_error("expected");
    } catch (const std::runtime_error&) {
    }
    {
        ShaderCompileTrace::Event event("reported");
        event.Fail("driver error");
    }
    ShaderCompileTrace::SetSinkForTesting(nullptr);
    const auto lines = Lines(sink);
    Require(lines.size() == 2, "failed events were not written");
    Require(Value(lines[0], "failed") == "true", "an event left by an exception is not marked failed");
    Require(Value(lines[1], "failed") == "true" && Value(lines[1], "failure") == "driver error", "an explicit failure is not recorded");
}

void CpuTime() {
    const auto before = ShaderCompileTrace::ThreadCpuNanoseconds();
    volatile std::uint64_t sum = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (std::chrono::steady_clock::now() < end) sum = sum + 1;
    const auto after = ShaderCompileTrace::ThreadCpuNanoseconds();
    Require(after > before, "thread CPU time does not advance while the thread computes");
    const auto idleBefore = ShaderCompileTrace::ThreadCpuNanoseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto idle = ShaderCompileTrace::ThreadCpuNanoseconds() - idleBefore;
    Require(idle < 50'000'000u, "thread CPU time advances while the thread sleeps");
}

}

int main() {
    try {
        Disabled();
        Nesting();
        QueueDelay();
        Failure();
        CpuTime();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "compile trace test failed: %s\n", error.what());
        return 1;
    }
    std::puts("compile trace tests passed");
    return 0;
}

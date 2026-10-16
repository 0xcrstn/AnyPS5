#include "prx/libSceAgcDriver/Execution/include/DrawTrace.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace {
using namespace AgcDriver::DrawTrace;
using namespace std::chrono_literals;
Clock::time_point fixtureTime;
std::uint64_t cpuTicks = 100;
std::uint64_t cycleTicks = std::uint64_t{1} << 60;
constexpr std::uint64_t gpuBase = std::uint64_t{1} << 60;

Clock::time_point Time() { return fixtureTime; }

CpuSample Cpu() {
#ifdef _WIN32
    const auto native = static_cast<std::uint64_t>(GetCurrentThreadId());
#else
    const auto native = static_cast<std::uint64_t>(syscall(SYS_gettid));
#endif
    return {native, cpuTicks, 1000000000, CpuStatus::Measured};
}

#ifdef _WIN32
CycleSample Cycles() {
    return {static_cast<std::uint64_t>(GetCurrentThreadId()), cycleTicks, CpuStatus::Measured};
}
#endif

CalibrationSample Calibration(std::uint64_t gpu) {
    CalibrationSample sample;
    sample.gpuTicks = gpu;
    sample.before = Clock::now();
#ifdef _WIN32
    LARGE_INTEGER counter{}, frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency)) throw std::runtime_error("synthetic calibration host clock unavailable");
    sample.hostTicks = static_cast<std::uint64_t>(counter.QuadPart);
    sample.hostFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
    sample.hostDomain = "query_performance_counter";
#else
    timespec counter{};
    if (clock_gettime(CLOCK_MONOTONIC, &counter) != 0) throw std::runtime_error("synthetic calibration host clock unavailable");
    sample.hostTicks = static_cast<std::uint64_t>(counter.tv_sec) * 1000000000 + static_cast<std::uint64_t>(counter.tv_nsec);
    sample.hostFrequency = 1000000000;
    sample.hostDomain = "clock_monotonic";
#endif
    sample.conversionHostTicks = sample.hostTicks;
    sample.after = Clock::now();
    sample.deviationNs = 17;
    sample.periodNs = 2.5;
    sample.validBits = 64;
    sample.available = true;
    return sample;
}

void Write(const std::filesystem::path& path, const std::string& bytes) {
    if (std::filesystem::exists(path)) throw std::runtime_error("fixture output already exists");
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << bytes;
}

void Produce(const std::filesystem::path& root) {
    Options options;
    options.runId = "12345678-1234-4234-8234-123456789abc";
    options.captureId = "22222222-2222-4222-8222-222222222222";
    options.revision = "aaaaaaaaaaaa";
    options.dirty = false;
    options.process = 7;
    options.start = 10;
    options.length = 2;
    options.recordsPerThread = 128;
    options.maximumThreads = 2;
    options.maximumMetadata = 16;
    options.maximumQueries = 4;
    options.maximumEvents = 128;
    options.maximumBytes = 4 * 1024 * 1024;
    options.threadCpu = true;
    options.sampleCpu = Cpu;
    options.threadCycles = true;
#ifdef _WIN32
    options.sampleCycles = Cycles;
#endif
    options.journal = true;
    options.maximumJournalBytes = 1024 * 1024;
    Session session(options);
    fixtureTime = Clock::now();
    const auto opening = fixtureTime;
    SessionScope binding(session, Time);
    BindThread("native contract fixture");
    session.Boundary(10, Time());
    const auto device = NextId();
    const auto recorder = NextId();
    DeclareIdentity(device, "device");
    DeclareIdentity(recorder, "recorder");
    const auto batch = DeclareBatch(device, recorder, 1);
    Context context;
    context.device = device;
    context.batch = batch;
    ContextScope contextBinding(context);
    Calibrate(device, Calibration(gpuBase));
    const auto guest = RegisterMetadata("guest_shader", std::string(64, 'a'), {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.guest-shader.v1"}, {"stage", "vertex"}});
    const auto compiler = RegisterMetadata("compiler_key", std::string(64, 'b'), {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.recompile-key.v1"}});
    const auto spirv = RegisterMetadata("spirv", std::string(64, 'c'), {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.final-spirv.v1"}});
    const auto variant = RegisterMetadata("shader_variant", std::string(64, 'd'), {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.shader-variant.v1"}, {"stage", "vertex"}}, {guest, compiler, spirv});
    const auto pipeline = RegisterMetadata("pipeline", std::string(64, 'e'), {{"digest_algorithm", "sha256"}, {"encoding", "anyps5.graphics-pipeline.v1"}}, {variant});
    RecordPresent(NextId(), 0, 10, Status::Complete);
    fixtureTime = opening + 2ms;
    {
        Operation operation("draw");
        AssociateMetadata(operation.GetId(), pipeline, "pipeline");
        Span span("Fixture.Draw");
        const auto command = NextId();
        const auto query = RegisterGpuQuery(batch, 0, operation.GetId(), "draw", false);
        if (query == 0) throw std::runtime_error("synthetic query was refused");
        Link(query, command, "measures");
        RecordHostCommand(command, operation.GetId(), "vkCmdDraw", 0, 3, false, Status::Unknown);
        RecordCounter("copy_cpu_bytes", 64, "bytes", operation.GetId());
        RecordHardwareCounter(command, "input_assembly_vertices", QueryStatus::Pending, 0, "vertices");
        fixtureTime = opening + 4ms;
        cpuTicks += 300;
        cycleTicks += 63;
        RecordHostCommand(command, operation.GetId(), "vkCmdDraw", 0, 3, false, Status::Complete);
        ResolveGpuQueryTicks(query, QueryStatus::Available, gpuBase + 12, gpuBase + 32, 2.5);
        RecordHardwareCounter(command, "input_assembly_vertices", QueryStatus::Available, 3, "vertices");
    }
    Calibrate(device, Calibration(gpuBase + 100));
    std::ostringstream journal;
    session.WriteJournal(journal);
    std::filesystem::create_directory(root / "prefix");
    const auto journalName = "aps5-draw-" + options.runId + "-7.journal.jsonl";
    Write(root / "prefix" / journalName, journal.str());
    fixtureTime = opening + 10ms;
    RecordPresent(NextId(), 0, 11, Status::Complete);
    session.Boundary(11, Time());
    fixtureTime = opening + 20ms;
    session.Boundary(12, Time());
    if (!session.Ready(Time())) throw std::runtime_error("synthetic session did not drain");
    session.Freeze(Time());
    for (int pass = 0; pass < 8 && !session.WriteJournal(journal); ++pass) {}
    if (!session.JournalFinished()) throw std::runtime_error("synthetic journal did not finish");
    Write(root / journalName, journal.str());
    const auto sidecar = root / ("aps5-draw-" + options.runId + "-7.json");
    if (std::filesystem::exists(sidecar)) throw std::runtime_error("fixture sidecar already exists");
    std::ofstream output(sidecar, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    session.WriteJson(output);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2 || !std::filesystem::is_directory(argv[1])) throw std::runtime_error("expected existing exclusive synthetic output directory");
        Produce(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "prx/libSceAgcDriver/Execution/include/PipelineCompileQueue.hpp"
#include "prx/libSceAgcDriver/Execution/include/PipelineSingleflight.hpp"
#include "prx/libSceAgcDriver/Execution/include/ComputePipelineAheadRequest.hpp"
#include "prx/libSceAgcDriver/Execution/include/ComputePipelineAheadPackets.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <semaphore>
#include <string_view>

namespace {

using namespace AgcDriver;
using namespace std::chrono_literals;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Gate {
    std::binary_semaphore entered{0}, release{0};
    std::atomic<bool> opened{false};
    ~Gate() { Open(); }
    void Open() { if (!opened.exchange(true)) release.release(); }
    void Block() {
        entered.release();
        Require(release.try_acquire_for(4s), "pipeline test gate timed out");
    }
    void Ready() { Require(entered.try_acquire_for(2s), "pipeline test worker did not start"); }
};

template<typename TPredicate>
void Await(TPredicate predicate, const char* message) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < end, message);
        std::this_thread::yield();
    }
}

void QueueBoundsAndDemand() {
    Gate gate;
    std::atomic<unsigned> prepared{0}, destroyed{0}, unexpected{0};
    PipelineCompileQueue queue(1, 2, 10);
    struct Cleanup {
        Gate& gate;
        PipelineCompileQueue& queue;
        ~Cleanup() { gate.Open(); queue.Cancel(); queue.Wait(); }
    } cleanup{gate, queue};
    Require(queue.TryEnqueue({1, 0}, 6, [&] { gate.Block(); return true; }), "first compile was refused");
    gate.Ready();
    Require(!queue.TryEnqueuePrepared({3, 0}, 5, [&] { ++prepared; return [] { return true; }; }), "retained-byte cap was exceeded below the task cap");
    auto owner = std::shared_ptr<int>(new int(7), [&](int* value) {
        static_cast<void>(queue.Counters());
        ++destroyed;
        delete value;
    });
    Require(queue.TryEnqueue({2, 0}, 4, [owner, &unexpected] { ++unexpected; return true; }), "second compile was refused");
    owner.reset();
    Require(!queue.TryEnqueuePrepared({3, 0}, 0, [&] { ++prepared; return [] { return true; }; }), "task cap was exceeded below the byte cap");
    Require(!queue.TryEnqueuePrepared({1, 0}, 0, [&] { ++prepared; return [] { return true; }; }), "pending identity was duplicated");
    Require(prepared == 0, "refused work copied its payload");
    Require(queue.Demand({2, 0}) && destroyed == 1, "foreground demand retained queued ownership");
    PipelineSingleflight<PipelineCompileKey, int> cache;
    const auto demand = cache.GetOrBuild({2, 0}, [] { return std::make_shared<int>(19); });
    Require(demand.created && *demand.value == 19 && !gate.opened, "queued demand waited behind an unrelated compile");
    auto cancelled = std::shared_ptr<int>(new int(8), [&](int* value) {
        static_cast<void>(queue.Counters());
        ++destroyed;
        delete value;
    });
    Require(queue.TryEnqueue({3, 0}, 4, [cancelled, &unexpected] { ++unexpected; return true; }), "released budget was not reusable");
    cancelled.reset();
    queue.Cancel();
    Require(destroyed == 2 && unexpected == 0, "cancellation ran or retained queued work");
    Require(!queue.TryEnqueuePrepared({4, 0}, 0, [&] { ++prepared; return [] { return true; }; }) && prepared == 0, "closed queue copied or accepted work");
    gate.Open();
    queue.Wait();
    queue.Drain();
    const auto counts = queue.Counters();
    Require(counts.accepted == 3 && counts.built == 1 && counts.demanded == 1 && counts.cancelled == 1 && counts.refused == 3 && counts.duplicate == 1, "queue outcomes were lost");
    Require(counts.outstanding == 0 && counts.retainedBytes == 0 && counts.peakBytes == 10, "queue retained budget after completion");
}

void QueueFailureRetry() {
    PipelineCompileQueue queue(2, 4, 40);
    bool rejected = false;
    try {
        queue.TryEnqueuePrepared({1, 0}, 10, []() -> std::function<bool()> { throw std::runtime_error("injected capture failure"); });
    } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected && queue.Counters().outstanding == 0 && queue.Counters().retainedBytes == 0, "failed capture retained admission ownership");
    Require(queue.TryEnqueue({1, 0}, 10, []() -> bool { throw std::runtime_error("injected compile failure"); }), "failure task was refused");
    queue.Drain();
    Require(queue.TryEnqueue({1, 0}, 10, [] { return true; }), "failed identity could not retry");
    Require(queue.TryEnqueue({2, 0}, 10, [] { return false; }), "present task was refused");
    queue.Drain();
    queue.Cancel();
    queue.Wait();
    const auto counts = queue.Counters();
    Require(counts.failed == 1 && counts.built == 1 && counts.present == 1 && counts.outstanding == 0, "failure stopped a worker or poisoned its identity");
}

void Singleflight() {
    using Store = PipelineSingleflight<PipelineCompileKey, int>;
    Store cache;
    Gate gate, failing;
    std::atomic<unsigned> builds{0};
    std::future<Store::Result> first, second, failed, joined;
    try {
        first = std::async(std::launch::async, [&] {
            return cache.GetOrBuild({1, 0}, [&] { ++builds; gate.Block(); return std::make_shared<int>(41); });
        });
        gate.Ready();
        second = std::async(std::launch::async, [&] {
            return cache.GetOrBuild({1, 0}, [&] { ++builds; return std::make_shared<int>(99); });
        });
        Await([&] { return cache.Joins() == 1; }, "matching foreground demand did not join the running compile");
        const auto other = cache.GetOrBuild({2, 0}, [] { return std::make_shared<int>(17); });
        Require(other.created && !gate.opened, "different identity waited on the compile-store lock");
        gate.Open();
        const auto a = first.get(), b = second.get();
        Require(a.created && b.joined && a.value == b.value && builds == 1, "matching identity compiled more than once");
        const auto high = cache.GetOrBuild({0x8000000000000001ull, 0}, [] { return std::make_shared<int>(23); });
        const auto push = cache.GetOrBuild({1, 32}, [] { return std::make_shared<int>(29); });
        Require(high.created && push.created && high.value != a.value && push.value != a.value, "runtime identity lost high bits or push-stage use");
        failed = std::async(std::launch::async, [&] {
            return cache.GetOrBuild({4, 0}, [&]() -> std::shared_ptr<int> { failing.Block(); throw std::runtime_error("injected build failure"); });
        });
        failing.Ready();
        joined = std::async(std::launch::async, [&] { return cache.GetOrBuild({4, 0}, [] { return std::make_shared<int>(1); }); });
        Await([&] { return cache.Joins() == 2; }, "failed compile had no matching waiter");
        failing.Open();
        unsigned failures = 0;
        try { static_cast<void>(failed.get()); } catch (const std::runtime_error&) { ++failures; }
        try { static_cast<void>(joined.get()); } catch (const std::runtime_error&) { ++failures; }
        Require(failures == 2 && cache.Find({4, 0}) == nullptr, "failed compile published objects or lost an exception");
        Require(cache.GetOrBuild({4, 0}, [] { return std::make_shared<int>(43); }).created, "singleflight failure poisoned future demand");
        cache.Clear();
        Require(cache.Find({1, 0}) == nullptr && *a.value == 41, "cache clear invalidated a retained pipeline owner");
    } catch (...) {
        gate.Open();
        failing.Open();
        throw;
    }
}

void RequestOwnership() {
    ShaderRecompiler::RecompileResult source;
    auto owner = std::make_shared<std::vector<std::uint32_t>>(std::initializer_list<std::uint32_t>{0x07230203u, 1, 2, 3, 4});
    std::weak_ptr<std::vector<std::uint32_t>> weak = owner;
    source.spirv = ShaderRecompiler::SharedSpirv(owner, *owner);
    source.variantId = 17;
    source.specializationId = 31;
    source.specialization = {{7, 0x12345678u}, {9, 0xabcdef01u}};
    source.hostSubgroupSize = 32;
    source.workgroupMemoryDwords = 256;
    source.runtimeAbiVersion = 5;
    source.bdaAbiVersion = 6;
    source.pushConstants = {std::byte{0x42}};
    ShaderRecompiler::DescriptorBinding binding{};
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    binding.descriptorSet = 0;
    binding.binding = 7;
    binding.count = 2;
    binding.guestDescriptor = {11, 12, 13, 14};
    source.bindings.push_back(binding);
    const auto budget = ComputePipelineAheadRequest::EstimateBytes(source);
    const auto request = ComputePipelineAheadRequest::Capture(source, budget);
    Require(request != nullptr && request->retainedBytes <= budget, "request exceeded its admitted byte estimate");
    Require(ComputePipelineAheadRequest::Capture(source, 4) == nullptr, "oversized module request was accepted");
    const auto spilled = source.spirv.Spill({}, true);
    source.spirv = spilled;
    const auto fromSpill = ComputePipelineAheadRequest::Capture(source, budget);
    Require(fromSpill != nullptr && !fromSpill->shader.spirv.Spilled(), "request retained a lazy spilled module");
    (*owner)[4] = 99;
    source.specialization.front().value = 0;
    source.bindings.front().binding = 0;
    source.pushConstants.clear();
    spilled.Unpin();
    source = {};
    owner.reset();
    Require(weak.expired(), "request retained mapped source ownership");
    const auto& shader = request->shader;
    Require(shader.spirv.Words()[4] == 4 && fromSpill->shader.spirv.Words()[4] == 4, "source mutation changed request SPIR-V");
    Require(shader.PipelineVariantId() == 31 && shader.variantId == 17 && shader.specialization.size() == 2 && shader.specialization[0].id == 7 && shader.specialization[0].value == 0x12345678u && shader.specialization[1].id == 9 && shader.specialization[1].value == 0xabcdef01u, "request lost specialization identity or values");
    Require(request->pushes && shader.pushConstants.empty() && shader.hostSubgroupSize == 32 && shader.workgroupMemoryDwords == 256 && shader.runtimeAbiVersion == 5 && shader.bdaAbiVersion == 6, "request lost pipeline configuration or retained dynamic push bytes");
    const auto& layout = shader.bindings.front();
    Require(layout.kind == binding.kind && layout.role == binding.role && layout.descriptorSet == 0 && layout.binding == 7 && layout.count == 2 && layout.guestDescriptor.empty(), "request changed descriptor layout or retained dynamic descriptors");
}

void PacketBoundaries() {
    for (const auto opcode : {0x10u, 0x15u, 0x69u, 0x76u, 0x79u, 0x7au}) {
        const auto header = 0xc0000000u | (opcode << 8u);
        Require(ComputePipelineAheadPacket(header), "inline compute state ended lookahead");
        Require(!ComputePipelineAheadPacket(header | 1u), "predicated state entered lookahead");
        Require(!ComputePipelineAheadPacket(opcode << 8u) && !ComputePipelineAheadPacket(0x40000000u | (opcode << 8u)), "non-type3 state entered lookahead");
    }
    for (const auto opcode : {0x16u, 0x22u, 0x27u, 0x2du, 0x37u, 0x3cu, 0x3fu, 0x40u, 0x42u, 0x46u, 0x49u, 0x58u, 0x63u, 0x64u, 0x83u, 0x93u, 0xffu}) Require(!ComputePipelineAheadPacket(0xc0000000u | (opcode << 8u)), "memory or control packet crossed the queue0 hint boundary");
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected --queue or --request");
        const std::string_view mode(argv[1]);
        if (mode == "--queue") {
            QueueBoundsAndDemand();
            QueueFailureRetry();
            Singleflight();
        } else if (mode == "--request") {
            RequestOwnership();
            PacketBoundaries();
        } else throw std::runtime_error("unknown pipeline compile test mode");
        std::puts("pipeline compile tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

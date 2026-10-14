#include "DrawSnapshotArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawSnapshotArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

using namespace AgcDriver::Graphics;
namespace GuestMemory = AgcDriver::GuestMemory;
using Snapshot = ShaderResources::DrawBindings::Snapshot;
using Statistics = ShaderResources::DrawBindingsStatistics;

void* AllocateWatchedBlock(std::size_t bytes, std::size_t alignment) {
    if (!GuestMemory::WriteWatched()) return nullptr;
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, alignment);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    if (!GuestMemory::Watched(reinterpret_cast<std::uint64_t>(block), bytes)) throw std::runtime_error("the watched block is not watched");
    return block;
}

void ReleaseWatchedBlock(void* block, std::size_t bytes) {
#ifdef _WIN32
    GuestArena::GuestArenaReset_nid_postfix(block, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
    munmap(block, bytes);
    GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
}

class Settings {
public:
    Settings() : enabled(DrawSnapshotArena::Enabled()), chunk(DrawSnapshotArena::ChunkBytes()), budget(DrawSnapshotArena::BudgetBytes()), extend(DrawSnapshotArena::ExtendBytes()), verify(DrawSnapshotArena::Verify()), twins(DrawSnapshotArena::DeviceTwins()) {}
    ~Settings() {
        DrawSnapshotArena::SetDeviceTwins(twins);
        DrawSnapshotArena::SetEnabled(enabled);
        DrawSnapshotArena::SetChunkBytes(chunk);
        DrawSnapshotArena::SetBudgetBytes(budget);
        DrawSnapshotArena::SetExtendBytes(extend);
        DrawSnapshotArena::SetVerify(verify);
    }
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

private:
    bool enabled;
    std::size_t chunk;
    std::size_t budget;
    std::size_t extend;
    bool verify;
    bool twins;
};

class Fixture {
public:
    Fixture(const Context& base, std::size_t bytes) : context(base), bytes(bytes) {
        if (context.hostImportAlignment == 0) return;
        block = AllocateWatchedBlock(bytes, 65536);
        if (block == nullptr) return;
        std::memset(block, 0, bytes);
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(block, bytes, true, true, true);
        }
        registered = true;
        spacer = AllocateWatchedBlock(OwnBytes, 65536);
        if (spacer == nullptr) return;
        own = AllocateWatchedBlock(OwnBytes, 65536);
        if (own == nullptr) return;
        std::memset(own, 0, OwnBytes);
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(own, OwnBytes, true, true, true);
        }
        ownRegistered = true;
        const bool imported = HostImportFor(context, OwnAddress(), OwnBytes) != nullptr;
        const bool watched = GuestMemory::Watched(Address(), bytes);
        if (!imported || !watched) {
            std::cout << "draw snapshot arena fixture: own block " << (imported ? "imported" : "not imported") << ", data block " << (watched ? "watched" : "not watched") << '\n';
            return;
        }
        cache = std::make_unique<DescriptorCache>(context);
        context.descriptorCache = cache.get();
        recorder = std::make_unique<Recorder>(context);
        recorder->Activate();
        const auto element = OwnAddress();
        ShaderRecompiler::DescriptorBinding binding;
        binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
        binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
        binding.descriptorSet = 0;
        binding.binding = 0;
        binding.count = 1;
        binding.guestDescriptor = {static_cast<std::uint32_t>(element), static_cast<std::uint32_t>(element >> 32u) & 0xffffu, 1024, 0x31000000u};
        binding.bufferWritten = {false};
        program.bindings.push_back(binding);
        compute = std::make_unique<CompiledShader>(CompiledShader{ShaderRecompiler::ShaderStage::Compute, &program, 0});
        resources = std::make_unique<ShaderResources>(context, *compute);
    }

    ~Fixture() {
        resources.reset();
        if (recorder != nullptr) recorder->Sync();
        recorder.reset();
        cache.reset();
        if (registered) {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
        }
        if (ownRegistered) {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(own);
            }
            HostImportFor(context, OwnAddress(), OwnBytes);
        }
        if (own != nullptr) ReleaseWatchedBlock(own, OwnBytes);
        if (block != nullptr) ReleaseWatchedBlock(block, bytes);
        if (spacer != nullptr) ReleaseWatchedBlock(spacer, OwnBytes);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    bool Ready() const { return resources != nullptr; }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(block); }
    std::uint64_t OwnAddress() const { return reinterpret_cast<std::uint64_t>(own); }
    std::byte* Bytes(std::uint64_t offset) const { return static_cast<std::byte*>(block) + offset; }
    Recorder& GetRecorder() { return *recorder; }
    const Context& GetContext() const { return context; }

    Snapshot Take(std::uint64_t offset, std::size_t size, Statistics* statistics = nullptr) {
        const std::vector<ShaderResources::MovedBuffer> moved{{0, Address() + offset, size, {}}};
        auto bindings = resources->PrepareDrawBindings(*recorder, moved, statistics);
        Require(bindings != nullptr && bindings->snapshots.size() == 1, "a moved read-only draw input was not snapshotted");
        held.push_back(bindings);
        return bindings->snapshots.front();
    }

    std::shared_ptr<ShaderResources::DrawBindings> TakeOwn() {
        auto bindings = resources->PrepareDrawBindings(*recorder);
        if (bindings != nullptr) held.push_back(bindings);
        return bindings;
    }

    void Release() { held.clear(); }

private:
    Context context;
    std::size_t bytes;
    static constexpr std::size_t OwnBytes = 65536;
    void* block = nullptr;
    bool registered = false;
    void* own = nullptr;
    void* spacer = nullptr;
    bool ownRegistered = false;
    std::unique_ptr<DescriptorCache> cache;
    std::unique_ptr<Recorder> recorder;
    ShaderRecompiler::RecompileResult program;
    std::unique_ptr<CompiledShader> compute;
    std::unique_ptr<ShaderResources> resources;
    std::vector<std::shared_ptr<ShaderResources::DrawBindings>> held;
};

bool Holds(const Snapshot& snapshot, std::byte value) {
    return std::all_of(snapshot.bytes.begin(), snapshot.bytes.end(), [&](std::byte byte) { return byte == value; });
}

bool SameBytes(const Snapshot& snapshot, const std::byte* guest) {
    return std::memcmp(snapshot.bytes.data(), guest, snapshot.bytes.size()) == 0;
}

void ContentTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    DrawSnapshotArena::SetExtendBytes(0);
    auto& arena = fixture.GetRecorder().SnapshotArena();
    static_cast<void>(arena.TakeCounters());
    std::memset(fixture.Bytes(0), 0x11, 65536);
    GuestMemory::BumpCollectEpoch();
    Statistics first{};
    const auto a = fixture.Take(0, 1024, &first);
    Require(a.buffer == nullptr && a.handle != VK_NULL_HANDLE, "an enabled arena did not serve the draw input");
    Require(first.arenaCopies == 1 && first.copied == 1 && a.bytes.size() == 1024 && Holds(a, std::byte{0x11}), "the first arena snapshot does not hold the guest bytes");
    Require(a.offset % fixture.GetContext().limits.minStorageBufferOffsetAlignment == 0, "an arena view breaks the storage buffer offset alignment");
    Statistics second{};
    const auto b = fixture.Take(1024, 1024, &second);
    Require(second.arenaHits == 1 && second.copied == 0 && b.handle == a.handle && b.offset == a.offset + 1024 && Holds(b, std::byte{0x11}), "a range inside a copied page was copied again in the same epoch");
    GuestMemory::BumpCollectEpoch();
    Statistics third{};
    const auto c = fixture.Take(0, 1024, &third);
    Require(third.arenaHits == 1 && c.handle == a.handle && c.offset == a.offset, "an unwritten range was copied again in a new epoch");
    std::memset(fixture.Bytes(512), 0x22, 1024);
    GuestMemory::BumpCollectEpoch();
    Statistics fourth{};
    const auto d = fixture.Take(0, 2048, &fourth);
    Require(fourth.arenaCopies == 1 && SameBytes(d, fixture.Bytes(0)), "a CPU store did not make the arena copy again");
    Require(Holds(a, std::byte{0x11}), "a new arena copy overwrote the bytes an earlier draw binds");
    GuestMemory::BumpCollectEpoch();
    Statistics fifth{};
    const auto e = fixture.Take(1024, 1024, &fifth);
    Require(fifth.arenaHits == 1 && e.handle == d.handle && e.offset == d.offset + 1024 && SameBytes(e, fixture.Bytes(1024)), "the recopied page did not serve a later range");
    std::memset(fixture.Bytes(1024), 0x33, 16);
    GuestMemory::MarkWritten(fixture.Address() + 1024, 16);
    Statistics sixth{};
    const auto f = fixture.Take(1024, 1024, &sixth);
    Require(sixth.arenaCopies == 1 && SameBytes(f, fixture.Bytes(1024)), "a driver store did not make the arena copy again");
    {
        GuestAllocations::Mutation mutation;
    }
    Statistics seventh{};
    const auto g = fixture.Take(1024, 1024, &seventh);
    Require(seventh.arenaCopies == 1 && SameBytes(g, fixture.Bytes(1024)), "an arena copy outlived a registry mutation");
    std::memset(fixture.Bytes(8192), 0x44, 4096);
    GuestMemory::BumpCollectEpoch();
    Statistics eighth{};
    const auto h = fixture.Take(8192 + 4, 64, &eighth);
    Require(eighth.arenaCopies == 1 && h.offset % fixture.GetContext().limits.minStorageBufferOffsetAlignment == 0 && Holds(h, std::byte{0x44}), "a misaligned arena view is not aligned or holds the wrong bytes");
    Statistics ninth{};
    const auto i = fixture.Take(8192 + 4 + 256, 64, &ninth);
    Require(ninth.arenaHits == 1 && i.handle == h.handle && i.offset == h.offset + 256, "a range at the same alignment phase inside a misaligned segment was not served");
    Statistics tenth{};
    const auto j = fixture.Take(8192, 64, &tenth);
    Require(Holds(j, std::byte{0x44}) && j.offset % fixture.GetContext().limits.minStorageBufferOffsetAlignment == 0, "a range off the segment's alignment phase was bound misaligned");
    fixture.Release();
    fixture.GetRecorder().Sync();
}

void PendingWriteTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    GuestMemory::BumpCollectEpoch();
    const auto own = fixture.TakeOwn();
    Require(own != nullptr && own->snapshots.size() == 1 && own->snapshots.front().buffer == nullptr && own->snapshots.front().bytes.size() == 1024, "the template's own input was not served by the arena");
    fixture.GetRecorder().NotePendingWrite(fixture.OwnAddress(), 1024);
    Require(fixture.TakeOwn() == nullptr, "a GPU-produced input was replaced with stale CPU memory by the arena");
    fixture.Release();
    fixture.GetRecorder().Sync();
}

void GpuReadTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    const auto& context = fixture.GetContext();
    auto& recorder = fixture.GetRecorder();
    std::memset(fixture.Bytes(16384), 0x66, 4096);
    GuestMemory::BumpCollectEpoch();
    const auto before = fixture.Take(16384, 1024);
    std::memset(fixture.Bytes(16384), 0x77, 1024);
    GuestMemory::BumpCollectEpoch();
    const auto after = fixture.Take(16384, 1024);
    Require(before.handle != after.handle || before.offset != after.offset, "the draw after a CPU store shares the earlier draw's arena bytes");
    std::memset(fixture.Bytes(16384), 0x78, 1024);
    auto downloaded = std::make_shared<Buffer>(context, 2048, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    VkBufferCopy region{before.offset, 0, 1024};
    copy(commands, before.handle, downloaded->Handle(), 1, &region);
    region = {after.offset, 1024, 1024};
    copy(commands, after.handle, downloaded->Handle(), 1, &region);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Keep(downloaded);
    recorder.Sync();
    const auto contents = downloaded->Bytes();
    Require(std::all_of(contents.begin(), contents.begin() + 1024, [](std::byte value) { return value == std::byte{0x66}; }), "the first draw read the input as stored after it was recorded");
    Require(std::all_of(contents.begin() + 1024, contents.end(), [](std::byte value) { return value == std::byte{0x77}; }), "the second draw read the input as stored after it was recorded");
    fixture.Release();
}

void WaitForReleases(Fixture& fixture) {
    auto& recorder = fixture.GetRecorder();
    recorder.Sync();
    recorder.FlushDeferredReleases();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (recorder.SnapshotArena().BusyChunks() != 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(recorder.SnapshotArena().BusyChunks() == 0, "completed batches still hold arena chunks");
}

void RecycleTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    DrawSnapshotArena::SetExtendBytes(0);
    auto& recorder = fixture.GetRecorder();
    recorder.Sync();
    recorder.FlushDeferredReleases();
    for (std::size_t page = 0; page < 64; ++page) std::memset(fixture.Bytes(65536 + page * 4096), static_cast<int>(page + 1), 4096);
    GuestMemory::BumpCollectEpoch();
    auto& arena = recorder.SnapshotArena();
    static_cast<void>(arena.TakeCounters());
    std::size_t served = 0;
    std::size_t fallbacks = 0;
    for (std::size_t page = 0; page < 48; ++page) {
        const auto snapshot = fixture.Take(65536 + page * 4096 + 64, 1024);
        Require(SameBytes(snapshot, fixture.Bytes(65536 + page * 4096 + 64)), "a budget-bound snapshot does not hold the guest bytes");
        if (snapshot.buffer == nullptr) ++served;
        else ++fallbacks;
    }
    const auto full = arena.TakeCounters();
    Require(served > 0 && fallbacks > 0 && full.refusedBudget == fallbacks, "an exhausted arena did not fall back to per-draw buffers");
    Require(arena.ChunkCount() <= 2, "the arena grew past its budget");
    fixture.Release();
    WaitForReleases(fixture);
    for (std::size_t page = 48; page < 64; ++page) {
        const auto snapshot = fixture.Take(65536 + page * 4096 + 64, 1024);
        Require(snapshot.buffer == nullptr && SameBytes(snapshot, fixture.Bytes(65536 + page * 4096 + 64)), "a recycled chunk did not serve new draws");
    }
    for (std::size_t page = 0; page < 48; ++page) {
        const auto snapshot = fixture.Take(65536 + page * 4096 + 64, 1024);
        Require(SameBytes(snapshot, fixture.Bytes(65536 + page * 4096 + 64)), "an entry of a recycled chunk served overwritten bytes");
    }
    const auto recycled = arena.TakeCounters();
    Require(recycled.chunksRecycled > 0, "completed chunks were not recycled");
    fixture.Release();
    recorder.Sync();
    recorder.FlushDeferredReleases();
}

void DeviceTwinTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    DrawSnapshotArena::SetDeviceTwins(true);
    const auto& context = fixture.GetContext();
    auto& recorder = fixture.GetRecorder();
    auto& arena = recorder.SnapshotArena();
    static_cast<void>(arena.TakeCounters());
    std::memset(fixture.Bytes(32768), 0x5a, 4096);
    GuestMemory::BumpCollectEpoch();
    const auto first = fixture.Take(32768, 1024);
    const auto plain = [&] {
        DrawSnapshotArena::SetDeviceTwins(false);
        const auto view = fixture.Take(32768, 1024);
        DrawSnapshotArena::SetDeviceTwins(true);
        return view;
    }();
    Require(first.handle != plain.handle && first.offset == plain.offset && Holds(first, std::byte{0x5a}), "an arena view with device twins is not served from another buffer at the chunk's offset");
    const auto download = [&](const Snapshot& snapshot, std::size_t bytes) {
        auto downloaded = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::memset(downloaded->Bytes().data(), 0, bytes);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy region{snapshot.offset, 0, bytes};
        context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, snapshot.handle, downloaded->Handle(), 1, &region);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Keep(downloaded);
        recorder.Submit();
        recorder.Sync();
        return std::vector<std::byte>(downloaded->Bytes().begin(), downloaded->Bytes().end());
    };
    const auto contents = download(first, 1024);
    Require(std::all_of(contents.begin(), contents.end(), [](std::byte value) { return value == std::byte{0x5a}; }), "a device twin of an arena chunk does not hold the copied segment");
    auto counters = arena.TakeCounters();
    Require(counters.twinViews == 1 && counters.twinCopies == 1 && counters.twinCopiedBytes >= 1024 && counters.twinsRefused == 0, "the first twin view did not copy the chunk's segment once");
    const auto again = fixture.Take(32768, 1024);
    Require(again.handle == first.handle && again.offset == first.offset, "a later batch's hit did not bind the same twin bytes");
    const auto later = download(again, 1024);
    Require(std::all_of(later.begin(), later.end(), [](std::byte value) { return value == std::byte{0x5a}; }), "a twin read by a later batch lost the segment");
    counters = arena.TakeCounters();
    Require(counters.twinViews == 1 && counters.twinCopies == 0, "a hit on a settled twin copied again");
    std::memset(fixture.Bytes(32768), 0x6b, 1024);
    GuestMemory::BumpCollectEpoch();
    const auto rewritten = fixture.Take(32768, 1024);
    const auto fresh = download(rewritten, 1024);
    Require(std::all_of(fresh.begin(), fresh.end(), [](std::byte value) { return value == std::byte{0x6b}; }), "a twin did not take a recopied segment before the batch read it");
    const auto old = download(first, 1024);
    Require(std::all_of(old.begin(), old.end(), [](std::byte value) { return value == std::byte{0x5a}; }), "a recopied segment overwrote the twin bytes an earlier view binds");
    counters = arena.TakeCounters();
    Require(counters.twinCopies == 1 && counters.twinViews == 1, "a recopied segment was not copied to the twin exactly once");
    fixture.Release();
    recorder.Sync();
    DrawSnapshotArena::SetDeviceTwins(false);
}

void DisabledTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(false);
    std::memset(fixture.Bytes(32768), 0x5a, 1024);
    GuestMemory::BumpCollectEpoch();
    const auto snapshot = fixture.Take(32768, 1024);
    Require(snapshot.buffer != nullptr && snapshot.handle == snapshot.buffer->Handle() && snapshot.offset == 0 && Holds(snapshot, std::byte{0x5a}), "a disabled arena changed the per-draw snapshot");
    fixture.Release();
    fixture.GetRecorder().Sync();
}

struct Pattern {
    const char* name;
    std::size_t draws;
    std::size_t epochEvery;
    bool storeEachEpoch;
    std::function<std::pair<std::uint64_t, std::size_t>(std::size_t)> range;
};

double MeasurePattern(Fixture& fixture, const Pattern& pattern, bool arena, Statistics& statistics, DrawSnapshotArena::Counters& counters) {
    DrawSnapshotArena::SetEnabled(arena);
    auto& recorder = fixture.GetRecorder();
    recorder.Sync();
    recorder.FlushDeferredReleases();
    static_cast<void>(recorder.SnapshotArena().TakeCounters());
    GuestMemory::BumpCollectEpoch();
    double totalUs = 0;
    for (std::size_t draw = 0; draw < pattern.draws; ++draw) {
        if (draw != 0 && draw % pattern.epochEvery == 0) {
            fixture.Release();
            recorder.Submit();
            if (pattern.storeEachEpoch) {
                const auto [offset, size] = pattern.range(draw);
                std::memset(fixture.Bytes(offset), static_cast<int>(draw & 0xff), 64);
            }
            GuestMemory::BumpCollectEpoch();
        }
        const auto [offset, size] = pattern.range(draw);
        const auto start = std::chrono::steady_clock::now();
        static_cast<void>(fixture.Take(offset, size, &statistics));
        totalUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }
    fixture.Release();
    recorder.Sync();
    recorder.FlushDeferredReleases();
    counters = recorder.SnapshotArena().TakeCounters();
    return totalUs / static_cast<double>(pattern.draws);
}

}

bool RunDrawSnapshotArenaTests(const Context& context) {
    const Settings settings;
    {
        Fixture fixture(context, std::size_t{1} << 20u);
        if (!fixture.Ready()) {
            std::cout << "host imports or write watching unavailable: draw snapshot arena not tested\n";
            return false;
        }
        ContentTests(fixture);
        PendingWriteTests(fixture);
        GpuReadTests(fixture);
        DeviceTwinTests(fixture);
        DisabledTests(fixture);
    }
    {
        DrawSnapshotArena::SetChunkBytes(65536);
        DrawSnapshotArena::SetBudgetBytes(2 * 65536);
        Fixture fixture(context, std::size_t{1} << 20u);
        Require(fixture.Ready(), "the recycle fixture lost its host import");
        RecycleTests(fixture);
    }
    std::cout << "draw snapshot arena tests passed\n";
    return true;
}

bool RunDrawSnapshotArenaBenchmark(const Context& context) {
    const Settings settings;
    constexpr std::size_t blockBytes = std::size_t{32} << 20u;
    Fixture fixture(context, blockBytes);
    if (!fixture.Ready()) {
        std::cout << "host imports or write watching unavailable: draw snapshot arena not measured\n";
        return false;
    }
    for (std::size_t offset = 0; offset < blockBytes; offset += 4096) std::memset(fixture.Bytes(offset), static_cast<int>(offset / 4096), 4096);
    std::mt19937_64 random(7);
    std::vector<std::uint64_t> scattered(4096);
    for (auto& offset : scattered) offset = (random() % ((blockBytes - (std::size_t{256} << 10u)) / 256)) * 256;
    const std::vector<Pattern> patterns{
        {"ring of 256 B constants", 4096, 64, false, [](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{(draw * 256) % (std::size_t{8} << 20u), 256}; }},
        {"ring of 4 KiB constants", 4096, 64, false, [](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{(draw * 4096) % (std::size_t{24} << 20u), 4096}; }},
        {"128 KiB windows sliding 4 KiB", 4096, 64, false, [](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{(draw % 2048) * 4096, std::size_t{128} << 10u}; }},
        {"one 128 KiB range, stored each epoch", 4096, 64, true, [](std::size_t) { return std::pair<std::uint64_t, std::size_t>{std::size_t{1} << 20u, std::size_t{128} << 10u}; }},
        {"scattered 117 KiB ranges", 4096, 64, false, [&](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{scattered[draw % scattered.size()], std::size_t{117} << 10u}; }},
    };
    for (const auto& pattern : patterns) {
        Statistics legacyStatistics{};
        Statistics arenaStatistics{};
        DrawSnapshotArena::Counters legacyCounters{};
        DrawSnapshotArena::Counters arenaCounters{};
        const auto legacyUs = MeasurePattern(fixture, pattern, false, legacyStatistics, legacyCounters);
        const auto arenaUs = MeasurePattern(fixture, pattern, true, arenaStatistics, arenaCounters);
        const auto parts = [&](const Statistics& part) {
            const auto draws = static_cast<double>(pattern.draws);
            return std::string("checks ") + std::to_string(part.checkUs / draws).substr(0, 5) + " collects " + std::to_string(part.collectUs / draws).substr(0, 5) + " lookups " + std::to_string(part.lookupUs / draws).substr(0, 5) + " buffers " + std::to_string(part.bufferUs / draws).substr(0, 5) + " copies " + std::to_string(part.copyUs / draws).substr(0, 5) + " keeps " + std::to_string(part.keepUs / draws).substr(0, 5) + " sets " + std::to_string((part.allocateUs + part.updateUs) / draws).substr(0, 5);
        };
        std::printf("[draw-arena-bench] %-38s   per-draw buffers: %s\n[draw-arena-bench] %-38s   arena:            %s\n", pattern.name, parts(legacyStatistics).c_str(), "", parts(arenaStatistics).c_str());
        std::printf("[draw-arena-bench] %-38s per draw: per-draw buffers %7.2f us (%llu copies, %.1f MiB) | arena %7.2f us (%llu hits, %llu copies, %.1f MiB copied, %llu refused)\n", pattern.name, legacyUs, static_cast<unsigned long long>(legacyStatistics.copied), static_cast<double>(legacyStatistics.copiedBytes) / 1048576.0, arenaUs, static_cast<unsigned long long>(arenaCounters.hits), static_cast<unsigned long long>(arenaCounters.copies), static_cast<double>(arenaCounters.copiedBytes) / 1048576.0, static_cast<unsigned long long>(arenaCounters.refusedBudget + arenaCounters.refusedSize));
    }
    return true;
}

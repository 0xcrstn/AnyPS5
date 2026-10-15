#include "DrawSnapshotArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DrawSnapshotArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PlaceholderImages.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
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
    Fixture(const Context& base, std::size_t bytes, std::size_t ownOffset = 0, ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Compute) : context(base), bytes(bytes) {
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
        const auto element = OwnAddress() + ownOffset;
        ShaderRecompiler::DescriptorBinding binding;
        binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
        binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
        binding.descriptorSet = 0;
        binding.binding = 0;
        binding.count = 1;
        binding.guestDescriptor = {static_cast<std::uint32_t>(element), static_cast<std::uint32_t>(element >> 32u) & 0xffffu, 1024, 0x31000000u};
        binding.bufferWritten = {false};
        program.bindings.push_back(binding);
        if (ownOffset != 0) program.pushConstants.resize(4);
        compute = std::make_unique<CompiledShader>(CompiledShader{stage, &program, 0});
        if (stage == ShaderRecompiler::ShaderStage::Compute) resources = std::make_unique<ShaderResources>(context, *compute);
        else resources = std::make_unique<ShaderResources>(context, std::span<const CompiledShader>(compute.get(), 1), ColorTarget{}, 0, 0, 0, std::span<const GuestMemorySnapshot>{});
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
    const ShaderResources& Resources() const { return *resources; }

    Snapshot Take(std::uint64_t offset, std::size_t size, Statistics* statistics = nullptr, const ShaderResources::ReadAnalysis* analysis = nullptr) {
        ShaderResources::MovedBuffer moved{0, Address() + offset, size, {}};
        if (analysis != nullptr) moved.readAnalysis = *analysis;
        auto bindings = resources->PrepareDrawBindings(*recorder, std::span(&moved, 1), statistics);
        Require(bindings != nullptr && bindings->snapshots.size() == 1, "a moved read-only draw input was not snapshotted");
        held.push_back(bindings);
        return bindings->snapshots.front();
    }

    std::shared_ptr<ShaderResources::DrawBindings> Bindings(std::uint64_t offset, std::size_t size, Statistics* statistics) {
        ShaderResources::MovedBuffer moved{0, Address() + offset, size, {}};
        auto bindings = resources->PrepareDrawBindings(*recorder, std::span(&moved, 1), statistics);
        Require(bindings != nullptr && bindings->snapshots.size() == 1, "a moved read-only draw input was not snapshotted");
        held.push_back(bindings);
        return bindings;
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

void PartialTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    DrawSnapshotArena::SetDeviceTwins(false);
    DrawSnapshotArena::SetVerify(true);
    DrawSnapshotArena::SetExtendBytes(65536);
    constexpr std::uint64_t offset = 131073;
    constexpr std::size_t bytes = 16384;
    constexpr std::size_t viewBytes = bytes + 4;
    auto* data = fixture.Bytes(0);
    for (std::size_t index = 0; index < (std::size_t{1} << 20u); ++index) data[index] = static_cast<std::byte>(index * 7u + 3u);
    const auto analysis = [&](std::uint64_t at, std::size_t size, ShaderRecompiler::BufferReadRanges ranges) {
        const auto address = fixture.Address() + at;
        return ShaderResources::ReadAnalysis{{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, static_cast<std::uint32_t>(size), 0x31000000u}, std::move(ranges)};
    };
    ShaderRecompiler::BufferReadRanges ranges;
    ranges.ranges = {{0u, 0u, 63u, 127u}, {0u, 0u, 8999u, 9011u}};
    const auto limited = analysis(offset, bytes, ranges);
    auto& recorder = fixture.GetRecorder();
    auto& arena = recorder.SnapshotArena();
    arena.TakeCounters();
    GuestMemory::BumpCollectEpoch();
    Statistics first;
    const auto original = fixture.Take(offset, bytes, &first, &limited);
    Require(original.buffer == nullptr && original.address == fixture.Address() + offset - 1 && original.bytes.size() == viewBytes, "an adjusted partial arena snapshot lost its logical extent");
    Require(original.offset % fixture.GetContext().limits.minStorageBufferOffsetAlignment == 0, "a partial arena snapshot has an unaligned descriptor offset");
    Require(first.arenaCopies == 1 && first.limited == 1 && first.copiedBytes == 76 && first.uncopiedBytes == viewBytes - 76, "the arena widened a proved 76-byte read into a full copy");
    Require(std::memcmp(original.bytes.data() + 64, fixture.Bytes(offset - 1 + 64), 64) == 0 && std::memcmp(original.bytes.data() + 9000, fixture.Bytes(offset - 1 + 9000), 12) == 0, "a partial arena snapshot applied its descriptor adjustment twice or not at all");
    auto counters = arena.TakeCounters();
    Require(counters.partialCopies == 1 && counters.copiedBytes == 76 && counters.uncopiedBytes == viewBytes - 76, "the arena did not report its actual partial copy bytes");
    ShaderRecompiler::BufferReadRanges subsetRanges;
    subsetRanges.ranges = {{0u, 0u, 8743u, 8755u}};
    const auto subsetAnalysis = analysis(offset + 256, 12288, subsetRanges);
    Statistics subsetStatistics;
    const auto subset = fixture.Take(offset + 256, 12288, &subsetStatistics, &subsetAnalysis);
    Require(subsetStatistics.arenaHits == 1 && subsetStatistics.copied == 0 && subset.handle == original.handle && subset.offset == original.offset + 256, "a shifted subset of a partial snapshot was recopied or bound at the wrong offset");
    ranges.ranges.insert(ranges.ranges.begin() + 1, {0u, 0u, 255u, 263u});
    const auto changedAnalysis = analysis(offset, bytes, ranges);
    Statistics changedStatistics;
    const auto changed = fixture.Take(offset, bytes, &changedStatistics, &changedAnalysis);
    Require(changedStatistics.arenaCopies == 1 && changedStatistics.limited == 1 && changedStatistics.copiedBytes == 8 && changed.handle == original.handle && changed.offset == original.offset, "a wider read shape of an unchanged partial snapshot did not copy only its missing bytes into it");
    Require(std::memcmp(changed.bytes.data() + 256, fixture.Bytes(offset - 1 + 256), 8) == 0, "a wider read shape reused bytes absent from a partial snapshot");
    *fixture.Bytes(offset - 1 + 64) = std::byte{0xee};
    GuestMemory::BumpCollectEpoch();
    Statistics writtenStatistics;
    const auto written = fixture.Take(offset, bytes, &writtenStatistics, &changedAnalysis);
    Require(writtenStatistics.arenaCopies == 1 && written.bytes[64] == std::byte{0xee}, "a CPU store did not invalidate a partial arena snapshot");
    Require(original.bytes[64] != std::byte{0xee} && changed.bytes[64] != std::byte{0xee}, "a later partial copy overwrote an in-flight snapshot");
    auto downloaded = std::make_shared<Buffer>(fixture.GetContext(), 80, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(fixture.GetContext(), commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    CopyBuffer(fixture.GetContext(), commands, original.handle, original.offset + 64, downloaded->Handle(), 0, 64);
    CopyBuffer(fixture.GetContext(), commands, subset.handle, subset.offset + 8744, downloaded->Handle(), 64, 12);
    CopyBuffer(fixture.GetContext(), commands, written.handle, written.offset + 64, downloaded->Handle(), 76, 4);
    RecordMemoryBarrier(fixture.GetContext(), commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    ShaderRecompiler::BufferReadRanges dynamicRanges;
    dynamicRanges.ranges = {{0u, ShaderRecompiler::BufferReadRanges::AnyIndex, 0u, 4u}};
    dynamicRanges.ranges.front().indexOrigin = ShaderRecompiler::BufferIndexOrigin::Memory;
    auto dynamicAnalysis = analysis(offset, bytes, dynamicRanges);
    dynamicAnalysis.words[1] |= 4u << 16u;
    dynamicAnalysis.words[2] = bytes / 4u;
    Statistics fullStatistics;
    const auto full = fixture.Take(offset, bytes, &fullStatistics, &dynamicAnalysis);
    Require(fullStatistics.arenaCopies == 1 && fullStatistics.limited == 0 && fullStatistics.wholeCopies[static_cast<std::size_t>(ShaderResources::WholeCopy::IndexMemory)] == 1 && SameBytes(full, fixture.Bytes(offset - 1)), "an unbounded read reused incomplete partial bytes");
    bool analysed = false;
    const auto registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto fullHit = arena.Take(recorder, fixture.Address() + offset - 1, viewBytes, registry, [&]() -> std::optional<DrawSnapshotArena::CopyRanges> { analysed = true; return DrawSnapshotArena::CopyRanges{{64, 128}}; });
    Require(fullHit.has_value() && !fullHit->copied && !analysed, "a complete arena hit evaluated read-range analysis");
    const auto noReads = analysis(786433, bytes, {});
    Statistics emptyStatistics;
    fixture.Take(786433, bytes, &emptyStatistics, &noReads);
    Require(emptyStatistics.limited == 1 && emptyStatistics.copiedBytes == 0 && emptyStatistics.uncopiedBytes == viewBytes, "an empty proved read set copied guest data");
    Statistics emptyThenFullStatistics;
    const auto emptyThenFull = fixture.Take(786433, bytes, &emptyThenFullStatistics);
    Require(emptyThenFullStatistics.arenaCopies == 1 && emptyThenFullStatistics.limited == 0 && SameBytes(emptyThenFull, fixture.Bytes(786432)), "an empty partial entry exposed uncopied bytes to an unanalysed draw");
    DrawSnapshotArena::SetDeviceTwins(true);
    const auto mixed = arena.Take(recorder, fixture.Address() + offset - 1, viewBytes, registry);
    Require(mixed.has_value() && mixed->buffer == full.handle, "a chunk containing partial snapshots was copied into a device twin");
    const auto twinAnalysis = analysis(524289, bytes, limited.ranges);
    Statistics twinStatistics;
    const auto withoutTwin = fixture.Take(524289, bytes, &twinStatistics, &twinAnalysis);
    Require(withoutTwin.buffer != nullptr && withoutTwin.twin == nullptr && withoutTwin.offset == 0 && twinStatistics.limited == 1 && twinStatistics.copiedBytes == 76, "a partial snapshot with arena twins enabled exposed uncopied bytes through a twin");
    recorder.Sync();
    const auto output = downloaded->Bytes();
    for (std::size_t index = 0; index < 64; ++index) Require(output[index] == static_cast<std::byte>((offset - 1 + 64 + index) * 7u + 3u), "the GPU lost the original partial snapshot's adjusted bytes");
    for (std::size_t index = 0; index < 12; ++index) Require(output[64 + index] == static_cast<std::byte>((offset - 1 + 9000 + index) * 7u + 3u), "the GPU read a shifted partial subset at the wrong arena offset");
    Require(output[76] == std::byte{0xee}, "the GPU did not read the later CPU-written partial snapshot");
    counters = arena.TakeCounters();
    Require(counters.verified != 0 && counters.verifyDiffering == 0 && counters.twinCopies == 0, "partial arena verification or device-twin exclusion failed");
    DrawSnapshotArena::SetDeviceTwins(false);
    DrawSnapshotArena::SetVerify(false);
    fixture.Release();
}

void ExtendedPartialTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    DrawSnapshotArena::SetDeviceTwins(false);
    DrawSnapshotArena::SetVerify(true);
    constexpr std::uint64_t offset = 393217;
    constexpr std::size_t bytes = 98304;
    constexpr std::size_t viewBytes = bytes + 4;
    auto* data = fixture.Bytes(0);
    for (std::size_t index = 393216; index < 524288; ++index) data[index] = static_cast<std::byte>(index * 5u + 1u);
    GuestMemory::BumpCollectEpoch();
    const auto analysis = [&](std::vector<ShaderRecompiler::BufferReadRange> reads) {
        const auto address = fixture.Address() + offset;
        ShaderRecompiler::BufferReadRanges ranges;
        ranges.ranges = std::move(reads);
        return ShaderResources::ReadAnalysis{{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, static_cast<std::uint32_t>(bytes), 0x31000000u}, std::move(ranges)};
    };
    const auto guest = [&](std::size_t at) { return fixture.Bytes(offset - 1 + at); };
    auto& arena = fixture.GetRecorder().SnapshotArena();
    arena.TakeCounters();
    const auto firstReads = analysis({{0u, 0u, 100u, 164u}});
    Statistics firstStatistics;
    const auto first = fixture.Take(offset, bytes, &firstStatistics, &firstReads);
    Require(firstStatistics.arenaCopies == 1 && firstStatistics.limited == 1 && firstStatistics.copiedBytes == 64 && first.bytes.size() == viewBytes, "a proved 64-byte read was not copied as a partial snapshot");
    const auto widerReads = analysis({{0u, 0u, 100u, 164u}, {0u, 0u, 70000u, 70016u}});
    Statistics widerStatistics;
    const auto wider = fixture.Take(offset, bytes, &widerStatistics, &widerReads);
    Require(widerStatistics.arenaCopies == 1 && widerStatistics.limited == 1 && widerStatistics.copiedBytes == 16 && wider.handle == first.handle && wider.offset == first.offset, "a wider read of an unchanged partial snapshot was not added to it in place");
    Require(std::memcmp(wider.bytes.data() + 101, guest(101), 64) == 0 && std::memcmp(wider.bytes.data() + 70001, guest(70001), 16) == 0, "a partial snapshot extended in place does not hold the guest bytes it covers");
    auto counters = arena.TakeCounters();
    Require(counters.partialCopies == 2 && counters.extendedCopies == 1 && counters.copiedBytes == 80, "the arena did not report its in-place extension");
    *guest(80001) = std::byte{0x5a};
    GuestMemory::BumpCollectEpoch();
    const auto laterReads = analysis({{0u, 0u, 100u, 164u}, {0u, 0u, 80000u, 80004u}});
    Statistics laterStatistics;
    const auto later = fixture.Take(offset, bytes, &laterStatistics, &laterReads);
    Require(laterStatistics.arenaCopies == 1 && laterStatistics.copiedBytes == 4 && later.handle == first.handle && later.offset == first.offset && later.bytes[80001] == std::byte{0x5a}, "a read of bytes stored after the partial snapshot was made was not copied fresh into it");
    *guest(70001) = std::byte{0xa5};
    GuestMemory::BumpCollectEpoch();
    const auto staleReads = analysis({{0u, 0u, 70000u, 70016u}});
    Statistics staleStatistics;
    const auto stale = fixture.Take(offset, bytes, &staleStatistics, &staleReads);
    Require(staleStatistics.arenaCopies == 1 && staleStatistics.copiedBytes == 16 && (stale.handle != first.handle || stale.offset != first.offset) && stale.bytes[70001] == std::byte{0xa5}, "a changed range covered by a partial snapshot was reused or recopied in place");
    Require(wider.bytes[70001] != std::byte{0xa5} && std::memcmp(first.bytes.data() + 101, guest(101), 64) == 0, "a later partial copy overwrote bytes an earlier draw reads");
    DrawSnapshotArena::SetVerify(false);
    fixture.Release();
}

void MovedWordTests(Fixture& fixture, bool ring) {
    ShaderResources::SetMovedWordRing(ring);
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding binding;
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::ShaderData;
    binding.descriptorSet = 0;
    binding.binding = 0;
    binding.count = 1;
    binding.guestDescriptor = {0u, 0u, 0u, 0u};
    program.shaderDataDwords = static_cast<std::uint32_t>(binding.guestDescriptor.size());
    program.bindings.push_back(std::move(binding));
    const CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    ShaderResources resources(fixture.GetContext(), compute);
    std::vector<std::shared_ptr<ShaderResources::DrawBindings>> held;
    const auto take = [&](std::uint32_t value, bool reused) {
        ShaderResources::MovedBuffer moved{0, 0, 16, {value, value + 1u, value + 2u, value + 3u}};
        Statistics statistics;
        const auto bindings = resources.PrepareDrawBindings(fixture.GetRecorder(), std::span(&moved, 1), &statistics);
        Require(bindings != nullptr && bindings->snapshots.size() == 1, "moved words were not snapshotted");
        Require(statistics.reused == (reused ? 1u : 0u) && statistics.copied == (reused ? 0u : 1u) && statistics.copiedBytes == (reused ? 0u : 16u), "moved-word copy accounting used an invalidated cache iterator");
        Require(std::memcmp(bindings->snapshots.front().bytes.data(), moved.words.data(), 16) == 0, "moved-word cache rollover served another value");
        Require(!ring || (bindings->snapshots.front().buffer == nullptr && bindings->snapshots.front().offset % fixture.GetContext().limits.minStorageBufferOffsetAlignment == 0), "ring-placed moved words were not bound at an aligned ring offset");
        held.push_back(bindings);
        return bindings->snapshots.front();
    };
    const auto original = take(1, false);
    for (std::uint32_t value = 2; value <= 8; ++value) take(value, false);
    const auto again = take(1, true);
    Require(again.handle == original.handle && again.offset == original.offset, "reused moved words were not bound where they were placed");
    for (std::uint32_t value = 2; value <= 8; ++value) take(value, true);
    for (std::uint32_t value = 9; value <= 16; ++value) take(value, false);
    for (std::uint32_t value = 9; value <= 16; ++value) take(value, true);
    const auto replaced = take(1, false);
    const std::array<std::uint32_t, 4> originalWords{1u, 2u, 3u, 4u};
    Require((original.handle != replaced.handle || original.offset != replaced.offset) && std::memcmp(original.bytes.data(), originalWords.data(), sizeof(originalWords)) == 0, "moved-word eviction overwrote a snapshot an earlier draw owns");
    ShaderResources::SetMovedWordRing(false);
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

void WorkerCopyTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    DrawSnapshotArena::SetExtendBytes(0);
    auto& recorder = fixture.GetRecorder();
    auto& arena = recorder.SnapshotArena();
    const auto whole = [] { return std::optional<DrawSnapshotArena::CopyRanges>{}; };
    const auto address = fixture.Address() + 131072;
    std::memset(fixture.Bytes(131072), 0x71, 65536);
    GuestMemory::BumpCollectEpoch();
    const auto registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    auto prefill = arena.PrepareCopy(address, 4096, registry, std::nullopt);
    Require(prefill.has_value() && prefill->copiedBytes == 4096 && prefill->first == address, "a worker did not copy an unserved draw input");
    const auto adopted = arena.TakePrefilled(recorder, address, 4096, registry, whole, *prefill);
    Require(adopted.has_value() && adopted->copied && Holds(Snapshot{address, nullptr, {}, adopted->buffer, adopted->offset, adopted->bytes}, std::byte{0x71}), "the encode did not adopt a worker's copy");
    const auto again = arena.Take(recorder, address, 4096, registry);
    Require(again.has_value() && !again->copied && again->buffer == adopted->buffer && again->offset == adopted->offset, "an adopted copy was not indexed for later draws");
    DrawSnapshotArena::SetWorkerHits(false);
    Require(!arena.PrepareCopy(address, 4096, registry, std::nullopt).has_value(), "a worker copied a range the arena already serves");
    DrawSnapshotArena::SetWorkerHits(true);
    static_cast<void>(arena.TakeCounters());
    const auto hit = arena.PrepareCopy(address + 256, 1024, registry, std::nullopt);
    Require(hit.has_value() && hit->hit && hit->copiedBytes == 0, "a worker did not hand over the arena hit for a range the arena already serves");
    const auto adoptedHit = arena.TakePrefilled(recorder, address + 256, 1024, registry, whole, *hit);
    const auto hitCounts = arena.TakeCounters();
    Require(adoptedHit.has_value() && !adoptedHit->copied && adoptedHit->buffer == adopted->buffer && adoptedHit->offset == adopted->offset + 256 && hitCounts.workerHits == 1 && hitCounts.hits == 1, "the encode did not take a worker's arena hit without a lookup");
    Require(!arena.TakePrefilled(recorder, address + 512, 1024, registry, whole, *hit).value().copied, "a worker's hit for another range was not looked up again");
    const auto stale = address + 16384;
    auto early = arena.PrepareCopy(stale, 4096, registry, std::nullopt);
    Require(early.has_value(), "a worker did not copy the second input");
    std::memset(fixture.Bytes(131072 + 16384), 0x73, 64);
    GuestMemory::BumpCollectEpoch();
    const auto fresh = arena.TakePrefilled(recorder, stale, 4096, registry, whole, *early);
    Require(fresh.has_value() && fresh->bytes[0] == std::byte{0x73} && fresh->bytes[64] == std::byte{0x71}, "a worker's copy made before a CPU store was adopted");
    const auto partial = address + 32768;
    const DrawSnapshotArena::CopyRanges ranges{{0, 256}, {1024, 2048}};
    auto limited = arena.PrepareCopy(partial, 4096, registry, ranges);
    Require(limited.has_value() && limited->copiedBytes == 1280, "a worker did not copy only the read ranges");
    const auto bounded = arena.TakePrefilled(recorder, partial, 4096, registry, [&] { return std::optional<DrawSnapshotArena::CopyRanges>{ranges}; }, *limited);
    Require(bounded.has_value() && bounded->limited && bounded->bytes[0] == std::byte{0x71} && bounded->bytes[1500] == std::byte{0x71}, "the encode did not adopt a worker's range-limited copy");
    fixture.Release();
    recorder.Sync();
}

void InPlaceTests(Fixture& fixture) {
    const bool previous = ShaderResources::InPlaceDrawInputsEnabled();
    ShaderResources::SetInPlaceDrawInputs(true);
    DrawSnapshotArena::SetEnabled(true);
    auto& recorder = fixture.GetRecorder();
    std::memset(fixture.Bytes(49152), 0x3c, 1024);
    GuestMemory::BumpCollectEpoch();
    Require(fixture.TakeOwn() == nullptr, "an in-place template input was snapshotted");
    Statistics moved{};
    const auto snapshot = fixture.Take(49152, 1024, &moved);
    Require(snapshot.inPlace && snapshot.buffer == nullptr && snapshot.bytes.data() == fixture.Bytes(49152) && snapshot.bytes.size() == 1024 && moved.inPlace == 1 && moved.copied == 0, "a moved read-only input was not bound through its host import");
    Require(snapshot.offset % fixture.GetContext().limits.minStorageBufferOffsetAlignment == 0, "an in-place input was bound at a misaligned offset");
    Require(recorder.PendingReadOverlaps(fixture.Address() + 49152, 1024), "an in-place input was not noted as a pending read");
    ShaderResources::SetInPlaceDrawInputs(false);
    Statistics copied{};
    const auto snapshotted = fixture.Take(49152, 1024, &copied);
    Require(!snapshotted.inPlace && snapshotted.bytes.data() != fixture.Bytes(49152) && copied.inPlace == 0 && Holds(snapshotted, std::byte{0x3c}), "the switch off still bound the input in place");
    fixture.Release();
    recorder.Sync();
    ShaderResources::SetInPlaceDrawInputs(previous);
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

void EpochDirtySetTests(Fixture& fixture) {
    struct Restore {
        bool enabled = GuestMemory::EpochDirtySetEnabled();
        bool verify = GuestMemory::EpochDirtySetVerify();
        ~Restore() {
            GuestMemory::SetEpochDirtySet(enabled);
            GuestMemory::SetEpochDirtySetVerify(verify);
        }
    } restore;
    for (const bool arena : {false, true}) {
        DrawSnapshotArena::SetEnabled(arena);
        DrawSnapshotArena::SetExtendBytes(0);
        GuestMemory::SetEpochDirtySet(true);
        GuestMemory::SetEpochDirtySetVerify(true);
        const auto disagreementsBefore = GuestMemory::DirtySetStatistics().verifyDisagreements;
        const char* mode = arena ? " (arena)" : " (per-draw buffers)";
        std::memset(fixture.Bytes(16384), 0x61, 8192);
        GuestMemory::BumpCollectEpoch();
        Statistics first{};
        const auto a = fixture.Take(16384, 4096, &first);
        Require(first.copied == 1 && Holds(a, std::byte{0x61}), std::string("the first snapshot under the dirty set does not hold the guest bytes") + mode);
        Statistics second{};
        const auto b = fixture.Take(16384, 4096, &second);
        Require(second.reused == 1 && second.copied == 0 && Holds(b, std::byte{0x61}), std::string("an unchanged range was copied again in the same epoch") + mode);
        GuestMemory::BumpCollectEpoch();
        Statistics third{};
        const auto c = fixture.Take(16384, 4096, &third);
        Require(third.reused == 1 && third.copied == 0 && Holds(c, std::byte{0x61}), std::string("an unchanged range was copied again in a new epoch") + mode);
        std::memset(fixture.Bytes(16384 + 512), 0x62, 64);
        GuestMemory::BumpCollectEpoch();
        Statistics fourth{};
        const auto d = fixture.Take(16384, 4096, &fourth);
        Require(fourth.copied == 1 && SameBytes(d, fixture.Bytes(16384)), std::string("a CPU store before an ordering point did not make the snapshot again") + mode);
        Require(Holds(a, std::byte{0x61}), std::string("a new snapshot overwrote the bytes an earlier draw binds") + mode);
        std::memset(fixture.Bytes(16384 + 1024), 0x63, 16);
        GuestMemory::MarkWritten(fixture.Address() + 16384 + 1024, 16);
        Statistics fifth{};
        const auto e = fixture.Take(16384, 4096, &fifth);
        Require(fifth.copied == 1 && SameBytes(e, fixture.Bytes(16384)), std::string("a driver store in the same epoch did not make the snapshot again") + mode);
        Statistics sixth{};
        const auto f = fixture.Take(16384, 4096, &sixth);
        Require(sixth.reused == 1 && sixth.copied == 0 && SameBytes(f, fixture.Bytes(16384)), std::string("an unchanged range after a driver store was copied again") + mode);
        {
            GuestAllocations::Mutation mutation;
        }
        Statistics seventh{};
        const auto g = fixture.Take(16384, 4096, &seventh);
        Require(seventh.copied == 1 && SameBytes(g, fixture.Bytes(16384)), std::string("a snapshot outlived a registry mutation") + mode);
        for (std::size_t draw = 0; draw < 512; ++draw) {
            if (draw % 64 == 0) {
                fixture.Release();
                fixture.GetRecorder().Submit();
                std::memset(fixture.Bytes((draw * 4096) % 32768), static_cast<int>(draw), 32);
                GuestMemory::BumpCollectEpoch();
            }
            const auto offset = (draw * 256) % 32768;
            const auto taken = fixture.Take(offset, 256);
            Require(SameBytes(taken, fixture.Bytes(offset)), std::string("a snapshot of the ring differs from the guest bytes") + mode);
        }
        fixture.Release();
        fixture.GetRecorder().Sync();
        Require(GuestMemory::DirtySetStatistics().verifyDisagreements == disagreementsBefore, std::string("the dirty set disagreed with a stamp check") + mode);
    }
    DrawSnapshotArena::SetEnabled(false);
}

void EpochDirtyOwnedRangeTests(const Context& context) {
    const Settings settings;
    struct Restore {
        bool enabled = GuestMemory::EpochDirtySetEnabled();
        bool verify = GuestMemory::EpochDirtySetVerify();
        ~Restore() {
            GuestMemory::SetEpochDirtySet(enabled);
            GuestMemory::SetEpochDirtySetVerify(verify);
        }
    } restore;
    constexpr std::size_t blockBytes = 65536;
    constexpr std::size_t smallBytes = 4096;
    constexpr std::size_t vertexBytes = 2 * blockBytes;
    DrawSnapshotArena::SetEnabled(true);
    DrawSnapshotArena::SetChunkBytes(std::size_t{4} << 20u);
    DrawSnapshotArena::SetBudgetBytes(std::size_t{8} << 20u);
    DrawSnapshotArena::SetExtendBytes(std::size_t{1} << 20u);
    GuestMemory::SetEpochDirtySet(true);
    for (const bool verify : {false, true}) {
        Fixture fixture(context, std::size_t{2} << 20u);
        Require(fixture.Ready(), "the owned-range fixture lost its host import");
        GuestMemory::SetEpochDirtySetVerify(verify);
        const char* mode = verify ? " (verified)" : " (unverified)";
        const auto disagreementsBefore = GuestMemory::DirtySetStatistics().verifyDisagreements;
        std::memset(fixture.Bytes(0), 0x71, std::size_t{2} << 20u);
        GuestMemory::BumpCollectEpoch();
        Require(GuestMemory::CollectWrites(fixture.Address(), std::size_t{2} << 20u) != 0, "the owned arena range was not collected before its snapshot");
        Statistics first{};
        const auto prefix = fixture.Take(0, smallBytes, &first);
        Require(first.arenaCopies == 1 && Holds(prefix, std::byte{0x71}), std::string("the widened arena snapshot was not copied") + mode);
        Statistics second{};
        const auto tail = fixture.Take(blockBytes, smallBytes, &second);
        Require(second.arenaHits == 1 && tail.handle == prefix.handle && Holds(tail, std::byte{0x71}), std::string("the default arena extension did not own the later range") + mode);
        std::memset(fixture.Bytes(blockBytes), 0x72, smallBytes);
        GuestMemory::MarkWritten(fixture.Address() + blockBytes, smallBytes);
        Statistics third{};
        const auto cleanPrefix = fixture.Take(0, smallBytes, &third);
        Require(third.arenaHits == 1 && Holds(cleanPrefix, std::byte{0x71}), std::string("a write outside the requested prefix prevented its reuse") + mode);
        Statistics fourth{};
        const auto changedTail = fixture.Take(blockBytes, smallBytes, &fourth);
        Require(fourth.arenaCopies == 1 && Holds(changedTail, std::byte{0x72}), std::string("a prefix check hid a write elsewhere in the widened arena snapshot") + mode);
        Require(Holds(tail, std::byte{0x71}), std::string("a widened arena refresh overwrote an earlier draw's bytes") + mode);

        auto& recorder = fixture.GetRecorder();
        const auto address = fixture.Address() + 4 * blockBytes;
        GuestMemory::BumpCollectEpoch();
        const auto generation = GuestMemory::CollectWrites(address, vertexBytes);
        Require(generation != 0, "the vertex snapshot range was not collected");
        auto vertex = std::make_shared<Buffer>(fixture.GetContext(), vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        std::memcpy(vertex->Bytes().data(), reinterpret_cast<const void*>(address), vertexBytes);
        recorder.KeepDrawSnapshot(address, vertexBytes, generation, GuestAllocations::GuestAllocationsGeneration_nid_postfix(), vertex, Recorder::SnapshotUse::Vertex, 0, GuestMemory::DirtySerial());
        std::memset(fixture.Bytes(5 * blockBytes), 0x73, smallBytes);
        GuestMemory::MarkWritten(address + blockBytes, smallBytes);
        Require(recorder.ReusableDrawSnapshot(address, smallBytes, Recorder::SnapshotUse::Vertex) == vertex, std::string("a write outside the smaller vertex request prevented its reuse") + mode);
        Require(recorder.ReusableDrawSnapshot(address, vertexBytes, Recorder::SnapshotUse::Vertex) == nullptr, std::string("a smaller vertex request hid a write in the larger owned snapshot") + mode);
        Require(vertex->Bytes()[blockBytes] == std::byte{0x71}, std::string("a vertex cache rejection overwrote an earlier draw's bytes") + mode);

        const std::array<std::uint64_t, 3> stamp{GuestMemory::DirtySerial(), GuestMemory::CollectEpoch(), GuestAllocations::GuestAllocationsGeneration_nid_postfix()};
        recorder.NoteUnwrittenDrawSnapshot(address, vertexBytes, stamp, generation, vertex);
        GuestMemory::SetEpochDirtySet(false);
        Require(recorder.UnwrittenDrawSnapshot(address, vertexBytes, stamp) == nullptr, "equal serial and tracker numbers reused a snapshot across disabling the dirty set");
        recorder.NoteUnwrittenDrawSnapshot(address, vertexBytes, stamp, generation, vertex);
        GuestMemory::SetEpochDirtySet(true);
        Require(recorder.UnwrittenDrawSnapshot(address, vertexBytes, stamp) == nullptr, "equal tracker and serial numbers reused a snapshot across enabling the dirty set");
        fixture.Release();
        recorder.Sync();
        Require(GuestMemory::DirtySetStatistics().verifyDisagreements == disagreementsBefore, std::string("the owned-range dirty proof disagreed with its stamp check") + mode);
    }
}

void EpochDirtyReferenceTests(const Context& context) {
    const Settings settings;
    struct Restore {
        bool enabled = GuestMemory::EpochDirtySetEnabled();
        bool verify = GuestMemory::EpochDirtySetVerify();
        ~Restore() {
            GuestMemory::SetEpochDirtySet(enabled);
            GuestMemory::SetEpochDirtySetVerify(verify);
        }
    } restore;
    constexpr std::size_t blockBytes = 65536;
    constexpr std::size_t snapshotBytes = 16;
    DrawSnapshotArena::SetEnabled(false);
    GuestMemory::SetEpochDirtySet(true);
    for (const bool verify : {false, true}) {
        Fixture fixture(context, 2 * blockBytes);
        Require(fixture.Ready(), "the reference-granularity fixture lost its host import");
        GuestMemory::SetEpochDirtySetVerify(verify);
        auto& recorder = fixture.GetRecorder();
        const auto address = fixture.Address();
        const auto disagreementsBefore = GuestMemory::DirtySetStatistics().verifyDisagreements;
        std::memset(fixture.Bytes(0), 0x71, 2 * blockBytes);
        const auto snapshot = [&] {
            auto buffer = std::make_shared<Buffer>(fixture.GetContext(), snapshotBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            std::memcpy(buffer->Bytes().data(), fixture.Bytes(0), snapshotBytes);
            return buffer;
        };
        const auto stamp = [&] {
            return std::array<std::uint64_t, 3>{GuestMemory::DirtySerial(), GuestMemory::CollectEpoch(), GuestAllocations::GuestAllocationsGeneration_nid_postfix()};
        };
        auto unknown = snapshot();
        const auto knownGeneration = GuestMemory::CollectWritesUncached(address + blockBytes, snapshotBytes);
        Require(knownGeneration != 0, "the unknown-generation control snapshot was not collected");
        const auto unknownStamp = stamp();
        recorder.NoteUnwrittenDrawSnapshot(address + blockBytes, snapshotBytes, unknownStamp, knownGeneration, unknown);
        Require(recorder.UnwrittenDrawSnapshot(address + blockBytes, snapshotBytes, unknownStamp) == unknown, "a valid generation failed the unknown-generation control");
        recorder.NoteUnwrittenDrawSnapshot(address + blockBytes, snapshotBytes, unknownStamp, 0, unknown);
        Require(recorder.UnwrittenDrawSnapshot(address + blockBytes, snapshotBytes, unknownStamp) == nullptr, "an unknown generation reused an unwritten snapshot at an equal serial");
        Require(GuestMemory::MarkWritten(address, snapshotBytes) != 0, "the unknown-generation fixture could not advance the dirty serial");
        Require(recorder.UnwrittenDrawSnapshot(address + blockBytes, snapshotBytes, stamp()) == nullptr, "an unknown generation reused an unwritten snapshot across serials");
        for (const auto offset : {snapshotBytes, std::size_t{4096}}) {
            GuestMemory::BumpCollectEpoch();
            const auto generation = GuestMemory::CollectWritesUncached(address, 2 * blockBytes);
            Require(generation != 0, "the reference-granularity snapshot was not collected");
            auto buffer = snapshot();
            const auto captured = stamp();
            recorder.KeepDrawSnapshot(address, snapshotBytes, generation, captured[2], buffer, Recorder::SnapshotUse::Vertex, 0, captured[0]);
            recorder.NoteUnwrittenDrawSnapshot(address, snapshotBytes, captured, generation, buffer);
            Require(GuestMemory::MarkWritten(address + offset, snapshotBytes) != 0, "a sibling driver store was not stamped");
            Require(recorder.ReusableDrawSnapshot(address, snapshotBytes, Recorder::SnapshotUse::Vertex) == nullptr, "a disjoint driver store in the reference block retained a vertex proof");
            Require(recorder.UnwrittenDrawSnapshot(address, snapshotBytes, stamp()) == nullptr, "a disjoint driver store in the reference block retained an unwritten proof");
            Require(buffer->Bytes().front() == std::byte{0x71}, "rejecting a sibling driver store changed an earlier snapshot");
        }
        GuestMemory::BumpCollectEpoch();
        const auto unrelatedGeneration = GuestMemory::CollectWritesUncached(address, 2 * blockBytes);
        Require(unrelatedGeneration != 0, "the unrelated-block snapshot was not collected");
        auto unrelated = snapshot();
        const auto unrelatedStamp = stamp();
        recorder.KeepDrawSnapshot(address, snapshotBytes, unrelatedGeneration, unrelatedStamp[2], unrelated, Recorder::SnapshotUse::Vertex, 0, unrelatedStamp[0]);
        recorder.NoteUnwrittenDrawSnapshot(address, snapshotBytes, unrelatedStamp, unrelatedGeneration, unrelated);
        Require(GuestMemory::MarkWritten(address + blockBytes + 4096, snapshotBytes) != 0, "a driver store in a distinct block was not stamped");
        Require(recorder.ReusableDrawSnapshot(address, snapshotBytes, Recorder::SnapshotUse::Vertex) == unrelated && recorder.UnwrittenDrawSnapshot(address, snapshotBytes, stamp()) == unrelated, "a store in a distinct 64 KiB block prevented snapshot reuse");
        Require(GuestMemory::MarkWritten(address, snapshotBytes) != 0, "the serial-advancement store was not stamped");
        Require(recorder.ReusableDrawSnapshot(address, snapshotBytes, Recorder::SnapshotUse::Vertex) == nullptr && recorder.UnwrittenDrawSnapshot(address, snapshotBytes, stamp()) == nullptr, "advancing a reusable snapshot serial hid a later overlapping store");
        Require(unrelated->Bytes().front() == std::byte{0x71}, "advancing a reusable snapshot serial changed an earlier draw's bytes");
        GuestMemory::BumpCollectEpoch();
        const auto generation = GuestMemory::CollectWritesUncached(address, 2 * blockBytes);
        Require(generation != 0, "the overlapping snapshot was not collected");
        auto buffer = snapshot();
        const auto captured = stamp();
        recorder.KeepDrawSnapshot(address, snapshotBytes, generation, captured[2], buffer, Recorder::SnapshotUse::Vertex, 0, captured[0]);
        recorder.NoteUnwrittenDrawSnapshot(address, snapshotBytes, captured, generation, buffer);
        fixture.Bytes(0)[0] = std::byte{0x72};
        Require(GuestMemory::CollectWritesUncached(address, snapshotBytes) != 0, "the overlapping CPU store was not collected");
        Require(recorder.ReusableDrawSnapshot(address, snapshotBytes, Recorder::SnapshotUse::Vertex) == nullptr && recorder.UnwrittenDrawSnapshot(address, snapshotBytes, stamp()) == nullptr, "a collected overlapping CPU store reused an earlier snapshot");
        Require(buffer->Bytes().front() == std::byte{0x71}, "rejecting an overlapping CPU store changed an earlier snapshot");
        Require(GuestMemory::DirtySetStatistics().verifyDisagreements == disagreementsBefore, "reference-granularity proofs still disagreed with the block stamps");
        if (verify) {
            const auto staleGeneration = GuestMemory::CollectWritesUncached(address, snapshotBytes);
            Require(staleGeneration != 0, "the stale-proof fixture was not collected");
            auto stale = snapshot();
            fixture.Bytes(0)[0] = std::byte{0x73};
            Require(GuestMemory::MarkWritten(address, snapshotBytes) != 0, "the stale-proof store was not stamped");
            recorder.KeepDrawSnapshot(address, snapshotBytes, staleGeneration, GuestAllocations::GuestAllocationsGeneration_nid_postfix(), stale, Recorder::SnapshotUse::Vertex, 0, GuestMemory::DirtySerial());
            Require(recorder.ReusableDrawSnapshot(address, snapshotBytes, Recorder::SnapshotUse::Vertex) == nullptr, "verification accepted a stale snapshot with an incorrectly advanced dirty serial");
            Require(GuestMemory::DirtySetStatistics().verifyDisagreements == disagreementsBefore + 1, "a genuine stale snapshot disagreement was suppressed");
            Require(stale->Bytes().front() == std::byte{0x72}, "a stale proof rewrote the earlier draw's snapshot");
            std::cout << "epoch reference proofs: one deliberately stale serial rejected and counted\n";
        }
        fixture.Release();
        recorder.Sync();
    }
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

void PersistentSetTests(Fixture& fixture) {
    DrawSnapshotArena::SetEnabled(true);
    DrawSnapshotArena::SetExtendBytes(0);
    ShaderResources::SetRecordBindingsMode(ShaderResources::RecordBindings::Persistent);
    std::memset(fixture.Bytes(0), 0x55, 4096);
    GuestMemory::BumpCollectEpoch();
    Statistics first{};
    const auto a = fixture.Bindings(0, 1024, &first);
    Require(first.persistentSets == 0 && a->allocation.set != VK_NULL_HANDLE, "the first draw over an input found a persistent descriptor set");
    GuestMemory::BumpCollectEpoch();
    Statistics second{};
    const auto b = fixture.Bindings(0, 1024, &second);
    Require(second.arenaHits == 1 && second.persistentSets == 1 && b->allocation.set == a->allocation.set, "a draw over an unchanged arena snapshot did not reuse the descriptor set of the draw before (" + std::to_string(second.arenaHits) + " arena hits, " + std::to_string(second.persistentSets) + " sets reused)");
    Statistics other{};
    const auto c = fixture.Bindings(1024, 1024, &other);
    Require(other.persistentSets == 0 && c->allocation.set != a->allocation.set, "a draw over another range reused a descriptor set bound to the first range");
    std::memset(fixture.Bytes(0), 0x66, 1024);
    GuestMemory::BumpCollectEpoch();
    Statistics changed{};
    const auto d = fixture.Bindings(0, 1024, &changed);
    Require(changed.arenaCopies == 1 && changed.persistentSets == 0 && d->allocation.set != a->allocation.set && Holds(d->snapshots.front(), std::byte{0x66}), "a draw over a range a CPU store changed reused the old descriptor set");
    Require(Holds(a->snapshots.front(), std::byte{0x55}), "a new snapshot overwrote the bytes behind a persistent descriptor set");
    ShaderResources::SetRecordBindingsMode(ShaderResources::RecordBindings::Legacy);
    GuestMemory::BumpCollectEpoch();
    Statistics legacy{};
    const auto e = fixture.Bindings(0, 1024, &legacy);
    Require(legacy.arenaHits == 1 && legacy.persistentSets == 0 && e->allocation.set != d->allocation.set, "APS5_RECORD_BINDINGS=legacy reused a descriptor set");
    ShaderResources::SetRecordBindingsMode(ShaderResources::RecordBindings::Persistent);
    Require(ShaderResources::ParseRecordBindings(nullptr) == ShaderResources::RecordBindings::Persistent && ShaderResources::ParseRecordBindings("legacy") == ShaderResources::RecordBindings::Legacy && ShaderResources::ParseRecordBindings("0") == ShaderResources::RecordBindings::Legacy, "APS5_RECORD_BINDINGS parses to the wrong mode");
    fixture.Release();
    fixture.GetRecorder().Sync();
}

void DrawParamsTests(const Context& context) {
    const bool previous = ShaderResources::DrawParamsEnabled();
    ShaderResources::SetDrawParams(true);
    struct Restore {
        bool previous;
        ~Restore() { ShaderResources::SetDrawParams(previous); }
    } restore{previous};
    {
        Fixture fixture(context, std::size_t{1} << 20u, 0, ShaderRecompiler::ShaderStage::Fragment);
        if (!fixture.Ready()) return;
        Require(fixture.Resources().TemplateOffsets(0).size() == 1, "a read-only guest buffer of a draw is not a dynamic storage buffer");
        std::memset(fixture.Bytes(4096), 0x21, 1024);
        std::memset(fixture.Bytes(8192), 0x22, 1024);
        GuestMemory::BumpCollectEpoch();
        Statistics first{};
        const auto a = fixture.Bindings(4096, 1024, &first);
        Statistics second{};
        const auto b = fixture.Bindings(8192, 1024, &second);
        Require(a->dynamicOffsets[0].size() == 1 && a->dynamicOffsets[0][0] == a->snapshots.front().offset && b->dynamicOffsets[0].size() == 1 && b->dynamicOffsets[0][0] == b->snapshots.front().offset, "a draw's dynamic offset is not its snapshot's offset");
        Require(Holds(a->snapshots.front(), std::byte{0x21}) && Holds(b->snapshots.front(), std::byte{0x22}), "draw parameters changed a snapshot's bytes");
        if (a->snapshots.front().handle == b->snapshots.front().handle) Require(second.persistentSets == 1 && b->allocation.set == a->allocation.set, "two draws whose inputs sit in one arena buffer at different offsets did not share a descriptor set");
        ShaderResources::SetRecordBindingsMode(ShaderResources::RecordBindings::Legacy);
        Statistics legacy{};
        const auto c = fixture.Bindings(4096, 1024, &legacy);
        Require(legacy.persistentSets == 0 && c->allocation.set != a->allocation.set && c->dynamicOffsets[0].size() == 1 && c->dynamicOffsets[0][0] == c->snapshots.front().offset && Holds(c->snapshots.front(), std::byte{0x21}), "APS5_RECORD_BINDINGS=legacy with draw parameters bound another range or reused a set");
        ShaderResources::SetRecordBindingsMode(ShaderResources::RecordBindings::Persistent);
        fixture.Release();
        fixture.GetRecorder().Sync();
    }
    ShaderResources::SetDrawParams(false);
    Fixture plain(context, std::size_t{1} << 20u, 0, ShaderRecompiler::ShaderStage::Fragment);
    if (!plain.Ready()) return;
    Require(plain.Resources().TemplateOffsets(0).empty(), "APS5_DRAW_PARAMS=0 still made a dynamic storage buffer");
    const auto bound = plain.Bindings(4096, 1024, nullptr);
    Require(bound->dynamicOffsets[0].empty(), "APS5_DRAW_PARAMS=0 still gave a draw dynamic offsets");
    plain.Release();
    plain.GetRecorder().Sync();
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
        PersistentSetTests(fixture);
        MovedWordTests(fixture, false);
        MovedWordTests(fixture, true);
        PendingWriteTests(fixture);
        GpuReadTests(fixture);
        DeviceTwinTests(fixture);
        WorkerCopyTests(fixture);
        DisabledTests(fixture);
        EpochDirtySetTests(fixture);
    }
    DrawParamsTests(context);
    EpochDirtyOwnedRangeTests(context);
    EpochDirtyReferenceTests(context);
    {
        Fixture fixture(context, std::size_t{1} << 20u, 1);
        Require(fixture.Ready(), "the partial arena fixture lost its host import");
        PartialTests(fixture);
    }
    {
        Fixture fixture(context, std::size_t{1} << 20u, 1);
        Require(fixture.Ready(), "the extended partial arena fixture lost its host import");
        ExtendedPartialTests(fixture);
    }
    {
        DrawSnapshotArena::SetChunkBytes(65536);
        DrawSnapshotArena::SetBudgetBytes(2 * 65536);
        Fixture fixture(context, std::size_t{1} << 20u);
        Require(fixture.Ready(), "the recycle fixture lost its host import");
        RecycleTests(fixture);
    }
    {
        Fixture fixture(context, std::size_t{1} << 20u);
        Require(fixture.Ready(), "the in-place input fixture lost its host import");
        InPlaceTests(fixture);
    }
    std::cout << "draw snapshot arena tests passed\n";
    return true;
}

bool RunEpochDirtySetBenchmark(const Context& context) {
    const Settings settings;
    struct Restore {
        bool enabled = GuestMemory::EpochDirtySetEnabled();
        ~Restore() { GuestMemory::SetEpochDirtySet(enabled); }
    } restore;
    constexpr std::size_t blockBytes = std::size_t{32} << 20u;
    Fixture fixture(context, blockBytes);
    if (!fixture.Ready()) {
        std::cout << "host imports or write watching unavailable: epoch dirty set not measured\n";
        return false;
    }
    for (std::size_t offset = 0; offset < blockBytes; offset += 4096) std::memset(fixture.Bytes(offset), static_cast<int>(offset / 4096), 4096);
    const std::vector<Pattern> patterns{
        {"ring of 256 B constants", 4096, 64, false, [](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{(draw * 256) % (std::size_t{8} << 20u), 256}; }},
        {"ring of 4 KiB constants", 4096, 64, false, [](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{(draw * 4096) % (std::size_t{24} << 20u), 4096}; }},
        {"128 KiB windows sliding 4 KiB", 4096, 64, false, [](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{(draw % 2048) * 4096, std::size_t{128} << 10u}; }},
        {"same 16 ranges every draw", 4096, 64, false, [](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{(draw % 16) * 65536, 4096}; }},
        {"same 16 ranges, one stored each epoch", 4096, 64, true, [](std::size_t draw) { return std::pair<std::uint64_t, std::size_t>{(draw % 16) * 65536, 4096}; }},
    };
    for (const bool arena : {false, true}) {
        for (const auto& pattern : patterns) {
            Statistics offStatistics{};
            Statistics onStatistics{};
            DrawSnapshotArena::Counters counters{};
            GuestMemory::SetEpochDirtySet(false);
            const auto offUs = MeasurePattern(fixture, pattern, arena, offStatistics, counters);
            GuestMemory::SetEpochDirtySet(true);
            const auto before = GuestMemory::DirtySetStatistics();
            const auto onUs = MeasurePattern(fixture, pattern, arena, onStatistics, counters);
            const auto after = GuestMemory::DirtySetStatistics();
            const auto parts = [&](const Statistics& part) {
                const auto draws = static_cast<double>(pattern.draws);
                return std::string("checks ") + std::to_string(part.checkUs / draws).substr(0, 5) + " collects " + std::to_string(part.collectUs / draws).substr(0, 5) + " lookups " + std::to_string(part.lookupUs / draws).substr(0, 5) + " copies " + std::to_string(part.copyUs / draws).substr(0, 5);
            };
            std::printf("[dirty-set-bench] %-40s %s: off %7.2f us (%s) | on %7.2f us (%s; dirty checks %llu, serial hits %llu, log clean %llu, widened walks %llu)\n", pattern.name, arena ? "arena" : "per-draw buffers", offUs, parts(offStatistics).c_str(), onUs, parts(onStatistics).c_str(), static_cast<unsigned long long>(after.checks - before.checks), static_cast<unsigned long long>(after.serialHits - before.serialHits), static_cast<unsigned long long>(after.logClean - before.logClean), static_cast<unsigned long long>(after.widenedWalks - before.widenedWalks));
        }
    }
    {
        Context context = fixture.GetContext();
        TextureDetiler detiler(context);
        context.detiler = &detiler;
        PlaceholderImages placeholders(context);
        context.placeholderImages = &placeholders;
        TextureCache textureCache(context);
        context.textureCache = &textureCache;
        const std::lock_guard gpu(GuestMemory::GpuMutex());
        constexpr std::size_t surfaces = 16;
        constexpr std::uint32_t side = 128;
        constexpr std::size_t surfaceBytes = std::size_t{side} * side * 4;
        const auto base = fixture.Address() + (std::size_t{16} << 20u);
        ShaderRecompiler::DescriptorBinding binding{};
        binding.kind = ShaderRecompiler::DescriptorKind::SampledImage;
        binding.role = ShaderRecompiler::DescriptorRole::GuestImages;
        binding.descriptorSet = 0;
        binding.binding = ShaderRecompiler::RuntimeAbi::FirstImageBinding + PlaceholderDimensionsPerClass + 2u;
        binding.count = surfaces;
        binding.imageShape = ShaderRecompiler::DescriptorImageShape::Image2D;
        binding.imageWritten.assign(surfaces, false);
        binding.imageSamplers.assign(surfaces, 0u);
        for (std::size_t i = 0; i < surfaces; ++i) {
            const auto address = base + i * surfaceBytes;
            const std::array<std::uint32_t, 8> words{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (20u << 20u) | (((side - 1u) & 3u) << 30u), ((side - 1u) >> 2u) | ((side - 1u) << 14u), 0xfacu | (0x1bu << 20u) | (9u << 28u), 0u, 0u, 0u, 0u};
            binding.guestDescriptor.insert(binding.guestDescriptor.end(), words.begin(), words.end());
        }
        ShaderRecompiler::RecompileResult program;
        program.bindings.push_back(std::move(binding));
        const CompiledShader shader{ShaderRecompiler::ShaderStage::Compute, &program, 0};
        for (const bool enabled : {false, true}) {
            GuestMemory::SetEpochDirtySet(enabled);
            GuestMemory::BumpCollectEpoch();
            auto resources = std::make_shared<ShaderResources>(context, shader);
            Require(resources->Reusable() && resources->Revalidate(shader), "the sampled template does not revalidate");
            constexpr int calls = 4096;
            const auto skipsBefore = ShaderResources::EpochSkips();
            const auto start = std::chrono::steady_clock::now();
            for (int call = 0; call < calls; ++call) {
                if (call % 64 == 0) GuestMemory::BumpCollectEpoch();
                Require(resources->Revalidate(shader), "a revalidation of the unchanged sampled template failed");
            }
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / calls;
            std::printf("[dirty-set-bench] Revalidate of a template with %zu sampled textures, epoch every 64 calls: dirty set %s %7.2f us per call (%llu skipped)\n", surfaces, enabled ? "on " : "off", us, static_cast<unsigned long long>(ShaderResources::EpochSkips() - skipsBefore));
            resources.reset();
            ClearCachedTextures(context.device);
        }
    }
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

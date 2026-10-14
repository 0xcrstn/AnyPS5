#include "prx/libSceAgcDriver/Graphics/include/DrawSnapshotArena.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Graphics {

namespace {

std::size_t EnvironmentBytes(const char* name, std::size_t unit, std::size_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    return static_cast<std::size_t>(std::strtoull(text, nullptr, 10)) * unit;
}

std::atomic<bool>& EnabledFlag() {
    static std::atomic<bool> flag{std::getenv("APS5_DRAW_SNAPSHOT_ARENA") != nullptr && std::strcmp(std::getenv("APS5_DRAW_SNAPSHOT_ARENA"), "0") != 0};
    return flag;
}

std::atomic<std::size_t>& ChunkSetting() {
    static std::atomic<std::size_t> bytes{std::max<std::size_t>(EnvironmentBytes("APS5_DRAW_SNAPSHOT_ARENA_CHUNK_MIB", std::size_t{1} << 20u, std::size_t{64} << 20u), DrawSnapshotArena::SegmentGranule)};
    return bytes;
}

std::atomic<std::size_t>& BudgetSetting() {
    static std::atomic<std::size_t> bytes{EnvironmentBytes("APS5_DRAW_SNAPSHOT_ARENA_MIB", std::size_t{1} << 20u, std::size_t{1024} << 20u)};
    return bytes;
}

std::atomic<std::size_t>& ExtendSetting() {
    static std::atomic<std::size_t> bytes{EnvironmentBytes("APS5_DRAW_SNAPSHOT_ARENA_EXTEND_KIB", 1024, 0)};
    return bytes;
}

std::atomic<bool>& VerifyFlag() {
    static std::atomic<bool> flag{std::getenv("APS5_VERIFY_DRAW_SNAPSHOT_ARENA") != nullptr};
    return flag;
}

std::atomic<std::uint64_t> nextIdentity{1};
std::atomic<DrawSnapshotArena*> activeArena{nullptr};

constexpr VkDeviceSize PlacementAlignment = 256;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

}

bool DrawSnapshotArena::Enabled() {
    return EnabledFlag().load(std::memory_order_relaxed);
}

void DrawSnapshotArena::SetEnabled(bool enabled) {
    EnabledFlag().store(enabled, std::memory_order_relaxed);
}

std::size_t DrawSnapshotArena::ChunkBytes() {
    return ChunkSetting().load(std::memory_order_relaxed);
}

void DrawSnapshotArena::SetChunkBytes(std::size_t bytes) {
    ChunkSetting().store(std::max(bytes, SegmentGranule), std::memory_order_relaxed);
}

std::size_t DrawSnapshotArena::BudgetBytes() {
    return BudgetSetting().load(std::memory_order_relaxed);
}

void DrawSnapshotArena::SetBudgetBytes(std::size_t bytes) {
    BudgetSetting().store(bytes, std::memory_order_relaxed);
}

std::size_t DrawSnapshotArena::ExtendBytes() {
    return ExtendSetting().load(std::memory_order_relaxed);
}

void DrawSnapshotArena::SetExtendBytes(std::size_t bytes) {
    ExtendSetting().store(bytes, std::memory_order_relaxed);
}

bool DrawSnapshotArena::Verify() {
    return VerifyFlag().load(std::memory_order_relaxed);
}

void DrawSnapshotArena::SetVerify(bool verify) {
    VerifyFlag().store(verify, std::memory_order_relaxed);
}

DrawSnapshotArena::DrawSnapshotArena(const Context& context) : context(context), alignment(std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4)), identity(nextIdentity.fetch_add(1, std::memory_order_relaxed)) {
    Require((alignment & (alignment - 1)) == 0 && alignment <= PlacementAlignment, "the storage buffer offset alignment does not divide the arena placement alignment");
    activeArena.store(this, std::memory_order_release);
}

DrawSnapshotArena::~DrawSnapshotArena() {
    auto expected = this;
    activeArena.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void DrawSnapshotArena::ReportActive() {
    auto* arena = activeArena.load(std::memory_order_acquire);
    if (arena == nullptr || !Enabled()) return;
    const auto taken = arena->TakeCounters();
    const auto takes = taken.takes != 0 ? static_cast<double>(taken.takes) : 1.0;
    std::size_t heldBytes = 0;
    std::size_t busy = 0;
    for (const auto& chunk : arena->chunks) {
        heldBytes += chunk->buffer->Bytes().size();
        if (chunk.use_count() > 1) ++busy;
    }
    std::fprintf(stderr, "[draw-arena] (10 s) %llu takes (%.1f MiB requested): %llu hits (%.1f%%), %llu stale, %llu copies (%.1f MiB copied, %llu exact, %llu unindexed); refused: size %llu, budget %llu; chunks %zu (%zu in flight, %.0f MiB, %llu created, %llu recycled), %zu entries; verify %llu checked, %llu differing\n", static_cast<unsigned long long>(taken.takes), static_cast<double>(taken.requestedBytes) / 1048576.0, static_cast<unsigned long long>(taken.hits), 100.0 * static_cast<double>(taken.hits) / takes, static_cast<unsigned long long>(taken.stale), static_cast<unsigned long long>(taken.copies), static_cast<double>(taken.copiedBytes) / 1048576.0, static_cast<unsigned long long>(taken.exactSegments), static_cast<unsigned long long>(taken.unindexed), static_cast<unsigned long long>(taken.refusedSize), static_cast<unsigned long long>(taken.refusedBudget), arena->chunks.size(), busy, static_cast<double>(heldBytes) / 1048576.0, static_cast<unsigned long long>(taken.chunksCreated), static_cast<unsigned long long>(taken.chunksRecycled), arena->entries.size(), static_cast<unsigned long long>(taken.verified), static_cast<unsigned long long>(taken.verifyDiffering));
}

DrawSnapshotArena::Counters DrawSnapshotArena::TakeCounters() {
    const auto taken = counters;
    counters = {};
    return taken;
}

std::size_t DrawSnapshotArena::BusyChunks() const {
    return static_cast<std::size_t>(std::count_if(chunks.begin(), chunks.end(), [](const std::shared_ptr<Chunk>& chunk) { return chunk.use_count() > 1; }));
}

std::optional<std::size_t> DrawSnapshotArena::reusableChunk() const {
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        if (chunks[index].use_count() == 1) return index;
    }
    return std::nullopt;
}

std::optional<DrawSnapshotArena::Placement> DrawSnapshotArena::allocate(std::size_t bytes) {
    const auto aligned = AlignUp(bytes, PlacementAlignment);
    if (current < chunks.size()) {
        auto& chunk = *chunks[current];
        if (chunk.used + aligned <= chunk.buffer->Bytes().size()) {
            const Placement placement{current, chunk.used};
            chunk.used += aligned;
            return placement;
        }
    }
    const auto chunkBytes = std::max<std::size_t>(ChunkBytes(), aligned);
    if (const auto reusable = reusableChunk(); reusable.has_value() && chunks[*reusable]->buffer->Bytes().size() >= aligned) {
        auto& chunk = *chunks[*reusable];
        chunk.used = aligned;
        ++chunk.serial;
        current = *reusable;
        ++counters.chunksRecycled;
        return Placement{current, 0};
    }
    if (createdBytes + chunkBytes > BudgetBytes()) return std::nullopt;
    auto chunk = std::make_shared<Chunk>();
    chunk->buffer = std::make_shared<Buffer>(context, chunkBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    chunk->used = aligned;
    chunks.push_back(std::move(chunk));
    createdBytes += chunkBytes;
    current = chunks.size() - 1;
    ++counters.chunksCreated;
    return Placement{current, 0};
}

DrawSnapshotArena::View DrawSnapshotArena::view(Recorder& recorder, std::size_t chunk, VkDeviceSize offset, std::size_t bytes, bool copied, std::size_t chargedBytes) {
    const auto& held = chunks[chunk];
    recorder.KeepOnce(held, chargedBytes);
    const auto all = held->buffer->Bytes();
    return View{held->buffer->Handle(), offset, std::span<const std::byte>(all.data() + offset, bytes), copied};
}

std::optional<DrawSnapshotArena::View> DrawSnapshotArena::lookup(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration) {
    auto found = entries.upper_bound(address);
    for (std::size_t depth = 0; depth < LookupDepth && found != entries.begin(); ++depth) {
        --found;
        const auto begin = found->first;
        const auto& entry = found->second;
        if (entry.end < address + bytes || (address - begin) % alignment != 0) continue;
        if (entry.chunkSerial != chunks[entry.chunk]->serial || entry.registryGeneration != registryGeneration) {
            found = entries.erase(found);
            continue;
        }
        if (GuestMemory::CollectWrites(begin, static_cast<std::size_t>(entry.end - begin)) == 0 || !GuestMemory::UnchangedSince(address, bytes, entry.generation)) {
            ++counters.stale;
            continue;
        }
        auto result = view(recorder, entry.chunk, entry.offset + (address - begin), bytes, false, 0);
        ++counters.hits;
        if (Verify()) {
            ++counters.verified;
            if (std::memcmp(result.bytes.data(), reinterpret_cast<const void*>(address), bytes) != 0) ++counters.verifyDiffering;
        }
        return result;
    }
    return std::nullopt;
}

std::optional<DrawSnapshotArena::View> DrawSnapshotArena::Take(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration) {
    ++counters.takes;
    counters.requestedBytes += bytes;
    if (bytes == 0 || bytes > ChunkBytes()) {
        ++counters.refusedSize;
        return std::nullopt;
    }
    if (auto hit = lookup(recorder, address, bytes, registryGeneration)) return hit;
    auto first = address;
    auto last = address + bytes;
    std::uint64_t segmentGeneration = 0;
    const auto widenedFirst = address - (address % SegmentGranule) / alignment * alignment;
    const auto widenedLast = AlignUp(address + bytes, SegmentGranule) + ExtendBytes();
    const auto widenedBytes = static_cast<std::size_t>(widenedLast - widenedFirst);
    if (widenedBytes <= ChunkBytes() && GuestMemory::Accessible(reinterpret_cast<const void*>(widenedFirst), widenedBytes)) {
        if (const auto widened = GuestMemory::CollectWrites(widenedFirst, widenedBytes); widened != 0) {
            first = widenedFirst;
            last = widenedLast;
            segmentGeneration = widened;
        }
    }
    if (segmentGeneration == 0) segmentGeneration = GuestMemory::CollectWrites(address, bytes);
    if (first == address && last == address + bytes) ++counters.exactSegments;
    const auto segmentBytes = static_cast<std::size_t>(last - first);
    const auto placement = allocate(segmentBytes);
    if (!placement.has_value()) {
        ++counters.refusedBudget;
        return std::nullopt;
    }
    auto& chunk = *chunks[placement->chunk];
    std::memcpy(chunk.buffer->Bytes().data() + placement->offset, reinterpret_cast<const void*>(first), segmentBytes);
    ++counters.copies;
    counters.copiedBytes += segmentBytes;
    if (segmentGeneration != 0) {
        if (entries.size() >= MaxEntries) entries.clear();
        entries[first] = Entry{last, segmentGeneration, registryGeneration, placement->chunk, chunk.serial, placement->offset};
    } else {
        ++counters.unindexed;
    }
    return view(recorder, placement->chunk, placement->offset + (address - first), bytes, true, segmentBytes);
}

}

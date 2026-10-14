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
    static std::atomic<std::size_t> bytes{EnvironmentBytes("APS5_DRAW_SNAPSHOT_ARENA_EXTEND_KIB", 1024, std::size_t{1} << 20u)};
    return bytes;
}

std::atomic<bool>& VerifyFlag() {
    static std::atomic<bool> flag{std::getenv("APS5_VERIFY_DRAW_SNAPSHOT_ARENA") != nullptr};
    return flag;
}

std::atomic<bool>& DeviceTwinsFlag() {
    static std::atomic<bool> flag{std::getenv("APS5_ARENA_DEVICE_TWINS") != nullptr && std::strcmp(std::getenv("APS5_ARENA_DEVICE_TWINS"), "0") != 0};
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

bool DrawSnapshotArena::DeviceTwins() {
    return DeviceTwinsFlag().load(std::memory_order_relaxed);
}

void DrawSnapshotArena::SetDeviceTwins(bool twins) {
    DeviceTwinsFlag().store(twins, std::memory_order_relaxed);
}

DrawSnapshotArena::DrawSnapshotArena(const Context& context) : context(context), alignment(std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4)), identity(nextIdentity.fetch_add(1, std::memory_order_relaxed)) {
    Require((alignment & (alignment - 1)) == 0 && alignment <= PlacementAlignment, "the storage buffer offset alignment does not divide the arena placement alignment");
    activeArena.store(this, std::memory_order_release);
}

DrawSnapshotArena::~DrawSnapshotArena() {
    auto expected = this;
    activeArena.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void DrawSnapshotArena::ReportActive(double elapsedSeconds) {
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
    std::fprintf(stderr, "[draw-arena] (%.3f s) %llu takes (%.1f MiB requested): %llu hits (%.1f%%), %llu stale, %llu copies (%.1f MiB copied, %llu exact, %llu unindexed); partial %llu copies (%.1f MiB left uncopied); refused: size %llu, budget %llu; chunks %zu (%zu in flight, %.0f MiB, %llu created, %llu recycled), %zu entries; verify %llu checked, %llu differing; device twins%s: %llu views, %llu prologue copies (%.1f MiB), %llu refused\n", elapsedSeconds, static_cast<unsigned long long>(taken.takes), static_cast<double>(taken.requestedBytes) / 1048576.0, static_cast<unsigned long long>(taken.hits), 100.0 * static_cast<double>(taken.hits) / takes, static_cast<unsigned long long>(taken.stale), static_cast<unsigned long long>(taken.copies), static_cast<double>(taken.copiedBytes) / 1048576.0, static_cast<unsigned long long>(taken.exactSegments), static_cast<unsigned long long>(taken.unindexed), static_cast<unsigned long long>(taken.partialCopies), static_cast<double>(taken.uncopiedBytes) / 1048576.0, static_cast<unsigned long long>(taken.refusedSize), static_cast<unsigned long long>(taken.refusedBudget), arena->chunks.size(), busy, static_cast<double>(heldBytes) / 1048576.0, static_cast<unsigned long long>(taken.chunksCreated), static_cast<unsigned long long>(taken.chunksRecycled), arena->entries.size(), static_cast<unsigned long long>(taken.verified), static_cast<unsigned long long>(taken.verifyDiffering), DeviceTwins() ? "" : " (off)", static_cast<unsigned long long>(taken.twinViews), static_cast<unsigned long long>(taken.twinCopies), static_cast<double>(taken.twinCopiedBytes) / 1048576.0, static_cast<unsigned long long>(taken.twinsRefused));
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
        chunk.pending.clear();
        chunk.containsPartial = false;
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

void DrawSnapshotArena::settleTwin(Recorder& recorder, Chunk& chunk) {
    if (chunk.twin == nullptr) {
        if (chunk.twinRefused) return;
        try {
            chunk.twin = std::make_shared<Buffer>(context, chunk.buffer->Size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        } catch (const std::exception&) {
            chunk.twinRefused = true;
            ++counters.twinsRefused;
            return;
        }
        chunk.pending.assign(1, VkBufferCopy{0, 0, chunk.used});
    }
    if (chunk.pending.empty()) return;
    context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(recorder.PrologueCommands(), chunk.buffer->Handle(), chunk.twin->Handle(), static_cast<std::uint32_t>(chunk.pending.size()), chunk.pending.data());
    counters.twinCopies += chunk.pending.size();
    for (const auto& copy : chunk.pending) counters.twinCopiedBytes += copy.size;
    chunk.pending.clear();
}

DrawSnapshotArena::View DrawSnapshotArena::view(Recorder& recorder, std::size_t chunk, VkDeviceSize offset, std::size_t bytes, bool copied, std::size_t chargedBytes, std::size_t copiedBytes, bool limited) {
    const auto& held = chunks[chunk];
    recorder.KeepOnce(held, chargedBytes);
    const auto all = held->buffer->Bytes();
    if (DeviceTwins() && !held->containsPartial) {
        settleTwin(recorder, *held);
        if (held->twin != nullptr) {
            ++counters.twinViews;
            return View{held->twin->Handle(), offset, std::span<const std::byte>(all.data() + offset, bytes), copied, copiedBytes, limited};
        }
    }
    return View{held->buffer->Handle(), offset, std::span<const std::byte>(all.data() + offset, bytes), copied, copiedBytes, limited};
}

std::optional<DrawSnapshotArena::View> DrawSnapshotArena::lookup(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, const CopyRanges* ranges) {
    auto found = entries.upper_bound(address);
    for (std::size_t depth = 0; depth < LookupDepth && found != entries.begin(); ++depth) {
        --found;
        const auto begin = found->first;
        auto& entry = found->second;
        if (entry.end < address + bytes || (address - begin) % alignment != 0) continue;
        if (entry.chunkSerial != chunks[entry.chunk]->serial || entry.registryGeneration != registryGeneration) {
            found = entries.erase(found);
            continue;
        }
        if (entry.limited != (ranges != nullptr)) continue;
        if (ranges != nullptr) {
            auto covered = entry.copiedRanges.begin();
            const auto shift = address - begin;
            const bool serves = std::all_of(ranges->begin(), ranges->end(), [&](const auto& range) {
                if (range.first == range.second) return true;
                while (covered != entry.copiedRanges.end() && covered->second <= shift + range.first) ++covered;
                return covered != entry.copiedRanges.end() && covered->first <= shift + range.first && covered->second >= shift + range.second;
            });
            if (!serves) continue;
        }
        if (GuestMemory::CollectWrites(begin, static_cast<std::size_t>(entry.end - begin)) == 0) {
            ++counters.stale;
            continue;
        }
        bool unchanged = false;
        if (entry.dirtySerial != 0 && GuestMemory::EpochDirtySetEnabled()) {
            std::uint64_t through = 0;
            const auto ownedBytes = static_cast<std::size_t>(entry.end - begin);
            unchanged = GuestMemory::UnchangedSinceDirtySerial(begin, ownedBytes, entry.dirtySerial, &through);
            if (unchanged && GuestMemory::EpochDirtySetVerify()) {
                const bool slow = GuestMemory::UnchangedSince(begin, ownedBytes, entry.generation);
                GuestMemory::NoteDirtySetVerify(slow, {"snapshot-arena", entry.dirtySerial, through, entry.generation}, begin, ownedBytes);
                unchanged = slow;
            }
            if (unchanged) entry.dirtySerial = through;
        }
        const auto unchangedReads = [&] {
            if (ranges == nullptr) return GuestMemory::UnchangedSince(address, bytes, entry.generation);
            return std::all_of(ranges->begin(), ranges->end(), [&](const auto& range) { return range.first == range.second || GuestMemory::UnchangedSince(address + range.first, static_cast<std::size_t>(range.second - range.first), entry.generation); });
        };
        if (!unchanged && !unchangedReads()) {
            ++counters.stale;
            continue;
        }
        auto result = view(recorder, entry.chunk, entry.offset + (address - begin), bytes, false, 0, 0, entry.limited);
        ++counters.hits;
        if (Verify()) {
            ++counters.verified;
            const bool same = ranges == nullptr ? std::memcmp(result.bytes.data(), reinterpret_cast<const void*>(address), bytes) == 0 : std::all_of(ranges->begin(), ranges->end(), [&](const auto& range) { return std::memcmp(result.bytes.data() + range.first, reinterpret_cast<const void*>(address + range.first), static_cast<std::size_t>(range.second - range.first)) == 0; });
            if (!same) ++counters.verifyDiffering;
        }
        return result;
    }
    return std::nullopt;
}

std::optional<DrawSnapshotArena::View> DrawSnapshotArena::Take(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration) {
    return Take(recorder, address, bytes, registryGeneration, [] { return std::optional<CopyRanges>{}; });
}

bool DrawSnapshotArena::beginTake(std::size_t bytes) {
    ++counters.takes;
    counters.requestedBytes += bytes;
    if (bytes == 0 || bytes > ChunkBytes()) {
        ++counters.refusedSize;
        return false;
    }
    return true;
}

std::optional<DrawSnapshotArena::View> DrawSnapshotArena::copy(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, std::optional<CopyRanges> ranges) {
    const bool limited = ranges.has_value();
    std::size_t copiedBytes = 0;
    if (limited) {
        std::uint64_t previous = 0;
        for (const auto& [first, last] : *ranges) {
            Require(first >= previous && first <= last && last <= bytes, "draw snapshot read ranges are invalid");
            copiedBytes += static_cast<std::size_t>(last - first);
            previous = last;
        }
        if (DeviceTwins()) return std::nullopt;
        if (auto hit = lookup(recorder, address, bytes, registryGeneration, &*ranges)) return hit;
    }
    auto first = address;
    auto last = address + bytes;
    std::uint64_t segmentGeneration = 0;
    const auto dirtySerial = GuestMemory::EpochDirtySetEnabled() ? GuestMemory::DirtySerial() : 0;
    const auto widenedFirst = address - (address % SegmentGranule) / alignment * alignment;
    const auto widenedLast = AlignUp(address + bytes, SegmentGranule) + ExtendBytes();
    const auto widenedBytes = static_cast<std::size_t>(widenedLast - widenedFirst);
    if (!limited && widenedBytes <= ChunkBytes() && GuestMemory::Accessible(reinterpret_cast<const void*>(widenedFirst), widenedBytes)) {
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
    if (limited) {
        chunk.containsPartial = true;
        for (const auto& [from, to] : *ranges) std::memcpy(chunk.buffer->Bytes().data() + placement->offset + from, reinterpret_cast<const void*>(address + from), static_cast<std::size_t>(to - from));
        ++counters.partialCopies;
        counters.uncopiedBytes += bytes - copiedBytes;
    } else {
        std::memcpy(chunk.buffer->Bytes().data() + placement->offset, reinterpret_cast<const void*>(first), segmentBytes);
        copiedBytes = segmentBytes;
        if (chunk.twin != nullptr && !chunk.containsPartial) chunk.pending.push_back({placement->offset, placement->offset, segmentBytes});
    }
    ++counters.copies;
    counters.copiedBytes += copiedBytes;
    if (segmentGeneration != 0) {
        if (entries.size() >= MaxEntries) entries.clear();
        Entry entry{last, segmentGeneration, registryGeneration, placement->chunk, chunk.serial, placement->offset, dirtySerial};
        if (limited) {
            entry.copiedRanges = std::move(*ranges);
            entry.limited = true;
        }
        entries[first] = std::move(entry);
    } else {
        ++counters.unindexed;
    }
    return view(recorder, placement->chunk, placement->offset + (address - first), bytes, true, segmentBytes, copiedBytes, limited);
}

}

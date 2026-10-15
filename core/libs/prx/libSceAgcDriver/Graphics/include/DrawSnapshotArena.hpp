#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSNAPSHOTARENA_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSNAPSHOTARENA_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class Buffer;
class Recorder;

class DrawSnapshotArena {
public:
    using CopyRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    struct View {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        std::span<const std::byte> bytes;
        bool copied = false;
        std::size_t copiedBytes = 0;
        bool limited = false;
    };
    struct Counters {
        std::uint64_t takes = 0;
        std::uint64_t hits = 0;
        std::uint64_t stale = 0;
        std::uint64_t copies = 0;
        std::uint64_t copiedBytes = 0;
        std::uint64_t partialCopies = 0;
        std::uint64_t uncopiedBytes = 0;
        std::uint64_t requestedBytes = 0;
        std::uint64_t unindexed = 0;
        std::uint64_t exactSegments = 0;
        std::uint64_t refusedSize = 0;
        std::uint64_t refusedBudget = 0;
        std::uint64_t chunksCreated = 0;
        std::uint64_t chunksRecycled = 0;
        std::uint64_t verified = 0;
        std::uint64_t verifyDiffering = 0;
        std::uint64_t twinCopies = 0;
        std::uint64_t twinCopiedBytes = 0;
        std::uint64_t twinViews = 0;
        std::uint64_t twinsRefused = 0;
        std::uint64_t extendedCopies = 0;
        std::uint64_t placed = 0;
        std::uint64_t placedBytes = 0;
        std::uint64_t placedReuses = 0;
        std::uint64_t placeRefused = 0;
        std::uint64_t prefilled = 0;
        std::uint64_t prefilledBytes = 0;
        std::uint64_t prefillsServed = 0;
        std::uint64_t adopted = 0;
        std::uint64_t adoptRefused = 0;
        std::uint64_t workerHits = 0;
        std::uint64_t workerHitsVerified = 0;
    };
    struct Prefill {
        std::shared_ptr<const void> pin;
        std::uint64_t arena = 0;
        std::size_t chunk = 0;
        std::uint64_t serial = 0;
        VkDeviceSize offset = 0;
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        std::uint64_t address = 0;
        std::size_t bytes = 0;
        std::uint64_t generation = 0;
        std::uint64_t registryGeneration = 0;
        std::uint64_t dirtySerial = 0;
        std::optional<CopyRanges> ranges;
        std::size_t copiedBytes = 0;
        bool hit = false;
        bool limited = false;
    };
    struct Placed {
        std::uint64_t arena = 0;
        std::size_t chunk = 0;
        std::uint64_t serial = 0;
        VkDeviceSize offset = 0;
        std::size_t bytes = 0;
    };

    explicit DrawSnapshotArena(const Context& context);
    ~DrawSnapshotArena();
    DrawSnapshotArena(const DrawSnapshotArena&) = delete;
    DrawSnapshotArena& operator=(const DrawSnapshotArena&) = delete;

    std::optional<View> Take(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration);
    template <class RangeProvider>
    std::optional<View> Take(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, RangeProvider&& ranges) {
        std::lock_guard lock(mutex);
        if (!beginTake(bytes)) return std::nullopt;
        if (auto hit = lookup(recorder, address, bytes, registryGeneration)) return hit;
        return copy(recorder, address, bytes, registryGeneration, ranges());
    }
    template <class RangeProvider>
    std::optional<View> TakePrefilled(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, RangeProvider&& ranges, const Prefill& prefill) {
        std::lock_guard lock(mutex);
        if (!beginTake(bytes)) return std::nullopt;
        if (prefill.hit) {
            if (auto hit = adoptHit(recorder, prefill, address, bytes, registryGeneration)) return hit;
        }
        if (auto hit = lookup(recorder, address, bytes, registryGeneration)) return hit;
        auto wanted = ranges();
        if (prefill.address == address && prefill.bytes == bytes && wanted == prefill.ranges) {
            if (auto adopted = adopt(recorder, prefill, registryGeneration)) return adopted;
        }
        return copy(recorder, address, bytes, registryGeneration, std::move(wanted));
    }
    std::optional<Prefill> PrepareCopy(std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, std::optional<CopyRanges> ranges);
    template <class Fill>
    std::optional<View> Place(Recorder& recorder, std::size_t bytes, Fill&& fill, Placed* placed) {
        std::lock_guard lock(mutex);
        const auto target = reserve(bytes, placed);
        if (!target) return std::nullopt;
        fill(*target);
        return view(recorder, placed->chunk, placed->offset, placed->bytes, true, placed->bytes, placed->bytes);
    }
    std::optional<View> Reuse(Recorder& recorder, const Placed& placed);
    Counters TakeCounters();
    std::size_t ChunkCount() const {
        std::lock_guard lock(mutex);
        return chunks.size();
    }
    std::size_t EntryCount() const {
        std::lock_guard lock(mutex);
        return entries.size();
    }
    std::size_t BusyChunks() const;
    std::uint64_t Identity() const { return identity; }

    static void ReportActive(double elapsedSeconds);
    static bool Enabled();
    static void SetEnabled(bool enabled);
    static std::size_t ChunkBytes();
    static void SetChunkBytes(std::size_t bytes);
    static std::size_t BudgetBytes();
    static void SetBudgetBytes(std::size_t bytes);
    static std::size_t ExtendBytes();
    static void SetExtendBytes(std::size_t bytes);
    static bool ParseWorkerHits(const char* text);
    static bool WorkerHits();
    static bool VerifyWorkerHits();
    static void SetWorkerHits(bool enabled);
    static bool Verify();
    static void SetVerify(bool verify);
    static bool DeviceTwins();
    static void SetDeviceTwins(bool twins);
    static bool WorkerCopies();
    static void SetWorkerCopies(bool enabled);
    static bool VerifyWorkerCopies();
    static constexpr std::size_t SegmentGranule = 4096;
    static constexpr std::size_t MaxEntries = 16384;
    static constexpr std::size_t LookupDepth = 8;
    static constexpr std::size_t MaxCopiedRanges = 64;

private:
    struct Chunk {
        std::shared_ptr<Buffer> buffer;
        std::shared_ptr<Buffer> twin;
        bool twinRefused = false;
        bool containsPartial = false;
        std::vector<VkBufferCopy> pending;
        std::size_t used = 0;
        std::uint64_t serial = 0;
    };
    struct Entry {
        std::uint64_t end = 0;
        std::uint64_t generation = 0;
        std::uint64_t registryGeneration = 0;
        std::size_t chunk = 0;
        std::uint64_t chunkSerial = 0;
        VkDeviceSize offset = 0;
        std::uint64_t dirtySerial = 0;
        CopyRanges copiedRanges;
        bool limited = false;
    };
    struct Placement {
        std::size_t chunk;
        VkDeviceSize offset;
    };
    std::optional<Placement> allocate(std::size_t bytes);
    bool beginTake(std::size_t bytes);
    std::optional<std::span<std::byte>> reserve(std::size_t bytes, Placed* placed);
    std::optional<View> copy(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, std::optional<CopyRanges> ranges);
    void settleTwin(Recorder& recorder, Chunk& chunk);
    std::optional<std::size_t> reusableChunk() const;
    View view(Recorder& recorder, std::size_t chunk, VkDeviceSize offset, std::size_t bytes, bool copied, std::size_t chargedBytes, std::size_t copiedBytes = 0, bool limited = false);
    struct Served {
        std::size_t chunk = 0;
        std::uint64_t serial = 0;
        VkDeviceSize offset = 0;
        bool limited = false;
    };
    std::optional<Served> servedEntry(std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, const CopyRanges* ranges) const;
    std::optional<View> adoptHit(Recorder& recorder, const Prefill& prefill, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration);
    std::optional<View> lookup(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, const CopyRanges* ranges = nullptr);
    std::optional<View> extend(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, const CopyRanges& ranges);
    std::optional<View> adopt(Recorder& recorder, const Prefill& prefill, std::uint64_t registryGeneration);
    bool served(std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration, const CopyRanges* ranges) const;
    mutable std::mutex mutex;

    Context context;
    VkDeviceSize alignment;
    std::uint64_t identity;
    std::vector<std::shared_ptr<Chunk>> chunks;
    std::size_t current = 0;
    std::size_t createdBytes = 0;
    std::map<std::uint64_t, Entry> entries;
    Counters counters;
};

}

#endif

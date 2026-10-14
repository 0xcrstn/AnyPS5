#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSNAPSHOTARENA_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSNAPSHOTARENA_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

class Buffer;
class Recorder;

class DrawSnapshotArena {
public:
    struct View {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        std::span<const std::byte> bytes;
        bool copied = false;
    };
    struct Counters {
        std::uint64_t takes = 0;
        std::uint64_t hits = 0;
        std::uint64_t stale = 0;
        std::uint64_t copies = 0;
        std::uint64_t copiedBytes = 0;
        std::uint64_t requestedBytes = 0;
        std::uint64_t unindexed = 0;
        std::uint64_t exactSegments = 0;
        std::uint64_t refusedSize = 0;
        std::uint64_t refusedBudget = 0;
        std::uint64_t chunksCreated = 0;
        std::uint64_t chunksRecycled = 0;
        std::uint64_t verified = 0;
        std::uint64_t verifyDiffering = 0;
    };

    explicit DrawSnapshotArena(const Context& context);
    ~DrawSnapshotArena();
    DrawSnapshotArena(const DrawSnapshotArena&) = delete;
    DrawSnapshotArena& operator=(const DrawSnapshotArena&) = delete;

    std::optional<View> Take(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration);
    Counters TakeCounters();
    std::size_t ChunkCount() const { return chunks.size(); }
    std::size_t EntryCount() const { return entries.size(); }
    std::size_t BusyChunks() const;
    std::uint64_t Identity() const { return identity; }

    static void ReportActive();
    static bool Enabled();
    static void SetEnabled(bool enabled);
    static std::size_t ChunkBytes();
    static void SetChunkBytes(std::size_t bytes);
    static std::size_t BudgetBytes();
    static void SetBudgetBytes(std::size_t bytes);
    static std::size_t ExtendBytes();
    static void SetExtendBytes(std::size_t bytes);
    static bool Verify();
    static void SetVerify(bool verify);
    static constexpr std::size_t SegmentGranule = 4096;
    static constexpr std::size_t MaxEntries = 16384;
    static constexpr std::size_t LookupDepth = 8;

private:
    struct Chunk {
        std::shared_ptr<Buffer> buffer;
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
    };
    struct Placement {
        std::size_t chunk;
        VkDeviceSize offset;
    };
    std::optional<Placement> allocate(std::size_t bytes);
    std::optional<std::size_t> reusableChunk() const;
    View view(Recorder& recorder, std::size_t chunk, VkDeviceSize offset, std::size_t bytes, bool copied, std::size_t chargedBytes);
    std::optional<View> lookup(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint64_t registryGeneration);

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

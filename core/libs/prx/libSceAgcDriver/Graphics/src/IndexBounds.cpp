#include "prx/libSceAgcDriver/Graphics/include/IndexBounds.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <tuple>

namespace AgcDriver::Graphics {

namespace {

struct Key {
    std::uint64_t address;
    std::size_t bytes;
    std::uint32_t indexSize;
    bool skipRestart;

    bool operator<(const Key& other) const { return std::tie(address, bytes, indexSize, skipRestart) < std::tie(other.address, other.bytes, other.indexSize, other.skipRestart); }
};

struct Entry {
    std::uint64_t generation;
    std::uint64_t registryGeneration;
    std::list<Key>::iterator recent;
    std::optional<std::uint32_t> lowest;
};

constexpr std::size_t Entries = 4096;

struct Cache {
    std::mutex mutex;
    std::map<Key, Entry> entries;
    std::list<Key> recency;
    LowestIndexCounters counters;
};

Cache& TheCache() {
    static Cache cache;
    return cache;
}

}

std::optional<std::uint32_t> LowestDrawIndex(std::span<const std::byte> indices, std::uint32_t indexSize, bool skipRestart) {
    Require(indexSize == 2 || indexSize == 4, "unsupported index size");
    const auto restartIndex = indexSize == 2 ? 0xffffu : 0xffffffffu;
    std::optional<std::uint32_t> lowest;
    for (std::size_t offset = 0; offset + indexSize <= indices.size(); offset += indexSize) {
        std::uint32_t index = 0;
        if (indexSize == 2) {
            std::uint16_t value = 0;
            std::memcpy(&value, indices.data() + offset, sizeof(value));
            index = value;
        } else {
            std::memcpy(&index, indices.data() + offset, sizeof(index));
        }
        if (skipRestart && index == restartIndex) continue;
        lowest = std::min(lowest.value_or(index), index);
    }
    return lowest;
}

std::optional<std::uint32_t> KnownLowestDrawIndex(std::uint64_t address, std::span<const std::byte> indices, std::uint32_t indexSize, bool skipRestart, std::uint64_t generation) {
    auto& cache = TheCache();
    const Key key{address, indices.size(), indexSize, skipRestart};
    const auto registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    if (generation != 0) {
        std::lock_guard lock(cache.mutex);
        if (const auto found = cache.entries.find(key); found != cache.entries.end()) {
            if (found->second.registryGeneration == registryGeneration && GuestMemory::UnchangedSince(address, indices.size(), found->second.generation)) {
                cache.recency.splice(cache.recency.end(), cache.recency, found->second.recent);
                ++cache.counters.hits;
                return found->second.lowest;
            }
            cache.recency.erase(found->second.recent);
            cache.entries.erase(found);
        }
    }
    const auto lowest = LowestDrawIndex(indices, indexSize, skipRestart);
    std::lock_guard lock(cache.mutex);
    ++cache.counters.scans;
    if (generation == 0 || cache.entries.contains(key)) return lowest;
    while (cache.entries.size() >= Entries) {
        cache.entries.erase(cache.recency.front());
        cache.recency.pop_front();
    }
    cache.recency.push_back(key);
    cache.entries.emplace(key, Entry{generation, registryGeneration, std::prev(cache.recency.end()), lowest});
    return lowest;
}

LowestIndexCounters TakeLowestIndexCounters() {
    auto& cache = TheCache();
    std::lock_guard lock(cache.mutex);
    const auto taken = cache.counters;
    cache.counters = {};
    return taken;
}

}

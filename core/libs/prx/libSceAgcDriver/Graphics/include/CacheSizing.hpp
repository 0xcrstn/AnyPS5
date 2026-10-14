#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CACHESIZING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CACHESIZING_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {

class EvictionHistory {
public:
    explicit EvictionHistory(std::size_t keys);
    void Evicted(std::uint64_t key);
    std::optional<std::uint64_t> Missed(std::uint64_t key);
    void Resize(std::size_t keys);
    std::size_t Capacity() const { return ring.size(); }
    std::size_t Size() const { return sequences.size(); }

private:
    struct Slot {
        std::uint64_t key = 0;
        std::uint64_t sequence = 0;
        bool used = false;
    };
    void forgetSlot(const Slot& slot);
    std::vector<Slot> ring;
    std::unordered_map<std::uint64_t, std::uint64_t> sequences;
    std::uint64_t next = 0;
};

class CacheSizing {
public:
    struct Limits {
        std::size_t initialEntries = 1024;
        std::size_t maxEntries = 1024;
        std::uint64_t maxBytes = ~std::uint64_t{0};
        bool adaptive = true;
    };
    struct Window {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::array<std::uint64_t, 3> evictedWithin{};
        std::uint64_t evictions = 0;
        std::uint64_t growths = 0;
    };
    static constexpr std::size_t HistoryFactor = 8;
    static constexpr std::size_t MaxHistoryKeys = std::size_t{1} << 20u;
    static constexpr std::size_t MinGrowthWindow = 256;
    static constexpr std::uint64_t MinGrowthHits = 32;

    explicit CacheSizing(Limits limits);
    static Limits FromEnvironment(const std::string& prefix, Limits defaults);
    static std::uint64_t KeyHash(const std::uint32_t* words, std::size_t count);

    std::size_t Capacity() const { return capacity; }
    const Limits& Configured() const { return limits; }
    bool OverLimit(std::size_t entries, std::uint64_t bytes) const { return entries > capacity || (entries > 1 && bytes > limits.maxBytes); }
    void Hit() { ++window.hits; }
    void Miss(std::uint64_t key, std::size_t entries, std::uint64_t bytes);
    void Evicted(std::uint64_t key);
    Window TakeWindow();
    std::string Describe(const char* name, std::size_t entries, std::uint64_t bytes);

private:
    void considerGrowth(std::size_t entries, std::uint64_t bytes);
    Limits limits;
    std::size_t capacity;
    EvictionHistory history;
    Window window;
    std::uint64_t growthMisses = 0;
    std::uint64_t growthNear = 0;
};

}

#endif

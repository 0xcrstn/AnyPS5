#include "prx/libSceAgcDriver/Graphics/include/CacheSizing.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

class SimulatedCache {
public:
    SimulatedCache(CacheSizing::Limits limits, std::uint64_t entryBytes) : sizing(limits), entryBytes(entryBytes) {}

    bool Access(std::uint64_t key) {
        if (const auto found = index.find(key); found != index.end()) {
            sizing.Hit();
            order.splice(order.begin(), order, found->second);
            return true;
        }
        sizing.Miss(key, order.size(), order.size() * entryBytes);
        order.push_front(key);
        index[key] = order.begin();
        while (sizing.OverLimit(order.size(), order.size() * entryBytes)) {
            sizing.Evicted(order.back());
            index.erase(order.back());
            order.pop_back();
        }
        return false;
    }

    CacheSizing sizing;
    std::uint64_t entryBytes;
    std::list<std::uint64_t> order;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> index;
};

void EvictionHistoryDistances() {
    EvictionHistory history(16);
    for (std::uint64_t key = 1; key <= 5; ++key) history.Evicted(key);
    const auto distance = history.Missed(3);
    Expect(distance && *distance == 2, "a key evicted two evictions before the latest is at distance 2");
    Expect(!history.Missed(3), "a missed key is forgotten");
    Expect(!history.Missed(99), "a key never evicted has no distance");
    const auto latest = history.Missed(5);
    Expect(latest && *latest == 0, "the latest eviction is at distance 0");
}

void EvictionHistoryForgetsOldest() {
    EvictionHistory history(4);
    for (std::uint64_t key = 1; key <= 6; ++key) history.Evicted(key);
    Expect(!history.Missed(1) && !history.Missed(2), "keys beyond the history capacity are forgotten");
    Expect(history.Size() == 4, "the history holds its capacity");
    history.Evicted(7);
    history.Evicted(8);
    history.Evicted(7);
    const auto again = history.Missed(7);
    Expect(again && *again == 0, "a key evicted twice is at the distance of its latest eviction");
    for (std::uint64_t key = 20; key < 24; ++key) history.Evicted(key);
    Expect(!history.Missed(8), "overwriting a slot forgets its key");
    Expect(history.Size() == 4, "a re-evicted key is counted once");
}

void EvictionHistoryResize() {
    EvictionHistory growing(4);
    for (std::uint64_t key = 1; key <= 4; ++key) growing.Evicted(key);
    growing.Resize(8);
    for (std::uint64_t key = 5; key <= 8; ++key) growing.Evicted(key);
    const auto first = growing.Missed(1);
    Expect(first && *first == 7, "growing keeps the remembered keys and their distances");
    EvictionHistory shrinking(8);
    for (std::uint64_t key = 1; key <= 8; ++key) shrinking.Evicted(key);
    shrinking.Resize(2);
    Expect(!shrinking.Missed(6), "shrinking forgets all but the newest keys");
    const auto newest = shrinking.Missed(8);
    Expect(newest && *newest == 0, "shrinking keeps the newest key");
    Expect(shrinking.Size() == 1, "shrinking drops the forgotten keys from the index");
}

void OverLimitByEntriesAndBytes() {
    const CacheSizing sizing({4, 4, 1000, false});
    Expect(!sizing.OverLimit(4, 1000), "a cache at both bounds is within them");
    Expect(sizing.OverLimit(5, 10), "one entry over the capacity is over the limit");
    Expect(sizing.OverLimit(2, 1001), "one byte over the budget is over the limit");
    Expect(!sizing.OverLimit(1, 5000), "a single entry larger than the budget stays");
}

void GrowsForCyclicWorkingSet() {
    SimulatedCache cache({256, 4096, ~std::uint64_t{0}, true}, 64);
    std::uint64_t lastHits = 0;
    for (int round = 0; round < 20; ++round) {
        lastHits = 0;
        for (std::uint64_t key = 0; key < 384; ++key) lastHits += cache.Access(key) ? 1 : 0;
    }
    Expect(cache.sizing.Capacity() == 512, "a working set of 1.5x the capacity doubles it once, got " + std::to_string(cache.sizing.Capacity()));
    Expect(lastHits == 384, "the grown cache holds the working set");
    const auto window = cache.sizing.TakeWindow();
    Expect(window.growths == 1, "one growth is reported");
    Expect(window.evictedWithin[0] != 0 && window.evictedWithin[0] <= window.evictedWithin[1] && window.evictedWithin[1] <= window.evictedWithin[2], "reuse buckets are cumulative");
}

void NoGrowthForStreamingKeys() {
    SimulatedCache cache({256, 4096, ~std::uint64_t{0}, true}, 64);
    for (std::uint64_t key = 0; key < 20000; ++key) cache.Access(key);
    Expect(cache.sizing.Capacity() == 256, "keys that never come back do not grow the cache");
    const auto window = cache.sizing.TakeWindow();
    Expect(window.evictedWithin[2] == 0, "no miss is attributed to an evicted key");
    Expect(window.evictions == 20000 - 256, "every insert beyond the capacity evicts");
}

void NoGrowthForDistantReuse() {
    SimulatedCache cache({256, 4096, ~std::uint64_t{0}, true}, 64);
    for (int round = 0; round < 10; ++round) {
        for (std::uint64_t key = 0; key < 1200; ++key) cache.Access(key);
    }
    Expect(cache.sizing.Capacity() == 256, "reuse beyond twice the capacity does not grow the cache");
    const auto window = cache.sizing.TakeWindow();
    Expect(window.evictedWithin[0] == 0 && window.evictedWithin[2] != 0, "distant reuse lands in the 8x bucket only");
}

void GrowthBoundedByBytes() {
    SimulatedCache cache({256, 4096, 300 * 1024, true}, 1024);
    for (int round = 0; round < 20; ++round) {
        for (std::uint64_t key = 0; key < 384; ++key) cache.Access(key);
    }
    Expect(cache.sizing.Capacity() == 300, "growth stops at the byte budget, got " + std::to_string(cache.sizing.Capacity()));
    Expect(cache.order.size() * cache.entryBytes <= 300 * 1024, "the held bytes stay within the budget");
}

void GrowthBoundedByMaxEntries() {
    SimulatedCache cache({256, 300, ~std::uint64_t{0}, true}, 64);
    for (int round = 0; round < 20; ++round) {
        for (std::uint64_t key = 0; key < 384; ++key) cache.Access(key);
    }
    Expect(cache.sizing.Capacity() == 300, "growth stops at the maximum entry count");
}

void FixedNeverGrows() {
    SimulatedCache cache({256, 4096, ~std::uint64_t{0}, false}, 64);
    for (int round = 0; round < 20; ++round) {
        for (std::uint64_t key = 0; key < 384; ++key) cache.Access(key);
    }
    Expect(cache.sizing.Capacity() == 256, "a fixed cache keeps its capacity");
    Expect(cache.sizing.Configured().maxEntries == 256, "a fixed cache reports its capacity as the maximum");
}

void Environment() {
    const CacheSizing::Limits defaults{1024, 8192, std::uint64_t{64} << 20u, true};
    auto limits = CacheSizing::FromEnvironment("APS5_TEST_CACHE", defaults);
    Expect(limits.initialEntries == 1024 && limits.maxEntries == 8192 && limits.maxBytes == defaults.maxBytes && limits.adaptive, "no variables keep the defaults");
    SetEnvironment("APS5_TEST_CACHE_ENTRIES", "16384");
    limits = CacheSizing::FromEnvironment("APS5_TEST_CACHE", defaults);
    Expect(limits.initialEntries == 16384 && limits.maxEntries == 16384, "a larger initial count raises the maximum");
    SetEnvironment("APS5_TEST_CACHE_MAX_ENTRIES", "2048");
    SetEnvironment("APS5_TEST_CACHE_MIB", "3");
    SetEnvironment("APS5_TEST_CACHE_FIXED", "1");
    limits = CacheSizing::FromEnvironment("APS5_TEST_CACHE", defaults);
    Expect(limits.initialEntries == 2048 && limits.maxEntries == 2048, "the maximum bounds the initial count");
    Expect(limits.maxBytes == std::uint64_t{3} << 20u, "the budget is read in MiB");
    Expect(!limits.adaptive, "the fixed switch turns growth off");
    SetEnvironment("APS5_TEST_CACHE_MIB", "lots");
    bool threw = false;
    try {
        CacheSizing::FromEnvironment("APS5_TEST_CACHE", defaults);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Expect(threw, "a value that is not a number throws");
    for (const char* name : {"APS5_TEST_CACHE_ENTRIES", "APS5_TEST_CACHE_MAX_ENTRIES", "APS5_TEST_CACHE_MIB", "APS5_TEST_CACHE_FIXED"}) SetEnvironment(name, nullptr);
}

void KeyHashDistinguishesKeys() {
    const std::uint32_t a[] = {1, 2, 3};
    const std::uint32_t b[] = {1, 2, 4};
    const std::uint32_t c[] = {1, 2, 3, 0};
    Expect(CacheSizing::KeyHash(a, 3) == CacheSizing::KeyHash(a, 3), "the hash is stable");
    Expect(CacheSizing::KeyHash(a, 3) != CacheSizing::KeyHash(b, 3), "a different word changes the hash");
    Expect(CacheSizing::KeyHash(a, 3) != CacheSizing::KeyHash(c, 4), "a different length changes the hash");
}

void DescribeResetsWindow() {
    SimulatedCache cache({4, 4, ~std::uint64_t{0}, true}, 100);
    for (std::uint64_t key = 0; key < 8; ++key) cache.Access(key);
    const auto line = cache.sizing.Describe("test cache", cache.order.size(), cache.order.size() * cache.entryBytes);
    Expect(line.find("test cache: 4 entries of capacity 4") != std::string::npos, "the line names the cache and its capacity: " + line);
    Expect(line.find("100 bytes per entry") != std::string::npos, "the line shows the bytes per entry: " + line);
    Expect(line.find("8 misses") != std::string::npos, "the line counts the misses: " + line);
    const auto window = cache.sizing.TakeWindow();
    Expect(window.misses == 0 && window.evictions == 0, "describing resets the window");
}

}

int main() {
    EvictionHistoryDistances();
    EvictionHistoryForgetsOldest();
    EvictionHistoryResize();
    OverLimitByEntriesAndBytes();
    GrowsForCyclicWorkingSet();
    NoGrowthForStreamingKeys();
    NoGrowthForDistantReuse();
    GrowthBoundedByBytes();
    GrowthBoundedByMaxEntries();
    FixedNeverGrows();
    Environment();
    KeyHashDistinguishesKeys();
    DescribeResetsWindow();
    if (failures != 0) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("cache sizing tests passed");
    return 0;
}

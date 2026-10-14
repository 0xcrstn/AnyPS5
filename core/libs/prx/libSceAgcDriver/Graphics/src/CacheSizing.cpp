#include "prx/libSceAgcDriver/Graphics/include/CacheSizing.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace AgcDriver::Graphics {

namespace {

std::optional<std::uint64_t> environmentNumber(const std::string& name) {
    const char* text = std::getenv(name.c_str());
    if (text == nullptr || *text == '\0') return std::nullopt;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) throw std::runtime_error(name + " is not a positive whole number: " + text);
    return value;
}

}

EvictionHistory::EvictionHistory(std::size_t keys) : ring(std::max<std::size_t>(1, keys)) {}

void EvictionHistory::forgetSlot(const Slot& slot) {
    if (!slot.used) return;
    const auto found = sequences.find(slot.key);
    if (found != sequences.end() && found->second == slot.sequence) sequences.erase(found);
}

void EvictionHistory::Evicted(std::uint64_t key) {
    auto& slot = ring[next % ring.size()];
    forgetSlot(slot);
    slot = {key, next, true};
    sequences[key] = next;
    ++next;
}

std::optional<std::uint64_t> EvictionHistory::Missed(std::uint64_t key) {
    const auto found = sequences.find(key);
    if (found == sequences.end()) return std::nullopt;
    const auto distance = next - 1 - found->second;
    sequences.erase(found);
    return distance;
}

void EvictionHistory::Resize(std::size_t keys) {
    keys = std::max<std::size_t>(1, keys);
    if (keys == ring.size()) return;
    std::vector<Slot> resized(keys);
    const auto oldest = next > keys ? next - keys : 0;
    for (const auto& slot : ring) {
        if (!slot.used) continue;
        if (slot.sequence < oldest) {
            forgetSlot(slot);
            continue;
        }
        resized[slot.sequence % keys] = slot;
    }
    ring = std::move(resized);
}

CacheSizing::CacheSizing(Limits configured) : limits(configured), capacity(std::max<std::size_t>(1, configured.initialEntries)), history(std::min(MaxHistoryKeys, std::max<std::size_t>(1, configured.initialEntries) * HistoryFactor)) {
    limits.initialEntries = capacity;
    limits.maxEntries = std::max(limits.maxEntries, capacity);
    if (!limits.adaptive) limits.maxEntries = capacity;
}

CacheSizing::Limits CacheSizing::FromEnvironment(const std::string& prefix, Limits defaults) {
    auto limits = defaults;
    const auto initial = environmentNumber(prefix + "_ENTRIES");
    const auto maximum = environmentNumber(prefix + "_MAX_ENTRIES");
    const auto mebibytes = environmentNumber(prefix + "_MIB");
    if (initial) limits.initialEntries = static_cast<std::size_t>(*initial);
    if (maximum) limits.maxEntries = static_cast<std::size_t>(*maximum);
    if (mebibytes) limits.maxBytes = *mebibytes << 20u;
    if (std::getenv((prefix + "_FIXED").c_str()) != nullptr) limits.adaptive = false;
    if (maximum) limits.initialEntries = std::min(limits.initialEntries, limits.maxEntries);
    limits.maxEntries = std::max(limits.maxEntries, limits.initialEntries);
    return limits;
}

std::uint64_t CacheSizing::KeyHash(const std::uint32_t* words, std::size_t count) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < count; ++i) hash = (hash ^ words[i]) * 1099511628211ull;
    hash ^= count;
    hash = (hash ^ (hash >> 30u)) * 0xbf58476d1ce4e5b9ull;
    hash = (hash ^ (hash >> 27u)) * 0x94d049bb133111ebull;
    return hash ^ (hash >> 31u);
}

void CacheSizing::Miss(std::uint64_t key, std::size_t entries, std::uint64_t bytes) {
    ++window.misses;
    ++growthMisses;
    if (const auto distance = history.Missed(key)) {
        if (*distance < capacity) {
            ++window.evictedWithin[0];
            ++growthNear;
        }
        if (*distance < 3 * static_cast<std::uint64_t>(capacity)) ++window.evictedWithin[1];
        if (*distance < 7 * static_cast<std::uint64_t>(capacity)) ++window.evictedWithin[2];
    }
    if (limits.adaptive && growthMisses >= std::max(MinGrowthWindow, capacity)) considerGrowth(entries, bytes);
}

void CacheSizing::considerGrowth(std::size_t entries, std::uint64_t bytes) {
    const bool worthwhile = growthNear >= MinGrowthHits && growthNear * 8 >= growthMisses;
    growthMisses = 0;
    growthNear = 0;
    if (!worthwhile || capacity >= limits.maxEntries) return;
    auto target = std::min(capacity * 2, limits.maxEntries);
    if (entries != 0 && bytes != 0) {
        const auto perEntry = std::max<std::uint64_t>(1, bytes / entries);
        target = static_cast<std::size_t>(std::min<std::uint64_t>(target, limits.maxBytes / perEntry));
    }
    if (target <= capacity) return;
    capacity = target;
    history.Resize(std::min(MaxHistoryKeys, capacity * HistoryFactor));
    ++window.growths;
}

void CacheSizing::Evicted(std::uint64_t key) {
    history.Evicted(key);
    ++window.evictions;
}

CacheSizing::Window CacheSizing::TakeWindow() {
    const auto taken = window;
    window = {};
    return taken;
}

std::string CacheSizing::Describe(const char* name, std::size_t entries, std::uint64_t bytes) {
    const auto taken = TakeWindow();
    const auto lookups = taken.hits + taken.misses;
    char budget[64] = "none";
    if (limits.maxBytes != ~std::uint64_t{0}) std::snprintf(budget, sizeof(budget), "%llu MiB", static_cast<unsigned long long>(limits.maxBytes >> 20u));
    char line[768];
    std::snprintf(line, sizeof(line), "[cache-sizing] %s: %zu entries of capacity %zu (initial %zu, max %zu, %s), ~%.1f MiB held (%llu bytes per entry, budget %s); window: %llu hits, %llu misses (%.1f%% hits), misses on keys evicted within 1x/3x/7x capacity %llu/%llu/%llu (would hit at 2x/4x/8x), %llu evictions, %llu growths; history %zu of %zu keys\n", name, entries, capacity, limits.initialEntries, limits.maxEntries, limits.adaptive ? "adaptive" : "fixed", static_cast<double>(bytes) / 1048576.0, static_cast<unsigned long long>(entries != 0 ? bytes / entries : 0), budget, static_cast<unsigned long long>(taken.hits), static_cast<unsigned long long>(taken.misses), lookups != 0 ? 100.0 * static_cast<double>(taken.hits) / static_cast<double>(lookups) : 0.0, static_cast<unsigned long long>(taken.evictedWithin[0]), static_cast<unsigned long long>(taken.evictedWithin[1]), static_cast<unsigned long long>(taken.evictedWithin[2]), static_cast<unsigned long long>(taken.evictions), static_cast<unsigned long long>(taken.growths), history.Size(), history.Capacity());
    return line;
}

}

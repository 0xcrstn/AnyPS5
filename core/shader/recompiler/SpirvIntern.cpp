#include "SpirvIntern.hpp"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ShaderRecompiler {

namespace {

using ContentKey = std::pair<std::uint64_t, std::uint64_t>;

struct InternTable {
    std::mutex mutex;
    std::multimap<ContentKey, std::weak_ptr<const std::vector<std::uint32_t>>> entries;
    SpirvInternCounters counters;
    std::uint64_t inserts = 0;
};

InternTable& table() {
    static auto* instance = new InternTable();
    return *instance;
}

ContentKey contentKey(const std::vector<std::uint32_t>& words) {
    std::uint64_t first = 0xcbf29ce484222325ull;
    std::uint64_t second = 0x9e3779b97f4a7c15ull ^ words.size();
    for (const auto word : words) {
        first = (first ^ word) * 0x100000001b3ull;
        second = (second ^ word) * 0xff51afd7ed558ccdull;
        second ^= second >> 29u;
    }
    return {first, second};
}

}

bool SpirvInternEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_SPIRV_INTERN");
        if (value == nullptr || *value == '\0' || std::string_view(value) == "1") return true;
        if (std::string_view(value) == "0") return false;
        throw std::invalid_argument("APS5_SPIRV_INTERN accepts only 0 or 1");
    }();
    return enabled;
}

SharedSpirv InternSpirv(std::vector<std::uint32_t> words, bool enabled) {
    if (!enabled) return SharedSpirv(std::move(words));
    const auto key = contentKey(words);
    auto& interned = table();
    std::lock_guard lock(interned.mutex);
    ++interned.counters.lookups;
    const auto [first, last] = interned.entries.equal_range(key);
    for (auto it = first; it != last;) {
        auto existing = it->second.lock();
        if (existing == nullptr) {
            it = interned.entries.erase(it);
            continue;
        }
        if (*existing == words) {
            ++interned.counters.shared;
            interned.counters.sharedBytes += words.size() * sizeof(std::uint32_t);
            const std::span<const std::uint32_t> view(*existing);
            return SharedSpirv(std::move(existing), view);
        }
        ++interned.counters.collisions;
        ++it;
    }
    auto stored = std::make_shared<const std::vector<std::uint32_t>>(std::move(words));
    interned.entries.emplace(key, stored);
    if (++interned.inserts % 512u == 0u) {
        for (auto it = interned.entries.begin(); it != interned.entries.end();) it = it->second.expired() ? interned.entries.erase(it) : std::next(it);
    }
    const std::span<const std::uint32_t> view(*stored);
    return SharedSpirv(std::move(stored), view);
}

SpirvInternCounters SpirvInternTotals() {
    auto& interned = table();
    std::lock_guard lock(interned.mutex);
    auto counters = interned.counters;
    counters.entries = interned.entries.size();
    return counters;
}

}

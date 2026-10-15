#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINECONTENTINDEX_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINECONTENTINDEX_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

namespace AgcDriver {

using PipelineContentKey = std::pair<std::uint64_t, std::uint64_t>;

inline PipelineContentKey HashPipelineContent(std::span<const std::uint64_t> fields) {
    std::uint64_t first = 0xcbf29ce484222325ull;
    std::uint64_t second = 0x84222325cbf29ce4ull;
    for (const auto field : fields) {
        for (std::uint32_t byte = 0; byte < 8u; ++byte) {
            const auto value = (field >> (byte * 8u)) & 0xffu;
            first = (first ^ value) * 0x100000001b3ull;
            second = (second + value + 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
            second ^= second >> 33u;
        }
    }
    return {first, second};
}

template<typename TValue>
class PipelineContentIndex {
public:
    std::shared_ptr<TValue> Find(const PipelineContentKey& key) {
        std::lock_guard lock(mutex);
        const auto found = entries.find(key);
        if (found == entries.end()) return nullptr;
        auto value = found->second.lock();
        if (value == nullptr) {
            entries.erase(found);
            return nullptr;
        }
        ++hits;
        return value;
    }

    void Insert(const PipelineContentKey& key, const std::shared_ptr<TValue>& value) {
        std::lock_guard lock(mutex);
        entries[key] = value;
        if (++inserts % 256u != 0u) return;
        for (auto it = entries.begin(); it != entries.end();) it = it->second.expired() ? entries.erase(it) : std::next(it);
    }

    std::uint64_t Hits() const {
        std::lock_guard lock(mutex);
        return hits;
    }

    std::size_t Size() const {
        std::lock_guard lock(mutex);
        return entries.size();
    }

private:
    mutable std::mutex mutex;
    std::map<PipelineContentKey, std::weak_ptr<TValue>> entries;
    std::uint64_t hits = 0;
    std::uint64_t inserts = 0;
};

}

#endif

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINECONTENTINDEX_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINECONTENTINDEX_HPP

#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
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
    struct BuildResult {
        std::shared_ptr<TValue> value;
        bool created = false;
        bool joined = false;
    };

    template<typename TBuilder>
    BuildResult GetOrBuild(const PipelineContentKey& key, TBuilder&& builder) {
        std::shared_future<std::shared_ptr<TValue>> future;
        std::shared_ptr<std::promise<std::shared_ptr<TValue>>> promise;
        {
            std::lock_guard lock(mutex);
            if (const auto found = entries.find(key); found != entries.end()) {
                if (auto value = found->second.lock()) {
                    ++hits;
                    return {std::move(value), false, false};
                }
                entries.erase(found);
            }
            if (const auto found = inFlight.find(key); found != inFlight.end()) {
                future = found->second;
                ++hits;
            } else {
                promise = std::make_shared<std::promise<std::shared_ptr<TValue>>>();
                future = promise->get_future().share();
                inFlight.emplace(key, future);
            }
        }
        if (promise == nullptr) return {future.get(), false, true};
        try {
            std::shared_ptr<TValue> value = std::forward<TBuilder>(builder)();
            {
                std::lock_guard lock(mutex);
                if (value != nullptr) insert(key, value);
                promise->set_value(value);
                inFlight.erase(key);
            }
            const bool created = value != nullptr;
            return {std::move(value), created, false};
        } catch (...) {
            std::lock_guard lock(mutex);
            promise->set_exception(std::current_exception());
            inFlight.erase(key);
            throw;
        }
    }

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
        if (value != nullptr) insert(key, value);
    }

    void Clear() {
        std::lock_guard lock(mutex);
        if (!inFlight.empty()) throw std::logic_error("pipeline content index clear requires drained builds");
        entries.clear();
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
    void insert(const PipelineContentKey& key, const std::shared_ptr<TValue>& value) {
        entries[key] = value;
        if (++inserts % 256u != 0u) return;
        for (auto it = entries.begin(); it != entries.end();) it = it->second.expired() ? entries.erase(it) : std::next(it);
    }

    mutable std::mutex mutex;
    std::map<PipelineContentKey, std::weak_ptr<TValue>> entries;
    std::map<PipelineContentKey, std::shared_future<std::shared_ptr<TValue>>> inFlight;
    std::uint64_t hits = 0;
    std::uint64_t inserts = 0;
};

}

#endif

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINESINGLEFLIGHT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINESINGLEFLIGHT_HPP

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace AgcDriver {

template<typename TKey, typename TValue>
class PipelineSingleflight {
public:
    struct Result {
        std::shared_ptr<TValue> value;
        bool created = false, joined = false;
    };

    std::uint64_t Joins() const {
        std::lock_guard lock(mutex);
        return joins;
    }

    std::shared_ptr<TValue> Find(const TKey& key) const {
        std::lock_guard lock(mutex);
        const auto found = entries.find(key);
        return found != entries.end() ? found->second : nullptr;
    }

    std::shared_ptr<TValue> Adopt(const TKey& key, std::shared_ptr<TValue> value) {
        std::lock_guard lock(mutex);
        return entries.emplace(key, std::move(value)).first->second;
    }

    template<typename TBuild>
    Result GetOrBuild(const TKey& key, TBuild&& build) {
        std::shared_future<std::shared_ptr<TValue>> waiting;
        std::optional<std::promise<std::shared_ptr<TValue>>> promise;
        {
            std::lock_guard lock(mutex);
            if (const auto found = entries.find(key); found != entries.end()) return {found->second, false, false};
            if (const auto found = pending.find(key); found != pending.end()) {
                waiting = found->second;
                ++joins;
            }
            else pending.emplace(key, promise.emplace().get_future().share());
        }
        if (waiting.valid()) return {waiting.get(), false, true};
        try {
            auto result = build();
            {
                std::lock_guard lock(mutex);
                entries.emplace(key, result);
                pending.erase(key);
            }
            promise->set_value(result);
            return {std::move(result), true, false};
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                pending.erase(key);
            }
            promise->set_exception(std::current_exception());
            throw;
        }
    }

    void Clear() {
        std::lock_guard lock(mutex);
        entries.clear();
    }

private:
    mutable std::mutex mutex;
    std::map<TKey, std::shared_ptr<TValue>> entries;
    std::map<TKey, std::shared_future<std::shared_ptr<TValue>>> pending;
    std::uint64_t joins = 0;
};

}

#endif

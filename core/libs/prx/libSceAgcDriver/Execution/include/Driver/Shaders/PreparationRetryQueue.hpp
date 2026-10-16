#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_PREPARATIONRETRYQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_PREPARATIONRETRYQUEUE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

template<typename TSource, typename TContext>
class PreparationRetryQueue {
public:
    static constexpr std::uint64_t Unranked = std::numeric_limits<std::uint64_t>::max();

    struct Entry {
        std::weak_ptr<TSource> source;
        std::weak_ptr<TContext> context;
        std::uint64_t rank = Unranked;
        std::uint64_t sequence = 0;
        bool demanded = false;
        bool processBudgeted = false;
    };

    explicit PreparationRetryQueue(std::size_t capacity) : capacity(capacity) {
        if (capacity == 0) throw std::invalid_argument("preparation retry capacity is zero");
        entries.reserve(capacity);
    }

    bool Push(std::weak_ptr<TSource> source, std::weak_ptr<TContext> context, std::uint64_t rank = Unranked, bool demanded = false, bool processBudgeted = false) {
        std::lock_guard lock(mutex);
        if (closed || source.expired() || context.expired()) return false;
        std::erase_if(entries, [](const auto& entry) { return entry.source.expired() || entry.context.expired(); });
        const auto duplicate = std::ranges::find_if(entries, [&](const auto& entry) { return sameOwner(entry.source, source) && sameOwner(entry.context, context); });
        if (duplicate != entries.end()) {
            duplicate->rank = std::min(duplicate->rank, rank);
            duplicate->demanded |= demanded;
            duplicate->processBudgeted &= processBudgeted;
            return true;
        }
        if (entries.size() == capacity) return false;
        entries.push_back({std::move(source), std::move(context), rank, ++sequence, demanded, processBudgeted});
        return true;
    }

    std::optional<Entry> Pop() {
        std::lock_guard lock(mutex);
        std::erase_if(entries, [](const auto& entry) { return entry.source.expired() || entry.context.expired(); });
        if (entries.empty()) return {};
        const bool oldest = (++popped % 8u) == 0;
        const auto selected = std::min_element(entries.begin(), entries.end(), [&](const auto& left, const auto& right) {
            if (left.demanded != right.demanded) return left.demanded;
            if (!oldest && left.rank != right.rank) return left.rank < right.rank;
            return oldest ? left.sequence < right.sequence : left.sequence > right.sequence;
        });
        Entry result = std::move(*selected);
        entries.erase(selected);
        return result;
    }

    void Cancel(const std::weak_ptr<TSource>& source) {
        std::lock_guard lock(mutex);
        std::erase_if(entries, [&](const auto& entry) { return sameOwner(entry.source, source); });
    }

    std::size_t Size() const {
        std::lock_guard lock(mutex);
        return entries.size();
    }

    void Close() {
        std::lock_guard lock(mutex);
        closed = true;
        entries.clear();
    }

private:
    template<typename TValue>
    static bool sameOwner(const std::weak_ptr<TValue>& left, const std::weak_ptr<TValue>& right) {
        return !left.owner_before(right) && !right.owner_before(left);
    }

    const std::size_t capacity;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::uint64_t sequence = 0;
    std::uint64_t popped = 0;
    bool closed = false;
};

}

#endif

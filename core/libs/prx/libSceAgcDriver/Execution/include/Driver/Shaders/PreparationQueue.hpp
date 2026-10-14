#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_PREPARATIONQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_PREPARATIONQUEUE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace AgcDriver::DriverDetail {

template<typename TTask>
class PreparationQueue {
public:
    static constexpr std::uint64_t Unranked = std::numeric_limits<std::uint64_t>::max();

    void Push(TTask task, const void* owner, std::uint64_t rank, bool demanded, bool behind) {
        Entry entry{std::move(task), owner};
        if (demanded && !behind) urgent.push_front(std::move(entry));
        else if (rank != Unranked && !behind) ranked.emplace(std::pair{rank, sequence++}, std::move(entry));
        else arrival.push_back(std::move(entry));
    }

    [[nodiscard]] bool Empty() const {
        return urgent.empty() && ranked.empty() && arrival.empty();
    }

    [[nodiscard]] std::size_t Size() const {
        return urgent.size() + ranked.size() + arrival.size();
    }

    [[nodiscard]] std::size_t RankedSize() const {
        return ranked.size();
    }

    TTask Pop() {
        if (!urgent.empty()) return take(urgent);
        if (!ranked.empty()) {
            auto task = std::move(ranked.begin()->second.task);
            ranked.erase(ranked.begin());
            return task;
        }
        if (!arrival.empty()) return take(arrival);
        throw std::logic_error("PreparationQueue::Pop on an empty queue");
    }

    std::optional<std::size_t> Prioritize(const void* owner) {
        std::size_t position = 0;
        for (auto it = urgent.begin(); it != urgent.end(); ++it, ++position) {
            if (it->owner != owner) continue;
            std::rotate(urgent.begin(), it, std::next(it));
            return position;
        }
        for (auto it = ranked.begin(); it != ranked.end(); ++it, ++position) {
            if (it->second.owner != owner) continue;
            urgent.push_front(std::move(it->second));
            ranked.erase(it);
            return position;
        }
        for (auto it = arrival.begin(); it != arrival.end(); ++it, ++position) {
            if (it->owner != owner) continue;
            urgent.push_front(std::move(*it));
            arrival.erase(it);
            return position;
        }
        return std::nullopt;
    }

private:
    struct Entry {
        TTask task;
        const void* owner = nullptr;
    };

    static TTask take(std::deque<Entry>& entries) {
        auto task = std::move(entries.front().task);
        entries.pop_front();
        return task;
    }

    std::deque<Entry> urgent;
    std::map<std::pair<std::uint64_t, std::uint64_t>, Entry> ranked;
    std::deque<Entry> arrival;
    std::uint64_t sequence = 0;
};

}

#endif

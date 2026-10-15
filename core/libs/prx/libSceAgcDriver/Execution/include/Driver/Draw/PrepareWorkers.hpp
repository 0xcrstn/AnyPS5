#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_PREPAREWORKERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_PREPAREWORKERS_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRecordThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

enum class FrozenFallback : std::uint8_t { None, Indirect, DrawCache, Debug, NoDevice, GpuMutexHeld, Labels, MetadataPass, RectList, Mesh, Count };

inline constexpr std::array<const char*, static_cast<std::size_t>(FrozenFallback::Count)> FrozenFallbackNames{"none", "indirect", "draw cache", "debug or dump", "no device", "GPU mutex held", "labels queued", "metadata pass", "rect list", "mesh"};

struct FrozenRegisterFiles {
    std::shared_ptr<const Registers> shader;
    std::shared_ptr<const Registers> context;
    std::shared_ptr<const Registers> userConfig;
};

struct FrozenQueueState {
    FrozenRegisterFiles registers;
    std::uint64_t indexBase = 0;
    std::uint64_t drawIndirectBase = 0;
    std::uint64_t dispatchIndirectBase = 0;
    std::uint32_t indexBufferSize = 0;
    std::uint32_t indexType = 0;
    std::uint32_t instanceCount = 1;
    Predication predication;
};

class RegisterFreezer {
public:
    FrozenQueueState Freeze(QueueState& queue) {
        if (&queue != owner) {
            owner = &queue;
            files = {};
        }
        refresh(queue.shader, files.shader);
        refresh(queue.context, files.context);
        refresh(queue.userConfig, files.userConfig);
        return {files, queue.indexBase, queue.drawIndirectBase, queue.dispatchIndirectBase, queue.indexBufferSize, queue.indexType, queue.instanceCount, queue.predication};
    }
    std::uint64_t Copies() const { return copies; }

private:
    void refresh(Registers& live, std::shared_ptr<const Registers>& frozen) {
        if (frozen != nullptr && !live.Changed()) return;
        frozen = std::make_shared<const Registers>(live);
        live.MarkUnchanged();
        ++copies;
    }
    const QueueState* owner = nullptr;
    FrozenRegisterFiles files;
    std::uint64_t copies = 0;
};

class FrozenQueueCopy {
public:
    QueueState& Apply(const FrozenQueueState& frozen) {
        apply(frozen.registers.shader, applied.shader, state.shader);
        apply(frozen.registers.context, applied.context, state.context);
        apply(frozen.registers.userConfig, applied.userConfig, state.userConfig);
        state.savedContext.reset();
        state.constantRam.clear();
        state.markers.clear();
        state.indexBase = frozen.indexBase;
        state.drawIndirectBase = frozen.drawIndirectBase;
        state.dispatchIndirectBase = frozen.dispatchIndirectBase;
        state.indexBufferSize = frozen.indexBufferSize;
        state.indexType = frozen.indexType;
        state.instanceCount = frozen.instanceCount;
        state.predication = frozen.predication;
        return state;
    }
    std::uint64_t Copies() const { return copies; }

private:
    void apply(const std::shared_ptr<const Registers>& from, std::shared_ptr<const Registers>& last, Registers& into) {
        if (from == nullptr) throw std::logic_error("frozen draw without a register file");
        if (from == last && !into.Changed()) return;
        into = *from;
        into.MarkUnchanged();
        last = from;
        ++copies;
    }
    QueueState state;
    FrozenRegisterFiles applied;
    std::uint64_t copies = 0;
};

class RecordHistory {
public:
    explicit RecordHistory(std::size_t capacity) : capacity(capacity) {}
    void Note(std::uint64_t ticket, std::shared_ptr<const DrawRecordEffects> effects) {
        std::lock_guard lock(mutex);
        if (!entries.empty() && ticket != entries.back().first + 1) entries.clear();
        entries.emplace_back(ticket, std::move(effects));
        while (entries.size() > capacity) entries.pop_front();
    }
    std::optional<std::vector<std::shared_ptr<const DrawRecordEffects>>> Between(std::uint64_t after, std::uint64_t through) const {
        std::vector<std::shared_ptr<const DrawRecordEffects>> found;
        if (through <= after) return found;
        std::lock_guard lock(mutex);
        if (entries.empty() || entries.front().first > after + 1 || entries.back().first < through) return std::nullopt;
        for (const auto& [ticket, effects] : entries) {
            if (ticket > after && ticket <= through) found.push_back(effects);
        }
        return found;
    }

private:
    mutable std::mutex mutex;
    std::deque<std::pair<std::uint64_t, std::shared_ptr<const DrawRecordEffects>>> entries;
    std::size_t capacity;
};

enum class FrozenPrepareVerdict : std::uint8_t { Fresh, PendingWrite, Stored, Unchecked };

inline FrozenPrepareVerdict CheckFrozenPrepare(const RecordHistory& history, std::uint64_t start, std::uint64_t ticket, std::span<const PreparedRead> reads, const std::function<bool(std::uint64_t, std::size_t)>& pendingWrite) {
    if (ticket == 0) throw std::logic_error("a frozen draw record checked without its ticket");
    const auto earlier = history.Between(start, ticket - 1);
    if (!earlier) return FrozenPrepareVerdict::Unchecked;
    switch (StaleAfterEach(*earlier, reads, pendingWrite)) {
    case DrawPrepareStale::No: return FrozenPrepareVerdict::Fresh;
    case DrawPrepareStale::PendingWrite: return FrozenPrepareVerdict::PendingWrite;
    case DrawPrepareStale::Stored: return FrozenPrepareVerdict::Stored;
    }
    return FrozenPrepareVerdict::Unchecked;
}

template <class TJob, class TResult>
class OrderedPrepare {
public:
    using Prepare = std::function<TResult(TJob&)>;
    using Emit = std::function<void(TJob&, std::optional<TResult>&, std::exception_ptr)>;
    using Enter = std::function<void(std::size_t)>;
    struct Statistics {
        std::uint64_t submitted = 0;
        std::uint64_t emitted = 0;
        std::uint64_t fullWaits = 0;
        std::uint64_t busyNs = 0;
    };

    OrderedPrepare(std::size_t workers, std::size_t bound, Prepare prepare, Emit emit, Enter enter = {}) : bound(bound), slots(bound), prepare(std::move(prepare)), emit(std::move(emit)), enter(std::move(enter)) {
        if (workers == 0 || bound == 0) throw std::logic_error("ordered preparation needs workers and a bound");
        threads.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index) threads.emplace_back([this, index] { serve(index); });
    }
    ~OrderedPrepare() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        work.notify_all();
        changed.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }
    OrderedPrepare(const OrderedPrepare&) = delete;
    OrderedPrepare& operator=(const OrderedPrepare&) = delete;

    std::uint64_t Submit(TJob job) {
        std::unique_lock lock(mutex);
        rethrowLocked();
        if (submitted - emitted >= bound) {
            ++fullWaits;
            changed.wait(lock, [&] { return submitted - emitted < bound || failure != nullptr || stopping; });
            rethrowLocked();
        }
        if (stopping) throw std::logic_error("ordered preparation stopped");
        const auto ticket = ++submitted;
        auto& slot = slots[ticket % bound];
        slot.job.emplace(std::move(job));
        slot.result.reset();
        slot.error = nullptr;
        slot.ready = false;
        lock.unlock();
        work.notify_one();
        return ticket;
    }
    void WaitEmitted() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return emitted == submitted || failure != nullptr || stopping; });
        rethrowLocked();
    }
    bool Idle() const {
        std::lock_guard lock(mutex);
        return emitted == submitted;
    }
    Statistics Counts() const {
        std::lock_guard lock(mutex);
        return {submitted, emitted, fullWaits, busyNs.load(std::memory_order_relaxed)};
    }
    std::size_t Workers() const { return threads.size(); }
    static bool OnWorker() { return workerIndex() != NoWorker; }

private:
    static constexpr std::size_t NoWorker = ~std::size_t{0};
    struct Slot {
        std::optional<TJob> job;
        std::optional<TResult> result;
        std::exception_ptr error;
        bool ready = false;
    };
    static std::size_t& workerIndex() {
        thread_local std::size_t index = NoWorker;
        return index;
    }
    void rethrowLocked() {
        if (failure != nullptr) std::rethrow_exception(failure);
    }
    void serve(std::size_t index) {
        workerIndex() = index;
        if (enter) enter(index);
        for (;;) {
            std::unique_lock lock(mutex);
            work.wait(lock, [&] { return stopping || started < submitted; });
            if (stopping) return;
            const auto ticket = ++started;
            auto& slot = slots[ticket % bound];
            TJob& job = *slot.job;
            lock.unlock();
            const auto begin = std::chrono::steady_clock::now();
            std::optional<TResult> result;
            std::exception_ptr error;
            try {
                result.emplace(prepare(job));
            } catch (...) {
                error = std::current_exception();
            }
            busyNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count()), std::memory_order_relaxed);
            lock.lock();
            slot.result = std::move(result);
            slot.error = error;
            slot.ready = true;
            if (emitting) continue;
            emitting = true;
            while (!stopping && emitted != submitted) {
                auto& head = slots[(emitted + 1) % bound];
                if (!head.ready) break;
                auto headJob = std::move(*head.job);
                auto headResult = std::move(head.result);
                const auto headError = head.error;
                head.job.reset();
                head.result.reset();
                head.error = nullptr;
                head.ready = false;
                lock.unlock();
                std::exception_ptr emitFailure;
                try {
                    emit(headJob, headResult, headError);
                } catch (...) {
                    emitFailure = std::current_exception();
                }
                lock.lock();
                ++emitted;
                if (emitFailure != nullptr && failure == nullptr) failure = emitFailure;
                changed.notify_all();
            }
            emitting = false;
        }
    }

    mutable std::mutex mutex;
    std::condition_variable work;
    std::condition_variable changed;
    std::size_t bound;
    std::vector<Slot> slots;
    Prepare prepare;
    Emit emit;
    Enter enter;
    std::vector<std::thread> threads;
    std::uint64_t submitted = 0;
    std::uint64_t started = 0;
    std::uint64_t emitted = 0;
    std::uint64_t fullWaits = 0;
    std::atomic<std::uint64_t> busyNs{0};
    std::exception_ptr failure;
    bool emitting = false;
    bool stopping = false;
};

}

#endif

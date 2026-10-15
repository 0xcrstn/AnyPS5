#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPILESERVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPILESERVICE_HPP

#include "CompileTrace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver {

enum class CompilePriority : std::uint8_t { Demand = 0, Upcoming = 1, Predicted = 2, Registration = 3 };

enum class CompileState : std::uint8_t { Queued, Running, Ready, Failed, Cancelled };

class CompileCancelled : public std::runtime_error {
public:
    CompileCancelled() : std::runtime_error("compile job was cancelled") {}
};

class CompileWaitForbidden : public std::logic_error {
public:
    CompileWaitForbidden() : std::logic_error("compile wait while the GPU mutex is held") {}
};

class CompileService;

class CompileTicket {
public:
    using WaitGuard = bool (*)();

    CompileState State() const {
        std::lock_guard lock(mutex);
        return state;
    }

    bool Ready() const {
        return State() == CompileState::Ready;
    }

    bool Settled() const {
        const auto current = State();
        return current == CompileState::Ready || current == CompileState::Failed || current == CompileState::Cancelled;
    }

    CompilePriority Priority() const {
        return static_cast<CompilePriority>(priority.load(std::memory_order_relaxed));
    }

    const std::string& Key() const {
        return key;
    }

    bool CancellationRequested() const {
        return cancelRequested.load(std::memory_order_relaxed);
    }

    void Wait() const {
        RequireWaitAllowed();
        std::unique_lock lock(mutex);
        settledSignal.wait(lock, [&] { return settledLocked(); });
        rethrowLocked();
    }

    template<typename TRep, typename TPeriod>
    bool WaitFor(std::chrono::duration<TRep, TPeriod> timeout) const {
        RequireWaitAllowed();
        std::unique_lock lock(mutex);
        if (!settledSignal.wait_for(lock, timeout, [&] { return settledLocked(); })) return false;
        rethrowLocked();
        return true;
    }

    void OnSettled(std::function<void()> callback) {
        {
            std::lock_guard lock(mutex);
            if (!settledLocked()) {
                callbacks.push_back(std::move(callback));
                return;
            }
        }
        callback();
    }

    bool OnSettledUnlessSettled(std::function<void()> callback) {
        std::lock_guard lock(mutex);
        if (settledLocked()) return false;
        callbacks.push_back(std::move(callback));
        return true;
    }

    std::exception_ptr Failure() const {
        std::lock_guard lock(mutex);
        return failure;
    }

    static void SetWaitGuard(WaitGuard guard) noexcept {
        waitGuard().store(guard, std::memory_order_relaxed);
    }

    static void RequireWaitAllowed() {
        const auto guard = waitGuard().load(std::memory_order_relaxed);
        if (guard != nullptr && guard()) throw CompileWaitForbidden();
    }

private:
    friend class CompileService;

    static std::atomic<WaitGuard>& waitGuard() {
        static std::atomic<WaitGuard> guard{nullptr};
        return guard;
    }

    bool settledLocked() const {
        return state == CompileState::Ready || state == CompileState::Failed || state == CompileState::Cancelled;
    }

    void rethrowLocked() const {
        if (state == CompileState::Failed) std::rethrow_exception(failure);
        if (state == CompileState::Cancelled) throw CompileCancelled();
    }

    void settle(CompileState result, std::exception_ptr error) {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard lock(mutex);
            state = result;
            failure = std::move(error);
            pending.swap(callbacks);
        }
        settledSignal.notify_all();
        std::string failures;
        for (auto& callback : pending) {
            try {
                callback();
            } catch (const std::exception& error) {
                failures += std::string(failures.empty() ? "" : "; ") + error.what();
            } catch (...) {
                failures += std::string(failures.empty() ? "" : "; ") + "unknown exception";
            }
        }
        if (!failures.empty()) {
            std::fprintf(stderr, "[compile-service] a settle callback of %s threw, which callbacks must never do: %s\n", key.c_str(), failures.c_str());
            std::terminate();
        }
    }

    void start() {
        std::lock_guard lock(mutex);
        state = CompileState::Running;
    }

    std::string key;
    mutable std::mutex mutex;
    mutable std::condition_variable settledSignal;
    CompileState state = CompileState::Queued;
    std::exception_ptr failure;
    std::vector<std::function<void()>> callbacks;
    std::atomic<std::uint8_t> priority{static_cast<std::uint8_t>(CompilePriority::Registration)};
    std::atomic<bool> cancelRequested{false};
};

class CompileService {
public:
    using Work = std::function<void(const CompileTicket&)>;

    struct Limits {
        std::size_t workers = 2;
        std::size_t demandReservedWorkers = 1;
        std::size_t budgetBytes = std::size_t{2} << 30u;
        std::chrono::milliseconds aging{2000};
    };

    struct Statistics {
        std::uint64_t submitted = 0, deduplicated = 0, promoted = 0, ready = 0, failed = 0, cancelled = 0;
        std::uint64_t dependencyFailures = 0, budgetDeferrals = 0, demandOverBudget = 0;
        std::array<std::uint64_t, 4> started{};
        std::size_t queued = 0, running = 0, reservedBytes = 0, peakReservedBytes = 0;
    };

    explicit CompileService(Limits limits) : limits(limits) {
        if (limits.workers == 0 || limits.workers > 16 || limits.demandReservedWorkers >= limits.workers + 1 || limits.budgetBytes == 0) throw std::invalid_argument("invalid compile service limits");
        threads.reserve(limits.workers);
        try {
            for (std::size_t index = 0; index < limits.workers; ++index) threads.emplace_back([this] { run(); });
        } catch (...) {
            Close();
            throw;
        }
    }

    ~CompileService() {
        Close();
    }

    CompileService(const CompileService&) = delete;
    CompileService& operator=(const CompileService&) = delete;

    std::shared_ptr<CompileTicket> Submit(const std::string& key, CompilePriority priority, std::size_t reservedBytes, Work work, std::vector<std::shared_ptr<CompileTicket>> dependencies = {}) {
        std::shared_ptr<CompileTicket> ticket;
        {
            std::lock_guard lock(mutex);
            if (closed) throw std::logic_error("compile service is closed");
            if (const auto found = jobs.find(key); found != jobs.end() && found->second.ticket->State() != CompileState::Cancelled) {
                ++stats.deduplicated;
                ticket = found->second.ticket;
                promoteLocked(*ticket, priority);
            } else {
                auto created = std::make_shared<CompileTicket>();
                created->key = key;
                created->priority.store(static_cast<std::uint8_t>(priority), std::memory_order_relaxed);
                Job job{created, std::move(work), std::move(dependencies), reservedBytes, ++sequence, ShaderCompileTrace::Clock::now(), false};
                for (const auto& dependency : job.dependencies) {
                    if (dependency == nullptr) throw std::invalid_argument("compile job has a null dependency");
                    promoteLocked(*dependency, priority);
                }
                jobs.insert_or_assign(key, std::move(job));
                ++stats.submitted;
                ticket = std::move(created);
            }
        }
        changed.notify_all();
        return ticket;
    }

    std::shared_ptr<CompileTicket> Find(const std::string& key) const {
        std::lock_guard lock(mutex);
        const auto found = jobs.find(key);
        return found != jobs.end() ? found->second.ticket : nullptr;
    }

    void Promote(const CompileTicket& ticket, CompilePriority priority) {
        {
            std::lock_guard lock(mutex);
            promoteLocked(ticket, priority);
        }
        changed.notify_all();
    }

    bool Park(const std::shared_ptr<CompileTicket>& ticket, std::function<void()> callback) {
        if (ticket == nullptr) throw std::invalid_argument("parking on a null compile ticket");
        {
            std::lock_guard lock(mutex);
            promoteLocked(*ticket, CompilePriority::Demand);
            if (!ticket->OnSettledUnlessSettled(std::move(callback))) return false;
        }
        changed.notify_all();
        return true;
    }

    bool Cancel(const CompileTicket& ticket) {
        std::shared_ptr<CompileTicket> cancelled;
        {
            std::lock_guard lock(mutex);
            const auto found = jobs.find(ticket.key);
            if (found == jobs.end() || found->second.ticket.get() != &ticket) return false;
            if (found->second.running) {
                found->second.ticket->cancelRequested.store(true, std::memory_order_relaxed);
                return true;
            }
            if (found->second.ticket->Settled()) return false;
            cancelled = found->second.ticket;
            jobs.erase(found);
            ++stats.cancelled;
        }
        cancelled->settle(CompileState::Cancelled, nullptr);
        changed.notify_all();
        return true;
    }

    std::size_t CancelSpeculative() {
        std::vector<std::shared_ptr<CompileTicket>> cancelled;
        {
            std::lock_guard lock(mutex);
            for (auto it = jobs.begin(); it != jobs.end();) {
                const auto& ticket = it->second.ticket;
                if (it->second.running || ticket->Settled() || ticket->Priority() == CompilePriority::Demand || ticket->Priority() == CompilePriority::Upcoming) {
                    ++it;
                    continue;
                }
                cancelled.push_back(ticket);
                it = jobs.erase(it);
                ++stats.cancelled;
            }
        }
        for (const auto& ticket : cancelled) ticket->settle(CompileState::Cancelled, nullptr);
        changed.notify_all();
        return cancelled.size();
    }

    std::shared_ptr<CompileTicket> RunAndWait(const std::string& key, std::size_t reservedBytes, Work work) {
        CompileTicket::RequireWaitAllowed();
        auto ticket = Submit(key, CompilePriority::Demand, reservedBytes, std::move(work));
        ticket->Wait();
        return ticket;
    }

    void Close() {
        std::vector<std::shared_ptr<CompileTicket>> cancelled;
        {
            std::lock_guard lock(mutex);
            if (!closed) {
                closed = true;
                for (auto it = jobs.begin(); it != jobs.end();) {
                    if (it->second.running || it->second.ticket->Settled() || it->second.ticket->Priority() == CompilePriority::Demand) {
                        ++it;
                        continue;
                    }
                    cancelled.push_back(it->second.ticket);
                    it = jobs.erase(it);
                    ++stats.cancelled;
                }
            }
        }
        for (const auto& ticket : cancelled) ticket->settle(CompileState::Cancelled, nullptr);
        changed.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
        }
    }

    void Forget(const std::string& key) {
        std::lock_guard lock(mutex);
        const auto found = jobs.find(key);
        if (found != jobs.end() && found->second.ticket->Settled()) jobs.erase(found);
    }

    Statistics Counters() const {
        std::lock_guard lock(mutex);
        auto result = stats;
        result.queued = 0;
        for (const auto& [key, job] : jobs) {
            if (!job.running && !job.ticket->Settled()) ++result.queued;
        }
        result.running = runningJobs;
        result.reservedBytes = reserved;
        return result;
    }

private:
    struct Job {
        std::shared_ptr<CompileTicket> ticket;
        Work work;
        std::vector<std::shared_ptr<CompileTicket>> dependencies;
        std::size_t reservedBytes = 0;
        std::uint64_t sequence = 0;
        ShaderCompileTrace::Clock::time_point enqueued{};
        bool running = false;
    };

    void promoteLocked(const CompileTicket& ticket, CompilePriority priority) {
        auto current = ticket.priority.load(std::memory_order_relaxed);
        const auto wanted = static_cast<std::uint8_t>(priority);
        if (wanted >= current) return;
        const_cast<CompileTicket&>(ticket).priority.store(wanted, std::memory_order_relaxed);
        ++stats.promoted;
        const auto found = jobs.find(ticket.key);
        if (found == jobs.end() || found->second.ticket.get() != &ticket) return;
        for (const auto& dependency : found->second.dependencies) promoteLocked(*dependency, priority);
    }

    int effectivePriorityLocked(const Job& job, ShaderCompileTrace::Clock::time_point now) const {
        const int base = job.ticket->priority.load(std::memory_order_relaxed);
        if (base <= static_cast<int>(CompilePriority::Upcoming) || limits.aging.count() <= 0) return base;
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.enqueued).count() / limits.aging.count();
        return std::max(static_cast<int>(CompilePriority::Upcoming), base - static_cast<int>(std::min<long long>(waited, 3)));
    }

    enum class Readiness { Waiting, Runnable, DependencyFailed };

    Readiness readinessLocked(const Job& job) const {
        for (const auto& dependency : job.dependencies) {
            const auto state = dependency->State();
            if (state == CompileState::Failed || state == CompileState::Cancelled) return Readiness::DependencyFailed;
            if (state != CompileState::Ready) return Readiness::Waiting;
        }
        return Readiness::Runnable;
    }

    Job* pickLocked(std::vector<std::shared_ptr<CompileTicket>>& dependencyFailures) {
        const auto now = ShaderCompileTrace::Clock::now();
        Job* best = nullptr;
        int bestPriority = 0;
        bool deferred = false;
        const std::size_t speculativeSlots = limits.workers - std::min(limits.demandReservedWorkers, limits.workers - 1);
        for (auto it = jobs.begin(); it != jobs.end();) {
            auto& job = it->second;
            if (job.running || job.ticket->Settled()) {
                ++it;
                continue;
            }
            const auto readiness = readinessLocked(job);
            if (readiness == Readiness::DependencyFailed) {
                dependencyFailures.push_back(job.ticket);
                it = jobs.erase(it);
                ++stats.dependencyFailures;
                continue;
            }
            ++it;
            if (readiness == Readiness::Waiting) continue;
            const bool demand = job.ticket->Priority() == CompilePriority::Demand;
            if (!demand && runningSpeculative >= speculativeSlots) continue;
            if (!demand && runningJobs != 0 && job.reservedBytes > limits.budgetBytes - std::min(reserved, limits.budgetBytes)) {
                deferred = true;
                continue;
            }
            const int priority = effectivePriorityLocked(job, now);
            if (best == nullptr || priority < bestPriority || (priority == bestPriority && job.sequence < best->sequence)) {
                best = &job;
                bestPriority = priority;
            }
        }
        if (best == nullptr && deferred) ++stats.budgetDeferrals;
        return best;
    }

    void run() noexcept {
        for (;;) {
            std::shared_ptr<CompileTicket> ticket;
            Work work;
            std::size_t bytes = 0;
            bool demand = false;
            ShaderCompileTrace::Clock::time_point enqueued{};
            std::vector<std::shared_ptr<CompileTicket>> dependencyFailures;
            {
                std::unique_lock lock(mutex);
                Job* job = nullptr;
                changed.wait(lock, [&] {
                    job = pickLocked(dependencyFailures);
                    return job != nullptr || !dependencyFailures.empty() || (closed && runnableOrQueuedLocked() == 0);
                });
                if (job != nullptr) {
                    job->running = true;
                    ticket = job->ticket;
                    work = std::move(job->work);
                    bytes = job->reservedBytes;
                    enqueued = job->enqueued;
                    demand = ticket->Priority() == CompilePriority::Demand;
                    if (demand && reserved + bytes > limits.budgetBytes) ++stats.demandOverBudget;
                    reserved += bytes;
                    stats.peakReservedBytes = std::max(stats.peakReservedBytes, reserved);
                    ++runningJobs;
                    if (!demand) ++runningSpeculative;
                    ++stats.started[static_cast<std::size_t>(ticket->Priority())];
                } else if (dependencyFailures.empty()) {
                    return;
                }
            }
            for (const auto& failed : dependencyFailures) {
                failed->settle(CompileState::Failed, std::make_exception_ptr(std::runtime_error("compile job dependency failed or was cancelled: " + failed->key)));
                std::lock_guard lock(mutex);
                ++stats.failed;
            }
            if (ticket == nullptr) {
                changed.notify_all();
                continue;
            }
            ticket->start();
            CompileState result = CompileState::Ready;
            std::exception_ptr error;
            {
                ShaderCompileTrace::Event event("queue.compile_job");
                event.Queued(enqueued).Text("key", ticket->key).Number("priority", static_cast<std::uint64_t>(ticket->Priority())).Number("reserved_bytes", bytes);
                try {
                    if (ticket->CancellationRequested()) throw CompileCancelled();
                    work(*ticket);
                } catch (const CompileCancelled&) {
                    result = CompileState::Cancelled;
                } catch (...) {
                    result = CompileState::Failed;
                    error = std::current_exception();
                }
                if (result != CompileState::Ready) event.Fail(result == CompileState::Cancelled ? "cancelled" : "failed");
            }
            work = {};
            {
                std::lock_guard lock(mutex);
                reserved -= bytes;
                --runningJobs;
                if (!demand) --runningSpeculative;
                if (const auto found = jobs.find(ticket->key); found != jobs.end() && found->second.ticket == ticket) {
                    found->second.running = false;
                    found->second.dependencies.clear();
                    if (result == CompileState::Cancelled) jobs.erase(found);
                }
                if (result == CompileState::Ready) ++stats.ready;
                else if (result == CompileState::Failed) ++stats.failed;
                else ++stats.cancelled;
            }
            ticket->settle(result, std::move(error));
            changed.notify_all();
        }
    }

    std::size_t runnableOrQueuedLocked() const {
        std::size_t count = runningJobs;
        for (const auto& [key, job] : jobs) {
            if (!job.running && !job.ticket->Settled()) ++count;
        }
        return count;
    }

    const Limits limits;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, Job> jobs;
    std::vector<std::thread> threads;
    Statistics stats;
    std::uint64_t sequence = 0;
    std::size_t reserved = 0;
    std::size_t runningJobs = 0;
    std::size_t runningSpeculative = 0;
    bool closed = false;
};

}

#endif

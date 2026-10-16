#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/PreparationAdmission.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/PreparationRetryQueue.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using AgcDriver::DriverDetail::PreparationAdmission;
using RetryQueue = AgcDriver::DriverDetail::PreparationRetryQueue<const int, int>;

void Require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

void BackpressureAndResumption() {
    PreparationAdmission admission({100, 2, 0});
    auto running = admission.TryAcquire(60);
    auto queued = admission.TryAcquire(30);
    Require(running && queued, "bounded jobs were refused below the budget");
    Require(!admission.TryAcquire(1), "queued jobs did not consume the job budget");
    const auto generation = admission.Counters().generation;
    queued.Reset();
    Require(!admission.TryAcquire(41), "concurrent reservations exceeded the byte budget");
    auto resumed = admission.TryAcquire(40);
    Require(resumed && admission.Counters().generation > generation, "budget refusal was permanent after a reservation was released");
    running.Reset();
    resumed.Reset();
    const auto stats = admission.Counters();
    Require(stats.bytes == 0 && stats.jobs == 0 && stats.peakBytes == 100 && stats.peakJobs == 2, "completion leaked an admission reservation");
}

void ProcessBudget() {
    PreparationAdmission admission({100, 4, 200});
    auto first = admission.TryAcquire(60, false, 100);
    Require(first && !admission.TryAcquire(41, false, 100), "process admission ignored outstanding working memory");
    Require(!admission.TryAcquire(1, false, 201), "speculation ran above process commitment budget");
    first.Reset();
    auto resumed = admission.TryAcquire(60, false, 100);
    Require(resumed && !admission.TryAcquire(1, false, std::numeric_limits<std::uint64_t>::max()), "process budget arithmetic overflowed");
}

void InitialAdmissionPolicyAndPartialLifetime() {
    PreparationAdmission admission({100, 2, 1});
    auto initial = admission.TryAcquire(60, false, 1000, false);
    Require(initial && !admission.TryAcquire(1, false, 1000, true), "initial registration policy and later process budget were conflated");
    auto primary = std::make_shared<PreparationAdmission::Reservation>(std::move(initial));
    auto partial = primary;
    primary.reset();
    Require(admission.Counters().bytes == 60 && !admission.TryAcquire(41, false, 0, false), "primary completion released memory while partial preparation retained its plan");
    partial.reset();
    Require(admission.Counters().bytes == 0 && admission.TryAcquire(1, false, 0, true), "partial completion did not release the shared reservation");
    RetryQueue retries(2);
    auto source = std::make_shared<const int>(1);
    auto context = std::make_shared<int>(2);
    Require(retries.Push(source, context, 3, false, true), "budgeted retry was refused");
    auto later = retries.Pop();
    Require(later && later->processBudgeted, "retry dropped the post-limit process budget policy");
    Require(retries.Push(later->source, later->context, later->rank, later->demanded, later->processBudgeted), "budgeted retry could not be resumed");
    Require(retries.Push(source, context, 3, false, false), "initial-policy retry could not be deduplicated");
    auto earlier = retries.Pop();
    Require(earlier && !earlier->processBudgeted, "retry deduplication discarded the initial registration preference");
}

void DemandBypass() {
    PreparationAdmission admission({100, 1, 120});
    auto speculative = admission.TryAcquire(100, false, 10);
    auto demanded = admission.TryAcquire(150, true, 500);
    Require(speculative && demanded, "demand waited behind speculative byte or process caps");
    Require(!admission.TryAcquire(1, true, 500), "more than one over-budget demand job was admitted");
    Require(!admission.TryAcquire(1), "speculation bypassed an active oversized demand job");
    demanded.Reset();
    auto next = admission.TryAcquire(150, true, 500);
    Require(next && admission.Counters().demandBypasses == 2, "oversized demand admission did not resume after completion");
}

void ReservationLifetime() {
    PreparationAdmission::Reservation survivor;
    {
        PreparationAdmission admission({100, 2, 0});
        survivor = admission.TryAcquire(60);
        auto replacement = admission.TryAcquire(40);
        replacement = std::move(survivor);
        Require(!survivor && replacement && admission.Counters().bytes == 60 && admission.Counters().jobs == 1, "move assignment leaked or duplicated a reservation");
        survivor = std::move(replacement);
        admission.Close();
        Require(!admission.TryAcquire(1, true) && admission.Counters().bytes == 60, "close released active memory before work completed");
    }
    survivor.Reset();
    survivor.Reset();
    PreparationAdmission admission({std::numeric_limits<std::size_t>::max(), 3, 0});
    auto full = admission.TryAcquire(std::numeric_limits<std::size_t>::max());
    Require(full && !admission.TryAcquire(1, true), "demand reservation overflowed the accounting");
    Require(PreparationAdmission::Estimate(std::numeric_limits<std::size_t>::max(), 0) == std::numeric_limits<std::size_t>::max(), "shader working estimate overflowed");
}

void ConcurrentReservation() {
    PreparationAdmission admission({100, 8, 0});
    std::barrier acquired(9);
    std::barrier release(9);
    std::atomic<unsigned> accepted{0};
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < 8; ++index) {
        workers.emplace_back([&] {
            auto reservation = admission.TryAcquire(25);
            if (reservation) accepted.fetch_add(1);
            acquired.arrive_and_wait();
            release.arrive_and_wait();
        });
    }
    acquired.arrive_and_wait();
    const auto stats = admission.Counters();
    release.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    Require(accepted.load() == 4 && stats.bytes == 100 && stats.jobs == 4 && admission.Counters().bytes == 0, "parallel speculation overcommitted or failed to release memory");
}

void WeakRetryLifetimeAndProgress() {
    PreparationAdmission admission({50, 1, 0});
    RetryQueue retries(2);
    auto source = std::make_shared<const int>(7);
    auto context = std::make_shared<int>(9);
    auto running = admission.TryAcquire(50);
    Require(!admission.TryAcquire(40) && retries.Push(source, context), "deferred preparation was not retained for a retry");
    Require(source.use_count() == 1 && context.use_count() == 1, "a retry token kept a shader or a device alive");
    running.Reset();
    auto entry = retries.Pop();
    Require(entry && entry->source.lock() == source && entry->context.lock() == context, "live deferred input was lost when budget became available");
    auto resumed = admission.TryAcquire(40);
    Require(resumed && retries.Size() == 0, "deferred work did not progress after bytes were released");
    Require(retries.Push(source, context) && retries.Push(source, context) && retries.Size() == 1, "retry deduplication retained duplicate metadata");
    source.reset();
    Require(!retries.Pop() && retries.Size() == 0, "expired shader input was resumed");
    source = std::make_shared<const int>(10);
    Require(retries.Push(source, context), "replacement shader was refused");
    context.reset();
    Require(!retries.Pop(), "expired device context was resumed");
}

void BoundedPriorityAndCancellation() {
    RetryQueue retries(9);
    auto context = std::make_shared<int>(0);
    std::array<std::shared_ptr<const int>, 10> sources;
    for (int index = 0; index < 10; ++index) sources[index] = std::make_shared<const int>(index);
    for (std::size_t index = 0; index < 9; ++index) Require(retries.Push(sources[index], context, index == 0 ? 99 : 1), "retry capacity was not usable");
    Require(!retries.Push(sources[9], context), "deferred metadata exceeded its capacity");
    Require(retries.Push(sources[1], context, 1, true), "demand promotion of an existing retry was refused");
    auto demanded = retries.Pop();
    Require(demanded && demanded->source.lock() == sources[1], "demand was scheduled behind predicted work");
    for (unsigned index = 0; index < 6; ++index) Require(retries.Pop()->source.lock() != sources[0], "prior-use rank was ignored");
    auto oldest = retries.Pop();
    Require(oldest && oldest->source.lock() == sources[0], "ranked retry work permanently starved old input");
    retries.Cancel(sources[2]);
    while (const auto entry = retries.Pop()) Require(entry->source.lock() != sources[2], "cancelled input was resumed");
    Require(retries.Push(sources[3], context), "cancelled metadata was not reclaimed");
    retries.Close();
    Require(retries.Size() == 0 && !retries.Push(sources[4], context), "closed retry queue retained or accepted work");
}

}

int main() {
    try {
        BackpressureAndResumption();
        ProcessBudget();
        InitialAdmissionPolicyAndPartialLifetime();
        DemandBypass();
        ReservationLifetime();
        ConcurrentReservation();
        WeakRetryLifetimeAndProgress();
        BoundedPriorityAndCancellation();
        std::cout << "preparation admission tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

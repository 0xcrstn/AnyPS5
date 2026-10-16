#include "prx/libSceAgcDriver/Graphics/include/PipelinePreparation.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeStore.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local std::size_t failedAllocationBytes = 0;
thread_local std::size_t failedAllocationMatch = 1;
thread_local bool failNextAllocation = false;
thread_local bool allocationFailed = false;
}

void* operator new(std::size_t bytes) {
    if (failNextAllocation || (failedAllocationBytes != 0 && failedAllocationBytes == bytes && --failedAllocationMatch == 0)) {
        failNextAllocation = false;
        failedAllocationBytes = 0;
        allocationFailed = true;
        throw std::bad_alloc();
    }
    if (void* result = std::malloc(bytes == 0 ? 1 : bytes)) return result;
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

using namespace AgcDriver::Graphics;
using namespace std::chrono_literals;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class Directory {
public:
    Directory() {
        path = std::filesystem::temp_directory_path() / ("anyps5-pipeline-preparation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Require(std::filesystem::create_directory(path), "could not create pipeline preparation test directory");
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

PipelineRecipe Fixture() {
    PipelineRecipe recipe;
    recipe.state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    recipe.state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    recipe.state.depthCompare = VK_COMPARE_OP_ALWAYS;
    auto shader = std::make_shared<ShaderRecompiler::RecompileResult>();
    shader->spirv = std::vector<std::uint32_t>{0x07230203u, 0x00010000u, 0u, 2u, 0u};
    recipe.stages.push_back({ShaderRecompiler::ShaderStage::Vertex, 0u, std::move(shader)});
    return recipe;
}

class Scheduler {
public:
    struct Job {
        std::string key;
        std::size_t bytes;
        std::function<void()> work;
        std::shared_ptr<std::promise<void>> completion;
    };

    PipelinePreparation::Schedule Callback() {
        return [this](const std::string& key, std::size_t bytes, std::function<void()> work) {
            auto completion = std::make_shared<std::promise<void>>();
            auto future = completion->get_future().share();
            std::lock_guard lock(mutex);
            jobs.push_back({key, bytes, std::move(work), std::move(completion)});
            return future;
        };
    }

    void Run(std::size_t index) {
        Job job;
        {
            std::lock_guard lock(mutex);
            Require(index < jobs.size() && bool(jobs[index].work), "scheduler job is absent or already ran");
            job = std::move(jobs[index]);
        }
        try {
            job.work();
            job.completion->set_value();
        } catch (...) {
            job.completion->set_exception(std::current_exception());
        }
    }

    void Cancel(std::size_t index) {
        std::shared_ptr<std::promise<void>> completion;
        {
            std::lock_guard lock(mutex);
            Require(index < jobs.size() && bool(jobs[index].work), "scheduler cancellation has no pending job");
            jobs[index].work = {};
            completion = std::move(jobs[index].completion);
        }
        completion->set_exception(std::make_exception_ptr(PipelinePreparationCancelled()));
    }

    std::size_t Count() {
        std::lock_guard lock(mutex);
        return jobs.size();
    }

private:
    std::mutex mutex;
    std::vector<Job> jobs;
};

void SingleflightAndAccounting() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    std::atomic<int> replays{0};
    auto recipe = Fixture();
    PipelinePreparation preparation(store, scheduler.Callback(), [&](const PipelineRecipe& input) {
        Require(PipelineRecipeIdentity(input) == PipelineRecipeIdentity(recipe), "replay changed recipe content");
        ++replays;
    }, [] { return true; }, {1u, 1024u * 1024u});
    Require(preparation.Prepare(recipe), "first recipe was not admitted");
    const auto reserved = preparation.Counters().retainedBytes;
    Require(reserved != 0u && reserved <= 1024u * 1024u, "admitted recipe exceeded its capture byte budget");
    Require(preparation.Prepare(recipe), "duplicate pending recipe was not joined");
    Require(scheduler.Count() == 1u, "duplicate recipe scheduled another job");
    auto different = recipe;
    different.state.frontFace = VK_FRONT_FACE_CLOCKWISE;
    Require(!preparation.Prepare(different), "full preparation queue admitted another recipe");
    const auto pending = preparation.Counters();
    Require(pending.pending == 1u && pending.retainedBytes == reserved && pending.deferred == 1u, "pending recipe accounting is wrong");
    scheduler.Run(0u);
    const auto ready = preparation.Checkpoint();
    Require(ready.ready == 1u && ready.failed == 0u && ready.pending == 0u && ready.retainedBytes == 0u && replays == 1, "successful replay did not release its admission");
    Require(preparation.Prepare(different), "released preparation admission could not be reused");
    scheduler.Run(1u);
    preparation.Checkpoint();
}

void ByteBudgetRefusal() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    PipelinePreparation preparation(store, scheduler.Callback(), [](const PipelineRecipe&) {}, [] { return true; }, {8u, 1u});
    Require(!preparation.Prepare(Fixture()) && scheduler.Count() == 0u, "recipe larger than capture budget reached the scheduler");
    const auto counters = preparation.Counters();
    Require(counters.deferred == 1u && counters.pending == 0u && counters.retainedBytes == 0u, "byte refusal retained a job or capture bytes");
}

void FailedCompletion() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    PipelinePreparation preparation(store, scheduler.Callback(), [](const PipelineRecipe&) {
        throw std::runtime_error("synthetic driver refusal");
    }, [] { return true; }, {2u, 1024u * 1024u});
    Require(preparation.Prepare(Fixture()), "failing replay was not admitted");
    scheduler.Run(0u);
    const auto failed = preparation.Checkpoint();
    Require(failed.failed == 1u && failed.ready == 0u && failed.pending == 0u && failed.retainedBytes == 0u, "failed replay was reported ready or retained its admission");
}

void SynchronousSubmissionFailure() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    PipelinePreparation preparation(store, [](const std::string&, std::size_t, std::function<void()>) -> std::shared_future<void> {
        throw std::runtime_error("synthetic scheduler refusal");
    }, [](const PipelineRecipe&) {}, [] { return true; }, {1u, 1024u * 1024u});
    preparation.Prepare(Fixture());
    const auto failed = preparation.Checkpoint();
    Require(failed.failed == 1u && failed.ready == 0u && failed.pending == 0u && failed.retainedBytes == 0u, "submission failure did not settle and release admission");
}

void CaptureAnalysisAllocationFailure() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    PipelinePreparation preparation(store, scheduler.Callback(), [](const PipelineRecipe&) {}, [] { return true; }, {1u, 1024u * 1024u});
    const auto recipe = Fixture();
    allocationFailed = false;
    failedAllocationMatch = 2;
    failedAllocationBytes = sizeof(PipelineRecipe::Stage);
    preparation.Capture(recipe);
    failedAllocationBytes = 0;
    const auto failed = preparation.Counters();
    Require(allocationFailed && scheduler.Count() == 0u && failed.admitted == 1u, "capture analysis allocation failure was not injected after admission and before scheduling");
    Require(failed.captureFailed == 1u && failed.failed == 0u && failed.pending == 0u && failed.retainedBytes == 0u, "capture analysis failure retained an orphan submission or miscounted failure");
    preparation.Checkpoint();
    Require(preparation.Prepare(recipe), "capture analysis rollback did not release admission for retry");
    scheduler.Run(0u);
    Require(preparation.Checkpoint().ready == 1u, "capture analysis rollback prevented a successful retry");
    preparation.Close();
}

void FailedPromiseAllocationFailure() {
    struct SchedulerRefusal {};
    for (const bool capture : {false, true}) {
        Directory directory;
        auto store = std::make_shared<PipelineRecipeStore>(directory.path);
        PipelinePreparation preparation(store, [](const std::string&, std::size_t, std::function<void()>) -> std::shared_future<void> {
            failNextAllocation = true;
            throw SchedulerRefusal{};
        }, [](const PipelineRecipe&) {}, [] { return true; }, {1u, 1024u * 1024u});
        const auto recipe = Fixture();
        allocationFailed = false;
        bool rejected = false;
        if (capture) preparation.Capture(recipe);
        else {
            try { preparation.Prepare(recipe); }
            catch (const std::bad_alloc&) { rejected = true; }
        }
        failNextAllocation = false;
        const auto failed = preparation.Counters();
        Require(allocationFailed && (capture || rejected) && failed.admitted == 1u, "failed-promise allocation was not injected after a scheduler exception");
        Require(failed.pending == 0u && failed.retainedBytes == 0u && failed.failed == (capture ? 0u : 1u) && failed.captureFailed == (capture ? 1u : 0u), "failed-promise allocation retained an orphan submission or miscounted failure");
        preparation.Checkpoint();
        preparation.Close();
    }
}

void GuardAndClosedAdmission() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    bool allowed = true;
    PipelinePreparation preparation(store, scheduler.Callback(), [](const PipelineRecipe&) {}, [&] { return allowed; }, {1u, 1024u * 1024u});
    allowed = false;
    bool rejected = false;
    try {
        preparation.Checkpoint();
    } catch (const std::exception&) {
        rejected = true;
    }
    Require(rejected, "checkpoint ignored the wait guard");
    allowed = true;
    preparation.Close();
    Require(!preparation.Prepare(Fixture()) && scheduler.Count() == 0u, "closed preparation accepted a replay");
}

void CompletedBeforeClose() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    PipelinePreparation preparation(store, scheduler.Callback(), [](const PipelineRecipe&) {}, [] { return true; }, {1u, 1024u * 1024u});
    Require(preparation.Prepare(Fixture()), "replay was not admitted before close");
    scheduler.Run(0u);
    preparation.Close();
    const auto counters = preparation.Counters();
    Require(counters.ready == 1u && counters.cancelled == 0u, "close misreported an already completed replay as cancelled");
}

void CaptureUsesScheduler() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    PipelinePreparation preparation(store, scheduler.Callback(), [](const PipelineRecipe&) {}, [] { return true; }, {2u, 1024u * 1024u});
    const auto recipe = Fixture();
    preparation.Capture(recipe);
    preparation.Capture(recipe);
    Require(scheduler.Count() == 1u && store->List(2u).empty(), "capture bypassed scheduler or scheduled duplicates");
    scheduler.Run(0u);
    const auto counters = preparation.Checkpoint();
    Require(counters.captured == 1u && counters.captureFailed == 0u && counters.retainedBytes == 0u, "scheduled capture did not settle and release its bytes");
    PipelineRecipe restored;
    Require(store->Load(PipelineRecipeIdentity(recipe), restored) == PipelineRecipeStoreStatus::Loaded, "scheduled capture did not persist its exact recipe");
}

void SchedulerCancellation() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    std::atomic<int> runs{0};
    PipelinePreparation preparation(store, scheduler.Callback(), [&](const PipelineRecipe&) { ++runs; }, [] { return true; }, {1u, 1024u * 1024u});
    Require(preparation.Prepare(Fixture()), "cancelled replay was not admitted");
    scheduler.Cancel(0u);
    const auto counters = preparation.Checkpoint();
    Require(counters.cancelled == 1u && counters.failed == 0u && counters.ready == 0u && counters.pending == 0u && counters.retainedBytes == 0u && runs == 0, "scheduler cancellation was reported as a compile failure or retained admission");
}

void StoredAdmissionRetries() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    const auto recipe = Fixture();
    auto other = recipe;
    other.state.frontFace = VK_FRONT_FACE_CLOCKWISE;
    Require(store->Store(recipe) == PipelineRecipeStoreStatus::Stored && store->Store(other) == PipelineRecipeStoreStatus::Stored, "stored replay fixtures were not persisted");
    std::size_t reservation = 0u;
    {
        Scheduler probe;
        PipelinePreparation preparation(store, probe.Callback(), [](const PipelineRecipe&) {}, [] { return true; }, {1u, 1024u * 1024u});
        Require(preparation.Prepare(recipe), "could not measure an admitted replay reservation");
        reservation = preparation.Counters().retainedBytes;
        probe.Run(0u);
        preparation.Checkpoint();
    }
    Require(reservation != 0u, "replay reservation is zero");
    Scheduler scheduler;
    std::atomic<int> runs{0};
    PipelinePreparation preparation(store, scheduler.Callback(), [&](const PipelineRecipe&) { ++runs; }, [] { return true; }, {8u, reservation});
    Require(preparation.PrepareStored(8u) == 1u && scheduler.Count() == 1u, "stored replay did not respect its byte admission limit");
    scheduler.Run(0u);
    preparation.Checkpoint();
    Require(preparation.PrepareStored(8u) == 1u && scheduler.Count() == 2u, "temporarily deferred stored recipe was discarded after admission released");
    scheduler.Run(1u);
    const auto counters = preparation.Checkpoint();
    Require(runs == 2 && counters.ready == 2u && counters.failed == 0u && counters.pending == 0u, "stored batch replay did not prepare both recipes exactly once");
    Require(preparation.PrepareStored(8u) == 0u && scheduler.Count() == 2u, "completed stored inventory was replayed again");
}

void EveryCloserDrains() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    std::atomic<int> runs{0};
    PipelinePreparation preparation(store, scheduler.Callback(), [&](const PipelineRecipe&) { ++runs; }, [] { return true; }, {1u, 1024u * 1024u});
    const auto recipe = Fixture();
    Require(preparation.Prepare(recipe), "queued replay was not admitted before teardown");
    std::promise<void> firstClosed;
    auto firstDone = firstClosed.get_future();
    std::thread first([&] {
        try { preparation.Close(); firstClosed.set_value(); }
        catch (...) { firstClosed.set_exception(std::current_exception()); }
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool closedAdmission = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!preparation.Prepare(recipe)) { closedAdmission = true; break; }
        std::this_thread::yield();
    }
    std::promise<void> secondClosed;
    auto secondDone = secondClosed.get_future();
    std::thread second([&] {
        try { preparation.Close(); secondClosed.set_value(); }
        catch (...) { secondClosed.set_exception(std::current_exception()); }
    });
    const bool secondReturnedEarly = secondDone.wait_for(20ms) == std::future_status::ready;
    scheduler.Run(0u);
    first.join();
    second.join();
    Require(closedAdmission, "teardown did not close admission");
    Require(!secondReturnedEarly, "a concurrent closer returned before queued work settled");
    firstDone.get();
    secondDone.get();
    const auto counters = preparation.Counters();
    Require(counters.cancelled == 1u && counters.failed == 0u && counters.ready == 0u && counters.retainedBytes == 0u && runs == 0, "teardown did not cancel and release queued replay");
}

void CloseDuringSubmission() {
    Directory directory;
    auto store = std::make_shared<PipelineRecipeStore>(directory.path);
    Scheduler scheduler;
    const auto enqueue = scheduler.Callback();
    std::promise<void> scheduling;
    std::promise<void> publish;
    const auto publication = publish.get_future().share();
    std::promise<void> closing;
    std::promise<void> closed;
    auto completion = closed.get_future();
    PipelinePreparation preparation(store, [&](const std::string& key, std::size_t bytes, std::function<void()> work) {
        scheduling.set_value();
        Require(publication.wait_for(2s) == std::future_status::ready, "submission publication was not released");
        return enqueue(key, bytes, std::move(work));
    }, [](const PipelineRecipe&) {}, [] { return true; }, {1u, 1024u * 1024u});
    std::exception_ptr submitError;
    std::thread submitter([&] {
        try {
            preparation.Prepare(Fixture());
        } catch (...) {
            submitError = std::current_exception();
        }
    });
    const bool started = scheduling.get_future().wait_for(2s) == std::future_status::ready;
    std::thread closer([&] {
        closing.set_value();
        try {
            preparation.Close();
            closed.set_value();
        } catch (...) {
            closed.set_exception(std::current_exception());
        }
    });
    closing.get_future().wait();
    const bool returnedBeforePublication = completion.wait_for(20ms) == std::future_status::ready;
    publish.set_value();
    submitter.join();
    if (scheduler.Count() != 0u) scheduler.Run(0u);
    closer.join();
    Require(started, "submission did not reach the scheduler");
    Require(!returnedBeforePublication, "close returned before in-progress scheduling was published and drained");
    completion.get();
    if (submitError != nullptr) std::rethrow_exception(submitError);
    const auto counters = preparation.Counters();
    Require(counters.pending == 0u && counters.retainedBytes == 0u, "teardown retained a submitted recipe");
}

}

int main() {
    try {
        SingleflightAndAccounting();
        ByteBudgetRefusal();
        FailedCompletion();
        SynchronousSubmissionFailure();
        CaptureAnalysisAllocationFailure();
        FailedPromiseAllocationFailure();
        GuardAndClosedAdmission();
        CompletedBeforeClose();
        CaptureUsesScheduler();
        SchedulerCancellation();
        StoredAdmissionRetries();
        EveryCloserDrains();
        CloseDuringSubmission();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pipeline preparation test: %s\n", error.what());
        return 1;
    }
}

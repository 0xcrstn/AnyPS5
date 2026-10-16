#include "prx/libSceAgcDriver/Graphics/include/PipelinePreparation.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeStore.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeGenerators.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

namespace AgcDriver::Graphics {

struct PipelinePreparation::State {
    struct Job {
        std::vector<std::byte> identity;
        std::shared_future<void> completion;
        std::size_t bytes = 0;
        bool capture = false;
        std::shared_ptr<std::atomic<bool>> skipped;
    };
    std::shared_ptr<PipelineRecipeStore> store;
    Schedule schedule;
    Replay replay;
    WaitAllowed waitAllowed;
    Limits limits;
    std::mutex inventoryMutex;
    std::vector<std::string> inventory;
    std::size_t nextStored = 0;
    bool inventoryLoaded = false;
    std::mutex mutex;
    std::condition_variable submitted;
    std::size_t submitting = 0;
    std::map<std::string, Job> jobs;
    std::atomic<bool> closed{false};
    Statistics stats;

    void collect() {
        for (auto it = jobs.begin(); it != jobs.end();) {
            auto& job = it->second;
            if (!job.completion.valid() || job.completion.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            try {
                job.completion.get();
                if (job.skipped->load()) ++stats.cancelled;
                else if (job.capture) ++stats.captured;
                else ++stats.ready;
            } catch (const PipelinePreparationCancelled&) {
                ++stats.cancelled;
            } catch (...) {
                if (job.capture) ++stats.captureFailed;
                else ++stats.failed;
            }
            stats.retainedBytes -= job.bytes;
            it = jobs.erase(it);
        }
        stats.pending = jobs.size();
    }
};

PipelinePreparation::PipelinePreparation(std::shared_ptr<PipelineRecipeStore> store, Schedule schedule, Replay replay, WaitAllowed waitAllowed, Limits limits) : state(std::make_shared<State>()) {
    Require(store != nullptr && bool(schedule) && bool(replay) && bool(waitAllowed), "pipeline preparation requires a store, scheduler, replay and wait guard");
    Require(limits.maximumJobs != 0 && limits.maximumBytes != 0, "pipeline preparation limits must be nonzero");
    state->store = std::move(store);
    state->schedule = std::move(schedule);
    state->replay = std::move(replay);
    state->waitAllowed = std::move(waitAllowed);
    state->limits = limits;
}

PipelinePreparation::~PipelinePreparation() {
    Close();
}

bool PipelinePreparation::Prepare(const PipelineRecipe& recipe) {
    return submit(recipe, false);
}

bool PipelinePreparation::submit(const PipelineRecipe& recipe, bool capture, bool* retryable) {
    if (retryable != nullptr) *retryable = false;
    {
        std::lock_guard lock(state->mutex);
        state->collect();
        if (state->closed.load()) return false;
        if (capture && (state->jobs.size() >= state->limits.maximumJobs || state->stats.retainedBytes >= state->limits.maximumBytes)) {
            ++state->stats.deferred;
            if (retryable != nullptr) *retryable = true;
            return false;
        }
    }
    static_cast<void>(MinimumPipelineRecipeBytes(recipe));
    auto identity = EncodePipelineRecipe(recipe);
    const auto key = std::string(capture ? "graphics-capture:" : "graphics-replay:") + PipelineRecipeIdentity(identity);
    const auto bytes = identity.size() * 8u;
    const auto skipped = std::make_shared<std::atomic<bool>>(false);
    struct Submission {
        State& state;
        const std::string& key;
        std::size_t bytes;
        bool capture;
        bool active = false;
        ~Submission() {
            if (!active) return;
            {
                std::lock_guard lock(state.mutex);
                state.jobs.erase(key);
                state.stats.retainedBytes -= bytes;
                state.stats.pending = state.jobs.size();
                if (!capture) ++state.stats.failed;
                --state.submitting;
            }
            state.submitted.notify_all();
        }
    } submission{*state, key, bytes, capture};
    {
        std::lock_guard lock(state->mutex);
        state->collect();
        if (state->closed.load()) return false;
        if (const auto found = state->jobs.find(key); found != state->jobs.end()) return found->second.identity == identity;
        if (bytes > state->limits.maximumBytes) {
            ++state->stats.deferred;
            return false;
        }
        if (state->jobs.size() >= state->limits.maximumJobs || bytes > state->limits.maximumBytes - state->stats.retainedBytes) {
            ++state->stats.deferred;
            if (retryable != nullptr) *retryable = true;
            return false;
        }
        state->jobs.emplace(key, State::Job{std::move(identity), {}, bytes, capture, skipped});
        ++state->submitting;
        submission.active = true;
        state->stats.retainedBytes += bytes;
        state->stats.pending = state->jobs.size();
        ++state->stats.admitted;
        if (capture && std::ranges::any_of(recipe.stages, [](const auto& stage) { return stage.inputIdentity.empty(); })) {
            auto inputs = recipe;
            for (auto& stage : inputs.stages) if (stage.inputIdentity.empty()) stage.program.reset();
            for (std::size_t index = 0; index < inputs.stages.size(); ++index) {
                if (inputs.stages[index].inputIdentity.empty() && !IsDerivedPipelineStage(inputs, index)) {
                    ++state->stats.incompleteInputs;
                    break;
                }
            }
        }
    }
    std::shared_future<void> completion;
    try {
        auto owned = std::make_shared<const PipelineRecipe>(recipe);
        completion = state->schedule(key, bytes, [shared = state, owned, capture, skipped] {
            if (shared->closed.load()) {
                skipped->store(true);
                return;
            }
            if (capture) {
                const auto status = shared->store->Store(*owned);
                if (status != PipelineRecipeStoreStatus::Stored && status != PipelineRecipeStoreStatus::Present) throw std::runtime_error("graphics pipeline recipe capture failed");
            } else shared->replay(*owned);
        });
        if (!completion.valid()) throw std::runtime_error("pipeline preparation scheduler returned an invalid completion");
    } catch (...) {
        std::promise<void> failed;
        failed.set_exception(std::current_exception());
        completion = failed.get_future().share();
    }
    {
        std::lock_guard lock(state->mutex);
        state->jobs.at(key).completion = std::move(completion);
        --state->submitting;
        submission.active = false;
    }
    state->submitted.notify_all();
    return true;
}

std::size_t PipelinePreparation::PrepareStored(std::size_t maximum) {
    std::lock_guard inventoryLock(state->inventoryMutex);
    if (state->closed.load()) return 0;
    if (!state->inventoryLoaded) {
        state->inventory = state->store->List(4096);
        state->inventoryLoaded = true;
    }
    const auto end = state->nextStored + std::min({maximum, state->limits.maximumJobs, state->inventory.size() - state->nextStored});
    std::size_t admitted = 0;
    while (state->nextStored < end) {
        PipelineRecipe recipe;
        const auto identity = state->inventory[state->nextStored];
        if (state->store->Load(identity, recipe) != PipelineRecipeStoreStatus::Loaded) {
            std::lock_guard lock(state->mutex);
            ++state->stats.failed;
            ++state->nextStored;
            continue;
        }
        bool retryable = false;
        if (submit(recipe, false, &retryable)) {
            ++admitted;
            ++state->nextStored;
        } else if (retryable) break;
        else {
            ++state->nextStored;
            std::lock_guard lock(state->mutex);
            ++state->stats.failed;
        }
    }
    {
        std::lock_guard lock(state->mutex);
        state->stats.storedRemaining = state->closed.load() ? 0u : state->inventory.size() - state->nextStored;
    }
    return admitted;
}

void PipelinePreparation::Capture(const PipelineRecipe& recipe) noexcept {
    if (state->closed.load()) return;
    try {
        submit(recipe, true);
    } catch (...) {
        std::lock_guard lock(state->mutex);
        ++state->stats.captureFailed;
    }
}

bool PipelinePreparation::CanCapture() noexcept {
    std::lock_guard lock(state->mutex);
    state->collect();
    if (state->closed.load()) return false;
    if (state->jobs.size() >= state->limits.maximumJobs || state->stats.retainedBytes >= state->limits.maximumBytes) {
        ++state->stats.deferred;
        return false;
    }
    return true;
}

void PipelinePreparation::RecordCaptureFailure() noexcept {
    std::lock_guard lock(state->mutex);
    ++state->stats.captureFailed;
}

PipelinePreparation::Statistics PipelinePreparation::Checkpoint() {
    Require(state->waitAllowed(), "pipeline preparation checkpoint cannot wait under GpuMutex");
    std::vector<std::shared_future<void>> completions;
    {
        std::unique_lock lock(state->mutex);
        state->submitted.wait(lock, [&] { return state->submitting == 0; });
        for (const auto& [key, job] : state->jobs) {
            completions.push_back(job.completion);
        }
    }
    for (const auto& completion : completions) completion.wait();
    return Counters();
}

PipelinePreparation::Statistics PipelinePreparation::Counters() {
    std::lock_guard lock(state->mutex);
    state->collect();
    return state->stats;
}

void PipelinePreparation::Close() {
    state->closed.store(true);
    {
        std::lock_guard lock(state->mutex);
        state->collect();
        state->stats.storedRemaining = 0;
        if (state->jobs.empty() && state->submitting == 0) return;
    }
    Checkpoint();
}

}

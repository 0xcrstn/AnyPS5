#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINEPREPARATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINEPREPARATION_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <stdexcept>

namespace AgcDriver::Graphics {

struct PipelineRecipe;
class PipelineRecipeStore;

class PipelinePreparationCancelled : public std::runtime_error {
public:
    PipelinePreparationCancelled() : std::runtime_error("pipeline preparation cancelled") {}
};

class PipelinePreparation {
public:
    using Schedule = std::function<std::shared_future<void>(const std::string&, std::size_t, std::function<void()>)>;
    using Replay = std::function<void(const PipelineRecipe&)>;
    using WaitAllowed = std::function<bool()>;
    struct Limits {
        std::size_t maximumJobs = 128;
        std::size_t maximumBytes = std::size_t{256} << 20u;
    };
    struct Statistics {
        std::uint64_t admitted = 0, ready = 0, failed = 0, cancelled = 0, deferred = 0, captured = 0, captureFailed = 0, incompleteInputs = 0;
        std::size_t pending = 0, retainedBytes = 0, storedRemaining = 0;
    };
    PipelinePreparation(std::shared_ptr<PipelineRecipeStore> store, Schedule schedule, Replay replay, WaitAllowed waitAllowed, Limits limits);
    ~PipelinePreparation();
    PipelinePreparation(const PipelinePreparation&) = delete;
    PipelinePreparation& operator=(const PipelinePreparation&) = delete;
    bool Prepare(const PipelineRecipe& recipe);
    std::size_t PrepareStored(std::size_t maximum);
    void Capture(const PipelineRecipe& recipe) noexcept;
    void RecordCaptureFailure() noexcept;
    bool CanCapture() noexcept;
    Statistics Checkpoint();
    Statistics Counters();
    void Close();

private:
    bool submit(const PipelineRecipe& recipe, bool capture, bool* retryable = nullptr);
    struct State;
    std::shared_ptr<State> state;
};

}

#endif

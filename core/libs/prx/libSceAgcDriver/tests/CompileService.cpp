#include "prx/libSceAgcDriver/Execution/include/CompileService.hpp"
#include "prx/libSceAgcDriver/Execution/include/ComputePipelineAheadRequest.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver;
using namespace std::chrono_literals;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Gate {
    std::binary_semaphore entered{0}, release{0};
    std::atomic<bool> opened{false};
    ~Gate() { Open(); }
    void Open() { if (!opened.exchange(true)) release.release(); }
    void Block() {
        entered.release();
        Require(release.try_acquire_for(5s), "compile service test gate timed out");
    }
    void Ready() { Require(entered.try_acquire_for(5s), "compile service worker did not start"); }
};

CompileService::Limits Single() {
    CompileService::Limits limits;
    limits.workers = 1;
    limits.demandReservedWorkers = 0;
    limits.aging = std::chrono::milliseconds(0);
    return limits;
}

struct Order {
    std::mutex mutex;
    std::vector<std::string> names;
    void Add(std::string name) {
        std::lock_guard lock(mutex);
        names.push_back(std::move(name));
    }
};

void Deduplication() {
    CompileService service(Single());
    Gate gate;
    std::atomic<int> runs{0};
    auto blocker = service.Submit("blocker", CompilePriority::Registration, 1, [&](const CompileTicket&) { gate.Block(); });
    gate.Ready();
    std::vector<std::shared_ptr<CompileTicket>> tickets;
    std::vector<std::thread> threads;
    std::mutex ticketsMutex;
    for (int thread = 0; thread < 8; ++thread) {
        threads.emplace_back([&] {
            for (int request = 0; request < 500; ++request) {
                auto ticket = service.Submit("same", CompilePriority::Predicted, 1, [&](const CompileTicket&) { runs.fetch_add(1); });
                std::lock_guard lock(ticketsMutex);
                tickets.push_back(std::move(ticket));
            }
        });
    }
    for (auto& thread : threads) thread.join();
    for (const auto& ticket : tickets) Require(ticket.get() == tickets.front().get(), "duplicate requests received different tickets");
    gate.Open();
    tickets.front()->Wait();
    blocker->Wait();
    Require(runs.load() == 1, "duplicate requests ran the job more than once");
    const auto counters = service.Counters();
    Require(counters.deduplicated == 3999 && counters.submitted == 2, "deduplication counters are wrong");
}

void PriorityOrder() {
    CompileService service(Single());
    Gate gate;
    Order order;
    auto blocker = service.Submit("blocker", CompilePriority::Demand, 1, [&](const CompileTicket&) { gate.Block(); });
    gate.Ready();
    auto registration = service.Submit("registration", CompilePriority::Registration, 1, [&](const CompileTicket&) { order.Add("registration"); });
    auto predicted = service.Submit("predicted", CompilePriority::Predicted, 1, [&](const CompileTicket&) { order.Add("predicted"); });
    auto upcoming = service.Submit("upcoming", CompilePriority::Upcoming, 1, [&](const CompileTicket&) { order.Add("upcoming"); });
    auto promoted = service.Submit("promoted", CompilePriority::Registration, 1, [&](const CompileTicket&) { order.Add("promoted"); });
    service.Promote(*promoted, CompilePriority::Demand);
    Require(promoted->Priority() == CompilePriority::Demand, "promotion did not raise the priority");
    service.Promote(*promoted, CompilePriority::Registration);
    Require(promoted->Priority() == CompilePriority::Demand, "promotion lowered a priority");
    gate.Open();
    registration->Wait();
    predicted->Wait();
    upcoming->Wait();
    promoted->Wait();
    const std::vector<std::string> expected{"promoted", "upcoming", "predicted", "registration"};
    Require(order.names == expected, "jobs did not run in priority order");
}

void Inheritance() {
    CompileService service(Single());
    Gate gate;
    Order order;
    auto blocker = service.Submit("blocker", CompilePriority::Demand, 1, [&](const CompileTicket&) { gate.Block(); });
    gate.Ready();
    auto other = service.Submit("other", CompilePriority::Upcoming, 1, [&](const CompileTicket&) { order.Add("other"); });
    auto part = service.Submit("part", CompilePriority::Registration, 1, [&](const CompileTicket&) { order.Add("part"); });
    auto pipeline = service.Submit("pipeline", CompilePriority::Registration, 1, [&](const CompileTicket&) { order.Add("pipeline"); }, {part});
    service.Promote(*pipeline, CompilePriority::Demand);
    Require(part->Priority() == CompilePriority::Demand, "promotion did not reach the dependency");
    gate.Open();
    pipeline->Wait();
    other->Wait();
    const std::vector<std::string> expected{"part", "pipeline", "other"};
    Require(order.names == expected, "dependency did not run first or the promoted chain did not run before other work");
}

void DemandSlot() {
    CompileService::Limits limits;
    limits.workers = 2;
    limits.demandReservedWorkers = 1;
    CompileService service(limits);
    Gate first, demandGate;
    std::atomic<bool> secondStarted{false};
    auto speculative = service.Submit("speculative", CompilePriority::Registration, 1, [&](const CompileTicket&) { first.Block(); });
    first.Ready();
    auto second = service.Submit("second", CompilePriority::Predicted, 1, [&](const CompileTicket&) { secondStarted = true; });
    std::this_thread::sleep_for(50ms);
    Require(!secondStarted.load(), "speculative work took the slot reserved for demand");
    auto demand = service.Submit("demand", CompilePriority::Demand, 1, [&](const CompileTicket&) {});
    Require(demand->WaitFor(5s), "demand did not run while speculative work held the other slot");
    Require(!secondStarted.load(), "speculative work ran in the demand slot");
    first.Open();
    second->Wait();
    speculative->Wait();
}

void Budget() {
    CompileService::Limits limits;
    limits.workers = 3;
    limits.demandReservedWorkers = 1;
    limits.budgetBytes = 100;
    CompileService service(limits);
    Gate gate;
    std::atomic<bool> bigStarted{false};
    auto holder = service.Submit("holder", CompilePriority::Registration, 80, [&](const CompileTicket&) { gate.Block(); });
    gate.Ready();
    auto big = service.Submit("big", CompilePriority::Registration, 40, [&](const CompileTicket&) { bigStarted = true; });
    std::this_thread::sleep_for(50ms);
    Require(!bigStarted.load(), "speculative work over the reservation budget was admitted");
    auto demand = service.Submit("demand", CompilePriority::Demand, 40, [&](const CompileTicket&) {});
    Require(demand->WaitFor(5s), "demand was refused by the speculative budget");
    gate.Open();
    big->Wait();
    holder->Wait();
    const auto counters = service.Counters();
    Require(counters.budgetDeferrals != 0 && counters.demandOverBudget == 1 && counters.reservedBytes == 0 && counters.peakReservedBytes == 120, "budget counters are wrong");
}

void Cancellation() {
    CompileService service(Single());
    Gate gate;
    std::atomic<bool> observed{false};
    auto running = service.Submit("running", CompilePriority::Registration, 1, [&](const CompileTicket& ticket) {
        gate.Block();
        if (ticket.CancellationRequested()) {
            observed = true;
            throw CompileCancelled();
        }
    });
    gate.Ready();
    auto queued = service.Submit("queued", CompilePriority::Registration, 1, [&](const CompileTicket&) { throw std::runtime_error("cancelled job ran"); });
    auto upcoming = service.Submit("upcoming", CompilePriority::Upcoming, 1, [&](const CompileTicket&) {});
    Require(service.Cancel(*queued), "queued job could not be cancelled");
    Require(queued->State() == CompileState::Cancelled, "cancelled job is not in the cancelled state");
    bool threw = false;
    try {
        queued->Wait();
    } catch (const CompileCancelled&) {
        threw = true;
    }
    Require(threw, "waiting on a cancelled job did not throw");
    auto speculative = service.Submit("speculative", CompilePriority::Predicted, 1, [&](const CompileTicket&) { throw std::runtime_error("speculative job ran"); });
    Require(service.CancelSpeculative() == 1, "speculative cancellation did not cancel exactly the predicted job");
    Require(speculative->State() == CompileState::Cancelled && upcoming->State() == CompileState::Queued, "speculative cancellation touched the wrong jobs");
    Require(service.Cancel(*running), "running job could not be asked to cancel");
    gate.Open();
    upcoming->Wait();
    Require(running->State() == CompileState::Cancelled && observed.load(), "running job did not observe its cancellation");
    auto again = service.Submit("queued", CompilePriority::Registration, 1, [&](const CompileTicket&) {});
    Require(again.get() != queued.get(), "a cancelled key was not resubmitted as a new job");
    again->Wait();
}

void Failure() {
    CompileService service(Single());
    auto failing = service.Submit("failing", CompilePriority::Demand, 1, [&](const CompileTicket&) { throw std::invalid_argument("driver rejected the module"); });
    auto dependent = service.Submit("dependent", CompilePriority::Demand, 1, [&](const CompileTicket&) { throw std::runtime_error("dependent ran after a failure"); }, {failing});
    bool sameError = false;
    try {
        failing->Wait();
    } catch (const std::invalid_argument& error) {
        sameError = std::string_view(error.what()) == "driver rejected the module";
    }
    Require(sameError && failing->State() == CompileState::Failed, "failure did not rethrow the job's own exception");
    bool dependentFailed = false;
    try {
        dependent->Wait();
    } catch (const std::runtime_error& error) {
        dependentFailed = std::string_view(error.what()).find("dependency failed") != std::string_view::npos;
    }
    Require(dependentFailed && dependent->State() == CompileState::Failed, "dependent of a failed job did not fail");
    auto retry = service.Submit("failing", CompilePriority::Demand, 1, [&](const CompileTicket&) {});
    Require(retry.get() == failing.get() && retry->State() == CompileState::Failed, "a failed key silently restarted");
}

void Teardown() {
    std::atomic<int> demandRuns{0};
    std::shared_ptr<CompileTicket> demand, speculative;
    {
        CompileService service(Single());
        Gate gate;
        auto blocker = service.Submit("blocker", CompilePriority::Registration, 1, [&](const CompileTicket&) { gate.Block(); });
        gate.Ready();
        demand = service.Submit("demand", CompilePriority::Demand, 1, [&](const CompileTicket&) { demandRuns.fetch_add(1); });
        speculative = service.Submit("speculative", CompilePriority::Registration, 1, [&](const CompileTicket&) { throw std::runtime_error("speculative job ran during teardown"); });
        std::thread closer([&] { service.Close(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (speculative->State() != CompileState::Cancelled) {
            Require(std::chrono::steady_clock::now() < deadline, "close did not cancel queued speculative work");
            std::this_thread::yield();
        }
        gate.Open();
        closer.join();
        bool refused = false;
        try {
            service.Submit("late", CompilePriority::Demand, 1, [](const CompileTicket&) {});
        } catch (const std::logic_error&) {
            refused = true;
        }
        Require(refused, "a closed service accepted a job");
    }
    Require(demandRuns.load() == 1 && demand->State() == CompileState::Ready, "teardown dropped a demanded job");
    Require(speculative->State() == CompileState::Cancelled, "teardown did not cancel speculative work");
}

std::atomic<bool> guardHeld{false};

void WaitGuard() {
    CompileTicket::SetWaitGuard([] { return guardHeld.load(); });
    CompileService service(Single());
    std::atomic<int> runs{0};
    auto ticket = service.Submit("guarded", CompilePriority::Demand, 1, [&](const CompileTicket&) { runs.fetch_add(1); });
    guardHeld = true;
    bool forbidden = false;
    try {
        ticket->Wait();
    } catch (const CompileWaitForbidden&) {
        forbidden = true;
    }
    Require(forbidden, "a wait under the guard was allowed");
    forbidden = false;
    try {
        service.RunAndWait("run-and-wait", 1, [&](const CompileTicket&) { runs.fetch_add(100); });
    } catch (const CompileWaitForbidden&) {
        forbidden = true;
    }
    Require(forbidden && service.Find("run-and-wait") == nullptr, "RunAndWait submitted work while waiting was forbidden");
    guardHeld = false;
    ticket->Wait();
    Require(runs.load() == 1, "guarded job ran a wrong number of times");
    CompileTicket::SetWaitGuard(nullptr);
}

ShaderRecompiler::RecompileResult Source(std::shared_ptr<std::vector<std::uint32_t>> words) {
    ShaderRecompiler::RecompileResult source;
    source.spirv = ShaderRecompiler::SharedSpirv(words, *words);
    source.variantId = 17;
    source.specialization = {{7, 0x12345678u}};
    source.hostSubgroupSize = 32;
    source.workgroupMemoryDwords = 64;
    source.runtimeAbiVersion = 5;
    source.pushConstants = {std::byte{0x42}};
    ShaderRecompiler::DescriptorBinding binding{};
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    binding.binding = 3;
    binding.count = 1;
    source.bindings.push_back(binding);
    return source;
}

void RecipeVerification() {
    auto words = std::make_shared<std::vector<std::uint32_t>>(std::initializer_list<std::uint32_t>{0x07230203u, 1, 2, 3, 4});
    const auto source = Source(words);
    const std::vector<std::uint32_t> layout{3, 7, 1, 32};
    const auto request = ComputePipelineAheadRequest::Capture(source, ComputePipelineAheadRequest::EstimateBytes(source));
    Require(request != nullptr, "request capture failed");
    const auto built = ComputePipelineRecipeHash(request->shader, request->pushes, layout);
    const auto demanded = ComputePipelineRecipeHash(source, true, layout);
    RequireSameComputeRecipe(built, demanded, 0x1000);
    const auto expectMismatch = [&](std::uint64_t other, const char* message) {
        bool threw = false;
        try {
            RequireSameComputeRecipe(built, other, 0x1000);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        Require(threw, message);
    };
    auto changedWords = std::make_shared<std::vector<std::uint32_t>>(*words);
    (*changedWords)[4] = 99;
    expectMismatch(ComputePipelineRecipeHash(Source(changedWords), true, layout), "an injected SPIR-V word mismatch passed verification");
    auto changedConstant = Source(words);
    changedConstant.specialization.front().value = 0;
    expectMismatch(ComputePipelineRecipeHash(changedConstant, true, layout), "an injected specialization mismatch passed verification");
    expectMismatch(ComputePipelineRecipeHash(source, false, layout), "an injected push-constant mismatch passed verification");
    const std::vector<std::uint32_t> otherLayout{3, 7, 2, 32};
    expectMismatch(ComputePipelineRecipeHash(source, true, otherLayout), "an injected layout mismatch passed verification");
    auto changedBinding = Source(words);
    changedBinding.bindings.front().binding = 4;
    expectMismatch(ComputePipelineRecipeHash(changedBinding, true, layout), "an injected binding mismatch passed verification");
}

}

int main() {
    try {
        Deduplication();
        PriorityOrder();
        Inheritance();
        DemandSlot();
        Budget();
        Cancellation();
        Failure();
        Teardown();
        WaitGuard();
        RecipeVerification();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "compile service test failed: %s\n", error.what());
        return 1;
    }
    std::puts("compile service tests passed");
    return 0;
}

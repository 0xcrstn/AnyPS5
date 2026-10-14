#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <barrier>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("preparation thread priority: " + message);
}

#ifdef _WIN32
int CurrentPriority() {
    return GetThreadPriority(GetCurrentThread());
}
constexpr int LoweredPriority = THREAD_PRIORITY_BELOW_NORMAL;
constexpr int DefaultPriority = THREAD_PRIORITY_NORMAL;
#elif defined(__linux__)
int CurrentPriority() {
    return getpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)));
}
constexpr int LoweredPriority = 5;
constexpr int DefaultPriority = 0;
#endif

}

int main() {
#if defined(_WIN32) || defined(__linux__)
    try {
        const auto workers = AgcDriver::DriverDetail::RegistrationPreparationThreads();
        Require(workers != 0, "no preparation threads");
        std::barrier started(static_cast<std::ptrdiff_t>(workers + 1u));
        std::latch finished(static_cast<std::ptrdiff_t>(workers));
        std::mutex mutex;
        std::vector<std::pair<std::thread::id, int>> seen;
        for (unsigned index = 0; index < workers; ++index) {
            AgcDriver::DriverDetail::SubmitRegistrationPreparation([&] {
                started.arrive_and_wait();
                {
                    std::lock_guard lock(mutex);
                    seen.emplace_back(std::this_thread::get_id(), CurrentPriority());
                }
                finished.count_down();
            });
        }
        started.arrive_and_wait();
        finished.wait();
        Require(seen.size() == workers, "not every preparation thread ran a task");
        for (const auto& [thread, priority] : seen) {
            Require(thread != std::this_thread::get_id(), "a preparation ran on the submitting thread");
            Require(priority == LoweredPriority, "a preparation thread runs at priority " + std::to_string(priority) + " instead of " + std::to_string(LoweredPriority));
        }
        Require(CurrentPriority() == DefaultPriority, "the submitting thread's priority changed");
        std::printf("preparation thread priority tests passed (%u threads)\n", workers);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#else
    std::printf("preparation thread priority is not lowered on this platform; skipped\n");
    return 77;
#endif
}

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include "prx/libc/include/Shutdown.hpp"
#include "AudioOutShutdown.hpp"

namespace {

std::atomic<std::uint32_t> inside{0};
std::atomic<bool> stopping{false};

std::mutex& InsideMutex() {
    static auto* const mutex = new std::mutex();
    return *mutex;
}

std::condition_variable& InsideChanged() {
    static auto* const changed = new std::condition_variable();
    return *changed;
}

void Leave() {
    if (inside.fetch_sub(1) != 1) return;
    { std::lock_guard lock(InsideMutex()); }
    InsideChanged().notify_all();
}

void Stop() {
    stopping.store(true);
    std::unique_lock lock(InsideMutex());
    InsideChanged().wait(lock, [] { return inside.load() == 0; });
}

}

AudioOutCall::AudioOutCall() {
    static const bool registered = (LibcRegisterShutdown_nid_postfix(Stop), true);
    static_cast<void>(registered);
    inside.fetch_add(1);
    if (stopping.load() || LibcShutdownToken_nid_postfix().stop_requested()) {
        Leave();
        throw ProcessShutdown{};
    }
}

AudioOutCall::~AudioOutCall() {
    Leave();
}

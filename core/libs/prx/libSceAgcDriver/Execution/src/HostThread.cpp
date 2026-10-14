#include "prx/libSceAgcDriver/Execution/include/HostThread.hpp"
#include <memory>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include <limits>
#else
#include <pthread.h>
#endif

namespace AgcDriver {

#ifdef _WIN32
namespace {

struct Start {
    HostThreadEntry entry;
    void* argument;
};

unsigned __stdcall RunHostThread(void* argument) {
    const std::unique_ptr<Start> start(static_cast<Start*>(argument));
    start->entry(start->argument);
    return 0;
}

}

bool StartDetachedHostThread(HostThreadEntry entry, void* argument, std::size_t stackReserveBytes) {
    if (stackReserveBytes > std::numeric_limits<unsigned>::max()) return false;
    auto start = std::make_unique<Start>(Start{entry, argument});
    const auto handle = _beginthreadex(nullptr, static_cast<unsigned>(stackReserveBytes), &RunHostThread, start.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle == 0) return false;
    start.release();
    CloseHandle(reinterpret_cast<HANDLE>(handle));
    return true;
}
#else
bool StartDetachedHostThread(HostThreadEntry entry, void* argument, std::size_t stackReserveBytes) {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    pthread_t thread;
    const bool started = pthread_attr_setstacksize(&attributes, stackReserveBytes) == 0 && pthread_create(&thread, &attributes, entry, argument) == 0;
    pthread_attr_destroy(&attributes);
    if (started) pthread_detach(thread);
    return started;
}
#endif

}

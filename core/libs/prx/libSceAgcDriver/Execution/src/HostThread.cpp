#include "prx/libSceAgcDriver/Execution/include/HostThread.hpp"
#include <cstring>
#include <memory>
#include <string>
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

void NameHostThread(const char* name) {
    std::wstring wide;
    for (const char* cursor = name; *cursor != '\0'; ++cursor) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
    SetThreadDescription(GetCurrentThread(), wide.c_str());
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

void NameHostThread(const char* name) {
#ifdef __linux__
    char truncated[16];
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = '\0';
    pthread_setname_np(pthread_self(), truncated);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    (void)name;
#endif
}
#endif

}

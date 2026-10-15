#include "prx/libSceAgcDriver/Execution/include/OrderedMutex.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace AgcDriver {

namespace {

const void* ThreadIdentity() {
    thread_local const char identity = 0;
    return &identity;
}

struct HeldLock {
    const void* identity;
    LockLevel level;
    std::uint64_t rank;
    const char* name;
    const void* site;
};

ThreadList<HeldLock, 32>& Held() {
    thread_local ThreadList<HeldLock, 32> held;
    return held;
}

std::atomic<int>& VerifyOverride() {
    static std::atomic<int> value{-1};
    return value;
}

std::string Backtrace(const void* caller) {
    std::string text;
#ifdef _WIN32
    void* frames[12] = {};
    const auto count = CaptureStackBackTrace(2, 12, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(frames[i]), &module)) GetModuleFileNameA(module, path, sizeof(path));
        const char* name = std::strrchr(path, '\\');
        char frame[160];
        std::snprintf(frame, sizeof(frame), " %s+0x%llx", name != nullptr ? name + 1 : path, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(module)));
        text += frame;
    }
#endif
    if (caller != nullptr) text += " (caller " + DescribeCode(caller) + ")";
    return text;
}

const char* ConfiguredValue() {
    static const char* value = std::getenv("APS5_LOCK_ORDER_VERIFY");
    return value;
}

bool ConfiguredVerify() {
    static const bool verify = [] {
        const char* value = ConfiguredValue();
        if (value == nullptr || std::strcmp(value, "0") == 0) return false;
        if (std::strcmp(value, "1") == 0 || std::strcmp(value, "queue") == 0) return true;
        throw std::runtime_error(std::string("APS5_LOCK_ORDER_VERIFY: expected 0, 1 or queue, got ") + value);
    }();
    return verify;
}

}

std::string DescribeCode(const void* address) {
#ifdef _WIN32
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, path, sizeof(path));
    const char* name = std::strrchr(path, '\\');
    char text[160];
    std::snprintf(text, sizeof(text), "%s+0x%llx", name != nullptr ? name + 1 : path, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module)));
    return text;
#else
    char text[32];
    std::snprintf(text, sizeof(text), "%p", address);
    return text;
#endif
}

namespace {

const void*& ThreadLockSite() {
    thread_local const void* site = nullptr;
    return site;
}

}

void SetLockSite(const void* site) {
    ThreadLockSite() = site;
}

const void* LockSite() {
    return ThreadLockSite();
}

const char* LockLevelName(LockLevel level) {
    switch (level) {
        case LockLevel::Device: return "device";
        case LockLevel::Queue: return "queue";
        case LockLevel::Recorder: return "recorder";
        case LockLevel::Slice: return "slice";
        case LockLevel::Owner: return "owner";
        case LockLevel::CacheShard: return "cache shard";
        case LockLevel::Leaf: return "leaf";
    }
    return "?";
}

bool LockOrderVerify() {
    const auto value = VerifyOverride().load(std::memory_order_acquire);
    return value < 0 ? ConfiguredVerify() : value != 0;
}

bool LockOrderQueueScoped() {
    static const bool scoped = ConfiguredVerify() && std::strcmp(ConfiguredValue(), "queue") == 0;
    return scoped;
}

void SetLockOrderVerifyForTests(bool verify) {
    VerifyOverride().store(verify ? 1 : 0, std::memory_order_release);
}

std::string HeldOrderedLocks() {
    std::string text;
    for (const auto& lock : Held()) {
        if (!text.empty()) text += ", ";
        text += std::string(lock.name) + " (" + LockLevelName(lock.level) + " " + std::to_string(lock.rank) + (lock.site != nullptr ? ", taken at " + DescribeCode(lock.site) : std::string()) + ")";
    }
    return text.empty() ? "nothing" : text;
}

void CheckLockOrder(LockLevel level, std::uint64_t rank, const char* name, const void* caller) {
    if (!LockOrderVerify()) return;
    for (const auto& held : Held()) {
        if (held.level < level || (held.level == level && held.rank < rank)) continue;
        throw std::logic_error(std::string("APS5_LOCK_ORDER_VERIFY: ") + name + " (" + LockLevelName(level) + " " + std::to_string(rank) + ") taken while holding " + HeldOrderedLocks() + "; at" + Backtrace(caller));
    }
}

void NoteLockAcquired(const void* identity, LockLevel level, std::uint64_t rank, const char* name, const void* site) {
    Held().push_back({identity, level, rank, name, site});
}

void NoteLockReleased(const void* identity) {
    auto& held = Held();
    for (auto* lock = held.end(); lock != held.begin();) {
        if ((--lock)->identity != identity) continue;
        held.erase(lock);
        return;
    }
}

void OrderedMutex::acquired(const void* site) {
    owner.store(ThreadIdentity(), std::memory_order_relaxed);
    if (++depth == 1) NoteLockAcquired(this, level, rank, name, site);
}

void OrderedMutex::lock() {
    LockFrom(__builtin_return_address(0));
}

void OrderedMutex::LockFrom(const void* site) {
    if (!HeldByThisThread()) CheckLockOrder(level, rank, name, site);
    mutex.lock();
    acquired(site);
}

void OrderedMutex::LockUnderDeviceLock() {
    mutex.lock();
    acquired();
}

bool OrderedMutex::try_lock() {
    if (!mutex.try_lock()) return false;
    acquired();
    return true;
}

void OrderedMutex::unlock() {
    if (--depth == 0) {
        owner.store(nullptr, std::memory_order_relaxed);
        NoteLockReleased(this);
    }
    mutex.unlock();
}

bool OrderedMutex::HeldByThisThread() const {
    return owner.load(std::memory_order_relaxed) == ThreadIdentity();
}

std::uint32_t OrderedMutex::DepthOnThisThread() const {
    return HeldByThisThread() ? depth : 0;
}

void OrderedMutex::AssertHeld(const char* what, const void* caller) const {
    if (!LockOrderVerify() || HeldByThisThread()) return;
    throw std::logic_error(std::string("APS5_LOCK_ORDER_VERIFY: ") + what + " runs without " + name + " (holding " + HeldOrderedLocks() + "); at" + Backtrace(caller));
}

}

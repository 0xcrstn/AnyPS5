#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ORDEREDMUTEX_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ORDEREDMUTEX_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

namespace AgcDriver {

enum class LockLevel : std::uint8_t { Device = 0, Queue = 1, Recorder = 2, Slice = 3, Owner = 4, CacheShard = 5, Leaf = 6 };

template<typename T, std::size_t Capacity>
class ThreadList {
public:
    T* begin() { return items.data(); }
    T* end() { return items.data() + count; }
    bool empty() const { return count == 0; }
    T& back() { return items[count - 1]; }
    void pop_back() { --count; }
    void push_back(const T& item) {
        if (count == Capacity) throw std::logic_error("a per-thread lock list overflowed");
        items[count++] = item;
    }
    void erase(T* item) {
        for (auto* next = item + 1; next != end(); ++next) *(next - 1) = *next;
        --count;
    }

private:
    std::array<T, Capacity> items;
    std::size_t count;
};

const char* LockLevelName(LockLevel level);
bool LockOrderVerify();
bool LockOrderQueueScoped();
void SetLockOrderVerifyForTests(bool verify);
void CheckLockOrder(LockLevel level, std::uint64_t rank, const char* name, const void* caller = nullptr);
std::string DescribeCode(const void* address);
void SetLockSite(const void* site);
const void* LockSite();
void NoteLockAcquired(const void* identity, LockLevel level, std::uint64_t rank, const char* name, const void* site = nullptr);
void NoteLockReleased(const void* identity);

class OrderedMutex {
public:
    OrderedMutex(LockLevel level, std::uint64_t rank, const char* name) : level(level), rank(rank), name(name) {}
    OrderedMutex(const OrderedMutex&) = delete;
    OrderedMutex& operator=(const OrderedMutex&) = delete;

    void lock();
    void LockFrom(const void* site);
    void LockUnderDeviceLock();
    bool try_lock();
    void unlock();
    bool HeldByThisThread() const;
    std::uint32_t DepthOnThisThread() const;
    void AssertHeld(const char* what, const void* caller = nullptr) const;
    LockLevel Level() const { return level; }
    std::uint64_t Rank() const { return rank; }
    const char* Name() const { return name; }

private:
    void acquired(const void* site = nullptr);
    const LockLevel level;
    const std::uint64_t rank;
    const char* const name;
    std::recursive_mutex mutex;
    std::atomic<const void*> owner{nullptr};
    std::uint32_t depth = 0;
};

std::string HeldOrderedLocks();

}

#endif

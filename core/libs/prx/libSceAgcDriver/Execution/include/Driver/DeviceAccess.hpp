#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEVICEACCESS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DEVICEACCESS_HPP

#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include "prx/libc/include/general/AtomicSharedPtr.hpp"

namespace AgcDriver::DriverDetail {

class DevicePointer {
public:
    std::shared_ptr<VulkanDevice> Load() const;
    operator std::shared_ptr<VulkanDevice>() const;
    DevicePointer& operator=(std::shared_ptr<VulkanDevice> value);
    void Reset();
    VulkanDevice* operator->() const;
    explicit operator bool() const;
    bool operator==(std::nullptr_t) const;

private:
    AtomicSharedPtr<VulkanDevice> pointer;
};

class QueueLock {
public:
    static constexpr std::uint32_t AllQueues = 0xffffffffu;
    QueueLock(const DevicePointer& device, std::uint32_t queue);
    QueueLock(const DevicePointer& device, std::uint32_t queue, std::defer_lock_t);
    QueueLock(const DevicePointer& device, std::uint32_t queue, std::try_to_lock_t);
    QueueLock(const QueueLock&) = delete;
    QueueLock& operator=(const QueueLock&) = delete;
    ~QueueLock();
    void lock();
    bool try_lock();
    void unlock();
    bool owns_lock() const { return owned; }

private:
    bool attach(bool tryOnly);
    const DevicePointer& device;
    const std::uint32_t queue;
    bool owned = false;
    std::shared_ptr<VulkanDevice> held;
    std::array<Graphics::Recorder*, 2> recorders{};
    std::size_t locked = 0;
};

class DeviceUseGate {
public:
    void lock_shared();
    void unlock_shared();
    void lock();
    void unlock();

private:
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t users = 0;
    bool replacing = false;
};

}

#endif

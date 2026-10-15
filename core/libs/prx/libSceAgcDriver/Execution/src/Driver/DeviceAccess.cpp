#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"

namespace AgcDriver::DriverDetail {

std::shared_ptr<VulkanDevice> DevicePointer::Load() const { return pointer.load(std::memory_order_acquire); }

DevicePointer::operator std::shared_ptr<VulkanDevice>() const { return Load(); }

DevicePointer& DevicePointer::operator=(std::shared_ptr<VulkanDevice> value) {
    pointer.store(std::move(value), std::memory_order_release);
    return *this;
}

void DevicePointer::Reset() { *this = nullptr; }

VulkanDevice* DevicePointer::operator->() const { return Load().get(); }

DevicePointer::operator bool() const { return Load() != nullptr; }

bool DevicePointer::operator==(std::nullptr_t) const { return Load() == nullptr; }

QueueLock::QueueLock(const DevicePointer& device, std::uint32_t queue) : device(device), site(__builtin_return_address(0)), queue(queue) {
    lock();
}

QueueLock::QueueLock(const DevicePointer& device, std::uint32_t queue, std::defer_lock_t) : device(device), site(__builtin_return_address(0)), queue(queue) {}

QueueLock::QueueLock(const DevicePointer& device, std::uint32_t queue, std::try_to_lock_t) : device(device), site(__builtin_return_address(0)), queue(queue) {
    try_lock();
}

QueueLock::~QueueLock() {
    if (owned) unlock();
}

bool QueueLock::attach(bool tryOnly) {
    held = device.Load();
    recorders = held != nullptr ? held->RecordersFor(queue) : std::array<Graphics::Recorder*, 2>{};
    locked = 0;
    for (auto* recorder : recorders) {
        if (recorder == nullptr) break;
        if (tryOnly) {
            if (!recorder->Lock().try_lock()) {
                while (locked != 0) recorders[--locked]->Lock().unlock();
                held.reset();
                return false;
            }
        } else {
            recorder->Lock().LockFrom(site);
        }
        ++locked;
    }
    return true;
}

void QueueLock::lock() {
    SetLockSite(site);
    GuestMemory::GpuMutex().lock();
    SetLockSite(nullptr);
    try {
        attach(false);
    } catch (...) {
        while (locked != 0) recorders[--locked]->Lock().unlock();
        held.reset();
        GuestMemory::GpuMutex().unlock();
        throw;
    }
    owned = true;
}

bool QueueLock::try_lock() {
    SetLockSite(site);
    const bool locked = GuestMemory::GpuMutex().try_lock();
    SetLockSite(nullptr);
    if (!locked) return false;
    if (!attach(true)) {
        GuestMemory::GpuMutex().unlock();
        return false;
    }
    owned = true;
    return true;
}

void QueueLock::unlock() {
    while (locked != 0) recorders[--locked]->Lock().unlock();
    held.reset();
    owned = false;
    GuestMemory::GpuMutex().unlock();
}

void DeviceUseGate::lock_shared() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return !replacing; });
    ++users;
}

void DeviceUseGate::unlock_shared() {
    std::lock_guard lock(mutex);
    if (--users == 0) changed.notify_all();
}

void DeviceUseGate::lock() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return !replacing; });
    replacing = true;
    changed.wait(lock, [&] { return users == 0; });
}

void DeviceUseGate::unlock() {
    std::lock_guard lock(mutex);
    replacing = false;
    changed.notify_all();
}

}

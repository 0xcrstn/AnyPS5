#include "prx/libSceAgcDriver/Execution/include/OrderedMutex.hpp"
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using AgcDriver::LockLevel;
using AgcDriver::OrderedMutex;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

template<typename TAction>
std::string Failure(TAction action) {
    try {
        action();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

}

int main() {
    OrderedMutex queue(LockLevel::Queue, 0, "queue");
    OrderedMutex graphics(LockLevel::Recorder, 0, "graphics recorder");
    OrderedMutex compute(LockLevel::Recorder, 1, "compute recorder");
    OrderedMutex owner(LockLevel::Owner, 0, "owner");
    AgcDriver::SetLockOrderVerifyForTests(true);
    {
        std::lock_guard a(queue);
        std::lock_guard b(graphics);
        std::lock_guard c(compute);
        std::lock_guard d(owner);
        std::lock_guard again(graphics);
        Expect(graphics.DepthOnThisThread() == 2 && owner.HeldByThisThread(), "nested and recursive acquisitions in order were not tracked");
    }
    Expect(!graphics.HeldByThisThread() && AgcDriver::HeldOrderedLocks() == "nothing", "released locks are still listed as held");
    {
        std::lock_guard d(owner);
        const auto inverted = Failure([&] { std::lock_guard b(graphics); });
        Expect(inverted.find("APS5_LOCK_ORDER_VERIFY: graphics recorder (recorder 0) taken while holding owner (owner 0)") != std::string::npos, "a recorder taken under an owner lock was not refused");
        Expect(graphics.try_lock(), "a try-lock out of order was refused");
        graphics.unlock();
    }
    {
        std::lock_guard c(compute);
        Expect(!Failure([&] { std::lock_guard b(graphics); }).empty(), "two recorders taken out of index order were not refused");
    }
    Expect(!Failure([&] { owner.AssertHeld("an owner operation"); }).empty(), "an assertion without its lock passed");
    {
        std::lock_guard d(owner);
        Expect(Failure([&] { owner.AssertHeld("an owner operation"); }).empty(), "an assertion with its lock failed");
    }
    std::thread other([&] {
        std::lock_guard b(graphics);
        Expect(!owner.HeldByThisThread(), "another thread's hold was seen");
    });
    {
        std::lock_guard d(owner);
        other.join();
    }
    AgcDriver::SetLockOrderVerifyForTests(false);
    {
        std::lock_guard d(owner);
        Expect(Failure([&] { std::lock_guard b(graphics); }).empty(), "the order was checked with the verify mode off");
    }
    if (failures != 0) return 1;
    std::puts("lock order tests passed");
    return 0;
}

#include "prx/libSceAgcDriver/Execution/include/DriverHeapTrace.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace AgcDriver::DriverHeapTrace;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef _WIN32
ModuleUse self() {
    for (const auto& use : Read()) {
        if (use.name == "agc_driver_heap_trace_tests.exe") return use;
    }
    throw std::runtime_error("the test executable was not traced");
}

void checkTrace() {
    require(Enabled(), "APS5_TRACE_DRIVER_HEAP is not set for the test");
    require(HookModules("agc_driver_heap_trace_tests") == 1, "the test executable was not hooked");
    require(HookModules("agc_driver_heap_trace_tests") == 0, "a module was hooked twice");
    const auto before = self();
    void* block = HeapAlloc(GetProcessHeap(), 0, 100000);
    require(block != nullptr, "HeapAlloc failed");
    auto during = self();
    require(during.heapBytes == before.heapBytes + 100000 && during.heapBlocks == before.heapBlocks + 1, "a traced module's heap block was not counted");
    block = HeapReAlloc(GetProcessHeap(), 0, block, 200000);
    require(block != nullptr, "HeapReAlloc failed");
    during = self();
    require(during.heapBytes == before.heapBytes + 200000 && during.heapBlocks == before.heapBlocks + 1, "a traced module's reallocated block was not counted once at its new size");
    HeapFree(GetProcessHeap(), 0, block);
    require(self().heapBytes == before.heapBytes && self().heapBlocks == before.heapBlocks, "a traced module's freed block is still counted");
    auto* reserved = static_cast<std::byte*>(VirtualAlloc(nullptr, 1u << 24u, MEM_RESERVE, PAGE_NOACCESS));
    require(reserved != nullptr, "VirtualAlloc reserve failed");
    require(VirtualAlloc(reserved, 1u << 20u, MEM_COMMIT, PAGE_READWRITE) != nullptr && VirtualAlloc(reserved + (4u << 20u), 2u << 20u, MEM_COMMIT, PAGE_READWRITE) != nullptr, "VirtualAlloc commit failed");
    require(self().virtualBytes == before.virtualBytes + (3u << 20u), "a traced module's commits inside its reservation were not counted");
    VirtualFree(reserved, 1u << 20u, MEM_DECOMMIT);
    require(self().virtualBytes == before.virtualBytes + (2u << 20u), "a traced module's decommit was not counted");
    VirtualFree(reserved, 0, MEM_RELEASE);
    require(self().virtualBytes == before.virtualBytes, "a traced module's released reservation is still counted");
}
#endif

}

int main() {
    try {
#ifdef _WIN32
        checkTrace();
#endif
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_driver_heap_trace_tests: %s\n", error.what());
        return 1;
    }
    std::puts("agc_driver_heap_trace_tests passed");
    return 0;
}

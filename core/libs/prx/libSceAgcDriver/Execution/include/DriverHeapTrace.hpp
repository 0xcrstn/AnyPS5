#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVERHEAPTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVERHEAPTRACE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver::DriverHeapTrace {

struct ModuleUse {
    std::string name;
    std::uint64_t heapBytes = 0;
    std::uint64_t heapBlocks = 0;
    std::uint64_t virtualBytes = 0;
};

bool Enabled();
std::size_t HookModules(std::string_view pathPart);
std::vector<ModuleUse> Read();
std::string Owner(std::uintptr_t address);

}

#endif

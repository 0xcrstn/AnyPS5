#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEMORYREPORT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MEMORYREPORT_HPP

#include <cstdint>
#include <functional>
#include <string>

namespace AgcDriver::MemoryReport {

std::string Describe(const std::function<std::string()>& device);
std::uint64_t IntervalSeconds();
void Start(std::function<std::string()> device);
void Stop();

}

#endif

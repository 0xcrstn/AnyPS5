#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTTHREAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HOSTTHREAD_HPP

#include <cstddef>

namespace AgcDriver {

using HostThreadEntry = void* (*)(void*);

bool StartDetachedHostThread(HostThreadEntry entry, void* argument, std::size_t stackReserveBytes);

}

#endif

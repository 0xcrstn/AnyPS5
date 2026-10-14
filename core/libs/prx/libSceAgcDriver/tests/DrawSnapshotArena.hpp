#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_DRAWSNAPSHOTARENA_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_DRAWSNAPSHOTARENA_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"

bool RunDrawSnapshotArenaTests(const AgcDriver::Graphics::Context& context);
bool RunDrawSnapshotArenaBenchmark(const AgcDriver::Graphics::Context& context);
bool RunEpochDirtySetBenchmark(const AgcDriver::Graphics::Context& context);

#endif

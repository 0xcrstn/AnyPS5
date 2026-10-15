#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DEFAULTSWITCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DEFAULTSWITCH_HPP

#include <cstdlib>
#include <cstring>

namespace AgcDriver {

inline bool DefaultOnSwitch(const char* name) {
    const char* text = std::getenv(name);
    return text == nullptr || std::strcmp(text, "0") != 0;
}

inline bool SwitchExplicitlySet(const char* name) {
    return std::getenv(name) != nullptr;
}

}

#endif

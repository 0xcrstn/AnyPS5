#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTEPIPELINEAHEADPACKETS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_COMPUTEPIPELINEAHEADPACKETS_HPP

#include <cstdint>

namespace AgcDriver {

constexpr bool ComputePipelineAheadPacket(std::uint32_t header) {
    if (header >> 30u != 3u || (header & 1u) != 0) return false;
    switch ((header >> 8u) & 0xffu) {
        case 0x10: case 0x15: case 0x69: case 0x76: case 0x79: case 0x7a: return true;
        default: return false;
    }
}

}

#endif

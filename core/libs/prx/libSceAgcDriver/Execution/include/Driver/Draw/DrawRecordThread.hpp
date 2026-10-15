#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWRECORDTHREAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWRECORDTHREAD_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DrawRecordEffects {
    std::uint64_t writeGenerationBefore = 0;
    std::uint64_t writeGenerationAfter = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> stores;
};

struct PreparedRead {
    std::uint64_t address;
    std::size_t bytes;
};

enum class DrawPrepareStale : std::uint8_t { No, PendingWrite, Stored };

inline DrawPrepareStale StaleDrawPrepare(const DrawRecordEffects& effects, std::span<const PreparedRead> reads, const std::function<bool(std::uint64_t, std::size_t)>& pendingWrite) {
    const bool noted = effects.writeGenerationAfter != effects.writeGenerationBefore;
    if (!noted && effects.stores.empty()) return DrawPrepareStale::No;
    for (const auto& read : reads) {
        if (read.bytes == 0) continue;
        const auto end = read.address + read.bytes;
        for (const auto& [begin, stop] : effects.stores) {
            if (begin < end && read.address < stop) return DrawPrepareStale::Stored;
        }
        if (noted && pendingWrite(read.address, read.bytes)) return DrawPrepareStale::PendingWrite;
    }
    return DrawPrepareStale::No;
}

inline bool OverlapsDrawRecords(std::uint32_t opcode, bool predicated, bool dispatches = false) {
    if (predicated) return false;
    if (dispatches && (opcode == 0x15 || opcode == 0x16)) return true;
    switch (opcode) {
    case 0x13: case 0x26: case 0x28: case 0x2a: case 0x2f:
    case 0x69: case 0x76: case 0x79: case 0x7a:
    case 0x27: case 0x2d: case 0x35: case 0x24: case 0x25: case 0x2c: case 0x38:
        return true;
    default:
        return false;
    }
}

}

#endif

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PASSSIGNATURES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PASSSIGNATURES_HPP

#include <cstdint>
#include <initializer_list>
#include <span>

namespace AgcDriver::Graphics {

class PassSignatures {
public:
    struct Counts {
        std::uint64_t passes = 0;
        std::uint64_t repeated = 0;
        std::uint64_t draws = 0;
        std::uint64_t repeatedDraws = 0;
        std::uint64_t frames = 0;
    };

    static bool Enabled();
    static void SetEnabled(bool enabled);
    static bool Active();
    static void BeginDraw(std::uint64_t passSerial, std::uint64_t passIdentity);
    static void Note(std::uint32_t command, std::initializer_list<std::uint64_t> arguments, std::span<const std::uint64_t> tail);
    static void EndDraw();
    static void NoteFrame();
    static Counts Totals();
    static void Report();
};

}

#endif

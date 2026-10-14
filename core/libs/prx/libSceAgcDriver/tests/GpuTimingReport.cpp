#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

void Keys() {
    constexpr std::uint64_t address = 0x116180b000ull;
    constexpr auto pixel = Recorder::DrawProgramKey(address, true);
    constexpr auto vertex = Recorder::DrawProgramKey(address, false);
    static_assert((pixel & Recorder::DrawProgramFlag) != 0 && (pixel & Recorder::VertexOnlyFlag) == 0);
    static_assert((vertex & Recorder::DrawProgramFlag) != 0 && (vertex & Recorder::VertexOnlyFlag) != 0);
    Expect(pixel != vertex && pixel != address && (pixel & ~Recorder::DrawProgramFlag) == address, "a draw program key does not keep its address apart from a dispatch key");
    Expect(Recorder::DrawProgramKey(address, true) >= Recorder::ClassKey(Recorder::CommandClass::Count), "a draw program key falls among the class keys");
}

void Dispatches() {
    std::vector<Recorder::GpuProgramTotals> programs{{0x1000, 4, 2.0, 0}, {0x2000, 2, 9.0, 753540}, {0x3000, 0, 0.0, 0}};
    const auto text = Recorder::FormatGpuPrograms(programs, false, 2);
    Expect(text == " 0x2000 x2 9ms 4500us 753540w 0x1000 x4 2ms 500us", "dispatch programs are not ordered by GPU time with their run time and words: '" + text + "'");
    Expect(Recorder::FormatGpuPrograms({}, false, 12).empty(), "no programs printed an entry");
}

void Draws() {
    std::vector<Recorder::GpuProgramTotals> programs{{Recorder::DrawProgramKey(0x4000, true), 10, 1.0, 0}, {Recorder::DrawProgramKey(0x5000, false), 1, 3.0, 0}};
    const auto text = Recorder::FormatGpuPrograms(programs, true, 16);
    Expect(text == " vs 0x5000 x1 3ms 3000us ps 0x4000 x10 1ms 100us", "draw programs are not printed by stage and address: '" + text + "'");
}

void Placements() {
    Recorder::GpuProgramTotals program{0x6000, 2, 4.0, 3815};
    program.placements = 2;
    program.inPlaceBytes = 2.0 * 1048576.0 * 3.0;
    program.hostCopyBytes = 1048576.0;
    program.deviceBytes = 0.0;
    auto text = Recorder::FormatGpuPrograms({program}, false, 12);
    Expect(text == " 0x6000 x2 4ms 2000us 3815w [in place 3.00 host copy 0.50 device 0.00 MiB]", "the bound bytes of a program are not printed per placement note: '" + text + "'");
    program.addressBased = 1;
    text = Recorder::FormatGpuPrograms({program}, false, 12);
    Expect(text == " 0x6000 x2 4ms 2000us 3815w [in place 3.00 host copy 0.50 device 0.00 MiB address-based]", "an address-based build is not marked: '" + text + "'");
}

}

int main() {
    Keys();
    Dispatches();
    Draws();
    Placements();
    if (failures != 0) return 1;
    std::printf("gpu timing report tests passed\n");
    return 0;
}

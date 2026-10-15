#include "prx/libSceAgcDriver/Graphics/include/LinearSpace.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AgcDriver::Graphics;
namespace Abi = ShaderRecompiler::BdaAbi;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

constexpr VkDeviceAddress Base = 0x7000000000ull;
constexpr std::uint64_t Page = LinearPageBytes;

Abi::Range linear(std::uint64_t begin, std::uint64_t end, std::uint32_t permissions) {
    return {begin, end, Base + begin, permissions, 0};
}

void pageCodeTests() {
    constexpr std::uint64_t at = 0x200000000ull;
    std::vector<Abi::Range> ranges{
        linear(at + 0x100, at + 4 * Page, Abi::Read),
        linear(at + 4 * Page, at + 6 * Page, Abi::Read | Abi::Write),
        {at + 8 * Page, at + 10 * Page, 0x9000000000ull, Abi::Read, 0},
        linear(at + 12 * Page, at + 16 * Page, Abi::Read),
        linear(at + 13 * Page + 0x40, at + 14 * Page, Abi::Read),
    };
    const auto codes = BuildLinearPageCodes(ranges, Base);
    Expect(codes.guestLow <= at + Page && codes.guestHigh >= at + 16 * Page, "the window covers every linear page");
    Expect(LinearPageCodeAt(codes, at + 0x200) == LinearPageCode::Fallback, "a page the range starts inside falls back");
    Expect(LinearPageCodeAt(codes, at + Page) == LinearPageCode::Read && LinearPageCodeAt(codes, at + 4 * Page - 1) == LinearPageCode::Read, "whole pages of a read-only linear range read linearly");
    Expect(LinearPageCodeAt(codes, at + 4 * Page) == LinearPageCode::ReadWrite && LinearPageCodeAt(codes, at + 6 * Page - 1) == LinearPageCode::ReadWrite, "a writable linear range is read-write");
    Expect(LinearPageCodeAt(codes, at + 6 * Page) == LinearPageCode::Fallback, "an unmapped page falls back");
    Expect(LinearPageCodeAt(codes, at + 9 * Page) == LinearPageCode::Fallback, "a range backed elsewhere falls back");
    Expect(LinearPageCodeAt(codes, at + 12 * Page) == LinearPageCode::Read, "a page before an overlapping range stays linear");
    Expect(LinearPageCodeAt(codes, at + 13 * Page) == LinearPageCode::Fallback, "a page a later range begins inside falls back");
    Expect(LinearPageCodeAt(codes, at + 14 * Page) == LinearPageCode::Fallback, "a page past the selected range's end falls back");
    Expect(LinearPageCodeAt(codes, at + 15 * Page) == LinearPageCode::Fallback, "the overlapped range is not selected after the later one ends");
    Expect(LinearPageCodeAt(codes, codes.guestHigh) == LinearPageCode::Fallback && LinearPageCodeAt(codes, 0) == LinearPageCode::Fallback, "addresses outside the window fall back");
    const std::vector<Abi::Range> twins{linear(at, at + 2 * Page, Abi::Read), linear(at, at + 2 * Page, Abi::Read | Abi::Write)};
    Expect(BuildLinearPageCodes(twins, Base).words.empty(), "ranges with the same begin are never linear");
    Expect(BuildLinearPageCodes(ranges, 0).words.empty(), "no linear space means no linear pages");
    const std::vector<Abi::Range> writeOnly{linear(at, at + 2 * Page, Abi::Write)};
    Expect(BuildLinearPageCodes(writeOnly, Base).words.empty(), "a write-only range is not linear");
}

void parseTests() {
    Expect(ParseLinearSpace(nullptr) && ParseLinearSpace("1") && !ParseLinearSpace("0"), "APS5_LINEAR_ADDRESS_SPACE values");
    bool threw = false;
    try {
        static_cast<void>(ParseLinearSpace("on"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Expect(threw, "an unknown APS5_LINEAR_ADDRESS_SPACE value throws");
}

}

int main() {
    pageCodeTests();
    parseTests();
    if (failures != 0) return 1;
    std::printf("linear space tests passed\n");
    return 0;
}

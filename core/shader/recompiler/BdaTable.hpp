#ifndef CORE_SHADER_RECOMPILER_BDATABLE_HPP
#define CORE_SHADER_RECOMPILER_BDATABLE_HPP

#include "BdaAbi.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ShaderRecompiler::BdaTable {

enum class Translation : std::uint32_t { Legacy, Radix, Verify, Linear, LinearVerify };
enum class Relative : std::uint32_t { Off, On, Verify };

inline constexpr std::uint32_t RadixVersion = 2;
inline constexpr std::uint32_t LinearVersion = 3;
inline constexpr std::uint32_t LinearWords = 10;
inline constexpr std::uint32_t LinearDeviceBaseWord = 0;
inline constexpr std::uint32_t LinearGuestLowWord = 2;
inline constexpr std::uint32_t LinearGuestHighWord = 4;
inline constexpr std::uint32_t LinearBitmapWord = 6;
inline constexpr std::uint32_t LinearPageShiftWord = 8;
inline constexpr std::uint32_t LinearReservedWord = 9;
inline constexpr std::uint32_t LinearPageBits = 2;
inline constexpr std::uint32_t LinearPagesPerWord = 32u / LinearPageBits;
inline constexpr std::uint32_t LinearFallback = 0;
inline constexpr std::uint32_t LinearRead = 1;
inline constexpr std::uint32_t LinearReadWrite = 2;
inline constexpr std::uint32_t HeaderWords = sizeof(BdaAbi::Header) / sizeof(std::uint32_t);
inline constexpr std::uint32_t WindowBaseWord = 4;
inline constexpr std::uint32_t WindowLastWord = 6;
inline constexpr std::uint32_t RootShiftWord = 8;
inline constexpr std::uint32_t RootCountWord = 9;
inline constexpr std::uint32_t NodeWordsWord = 10;
inline constexpr std::uint32_t RadixReservedWord = 11;
inline constexpr std::uint32_t RadixRangeWord = 12;
inline constexpr std::uint32_t RangeWords = sizeof(BdaAbi::Range) / sizeof(std::uint32_t);
inline constexpr std::uint32_t EntryWords = 2;
inline constexpr std::uint32_t ChildBits = 8;
inline constexpr std::uint32_t ChildEntries = 1u << ChildBits;
inline constexpr std::uint32_t NodeWords = ChildEntries * EntryWords;
inline constexpr std::uint32_t RootLimit = 16384;
inline constexpr std::uint32_t SearchLimit = 4;
inline constexpr std::uint32_t NodeBit = 0x80000000u;

inline Translation ParseTranslation(const char* value) {
    if (value == nullptr) return Translation::Radix;
    const std::string_view text(value);
    if (text == "legacy") return Translation::Legacy;
    if (text == "radix") return Translation::Radix;
    if (text == "verify") return Translation::Verify;
    if (text == "linear") return Translation::Linear;
    if (text == "linear_verify") return Translation::LinearVerify;
    throw std::runtime_error("APS5_BDA_ABI must be legacy, radix, verify, linear or linear_verify, not \"" + std::string(text) + "\"");
}

inline Translation TranslationSetting() {
    static const Translation translation = ParseTranslation(std::getenv("APS5_BDA_ABI"));
    return translation;
}

inline Relative ParseRelative(const char* value) {
    if (value == nullptr) return Relative::On;
    const std::string_view text(value);
    if (text == "off") return Relative::Off;
    if (text == "on") return Relative::On;
    if (text == "verify") return Relative::Verify;
    throw std::runtime_error("APS5_BDA_RELATIVE must be off, on or verify, not \"" + std::string(text) + "\"");
}

inline bool RangeCacheVerified() {
    static const bool verified = std::getenv("APS5_BDA_RANGE_CACHE") != nullptr && std::getenv("APS5_VERIFY_BDA_RANGE_CACHE") != nullptr;
    return verified;
}

inline Relative RelativeSetting() {
    static const Relative relative = [] {
        const char* explicitValue = std::getenv("APS5_BDA_RELATIVE");
        const auto parsed = ParseRelative(explicitValue);
        if (std::getenv("APS5_BDA_RANGE_CACHE") == nullptr) return parsed;
        const auto implied = RangeCacheVerified() ? Relative::Verify : Relative::On;
        if (explicitValue != nullptr && parsed != implied) throw std::runtime_error("APS5_BDA_RANGE_CACHE selects APS5_BDA_RELATIVE=" + std::string(implied == Relative::Verify ? "verify" : "on") + ", which conflicts with APS5_BDA_RELATIVE=" + explicitValue);
        return implied;
    }();
    return relative;
}

inline BdaAbi::FaultReason RelativeMismatchReason() {
    return RangeCacheVerified() ? BdaAbi::FaultReason::RangeCacheMismatch : BdaAbi::FaultReason::RelativeMismatch;
}

inline bool InlineFastPathSetting() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_BDA_INLINE_FAST_PATH");
        if (value == nullptr || std::string_view(value) == "on") return true;
        if (std::string_view(value) == "off") return false;
        throw std::runtime_error("APS5_BDA_INLINE_FAST_PATH must be off or on, not \"" + std::string(value) + "\"");
    }();
    return enabled;
}

inline bool LinearLayout(Translation translation) {
    return translation == Translation::Linear || translation == Translation::LinearVerify;
}

inline std::uint32_t TableVersion(Translation translation) {
    return translation == Translation::Legacy ? BdaAbi::Version : LinearLayout(translation) ? LinearVersion : RadixVersion;
}

struct LinearSpace {
    std::uint64_t deviceBase = 0;
    std::uint64_t guestLow = 0;
    std::uint64_t guestHigh = 0;
    std::uint64_t bitmapAddress = 0;
    std::uint32_t pageShift = 16;
};

inline std::uint32_t RangeBaseWord(Translation translation) {
    return translation == Translation::Legacy ? HeaderWords : RadixRangeWord;
}

inline std::uint32_t Word(std::span<const std::uint32_t> words, std::uint64_t index) {
    if (index >= words.size()) throw std::runtime_error("BDA table read past its end at word " + std::to_string(index));
    return words[static_cast<std::size_t>(index)];
}

inline std::uint64_t Address(std::span<const std::uint32_t> words, std::uint64_t index) {
    return Word(words, index) | (static_cast<std::uint64_t>(Word(words, index + 1)) << 32u);
}

inline BdaAbi::Range RangeAt(std::span<const std::uint32_t> words, std::uint32_t rangeBase, std::uint32_t index) {
    BdaAbi::Range range{};
    const auto first = static_cast<std::uint64_t>(rangeBase) + static_cast<std::uint64_t>(index) * RangeWords;
    if (first + RangeWords > words.size()) throw std::runtime_error("BDA table range " + std::to_string(index) + " lies past the table");
    std::memcpy(&range, words.data() + first, sizeof(range));
    return range;
}

inline std::uint32_t SelectedByBegins(std::span<const BdaAbi::Range> ranges, std::uint64_t address) {
    const auto found = std::upper_bound(ranges.begin(), ranges.end(), address, [](std::uint64_t value, const BdaAbi::Range& range) { return value < range.begin; });
    return static_cast<std::uint32_t>(found - ranges.begin());
}

struct Builder {
    std::span<const BdaAbi::Range> ranges;
    std::uint64_t base = 0;
    std::vector<std::uint32_t>& words;

    std::uint32_t selectedAt(std::uint64_t offset) const { return SelectedByBegins(ranges, base + offset); }

    void fill(std::size_t entry, std::uint64_t first, std::uint64_t last, std::uint32_t shift) {
        const auto selected = selectedAt(first);
        const auto begins = selectedAt(last) - selected;
        if (begins <= SearchLimit) {
            words[entry] = selected;
            words[entry + 1] = begins;
            return;
        }
        if (shift < ChildBits) throw std::logic_error("BDA radix region of one byte holds several range begins");
        if (words.size() + NodeWords > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("BDA radix table exceeds 32-bit word offsets");
        const auto node = static_cast<std::uint32_t>(words.size());
        words.resize(words.size() + NodeWords);
        words[entry] = node;
        words[entry + 1] = NodeBit;
        const auto childShift = shift - ChildBits;
        const auto childSpan = std::uint64_t{1} << childShift;
        for (std::uint32_t child = 0; child < ChildEntries; ++child) {
            const auto childFirst = first + static_cast<std::uint64_t>(child) * childSpan;
            if (childFirst > last) {
                words[node + child * EntryWords] = selectedAt(last);
                words[node + child * EntryWords + 1] = 0;
                continue;
            }
            fill(node + child * EntryWords, childFirst, std::min(last, childFirst + (childSpan - 1)), childShift);
        }
    }
};

inline std::vector<std::uint32_t> BuildRadix(std::span<const BdaAbi::Range> ranges, Translation translation);

inline std::vector<std::uint32_t> Build(std::span<const BdaAbi::Range> ranges, Translation translation, const LinearSpace* space = nullptr) {
    if (!LinearLayout(translation)) {
        if (space != nullptr) throw std::runtime_error("a linear space needs a linear BDA table");
        return BuildRadix(ranges, translation);
    }
    auto words = BuildRadix(ranges, Translation::Radix);
    const LinearSpace linear = space != nullptr ? *space : LinearSpace{};
    if (linear.guestHigh < linear.guestLow || (linear.guestHigh != linear.guestLow && (linear.deviceBase == 0 || linear.bitmapAddress == 0)) || linear.pageShift < 12u || linear.pageShift > 32u) throw std::runtime_error("invalid BDA linear space");
    words[0] = LinearVersion;
    words[RadixReservedWord] = static_cast<std::uint32_t>(words.size());
    for (const auto value : {linear.deviceBase, linear.guestLow, linear.guestHigh, linear.bitmapAddress}) {
        words.push_back(static_cast<std::uint32_t>(value));
        words.push_back(static_cast<std::uint32_t>(value >> 32u));
    }
    words.push_back(linear.pageShift);
    words.push_back(0u);
    return words;
}

inline std::vector<std::uint32_t> BuildRadix(std::span<const BdaAbi::Range> ranges, Translation translation) {
    if (ranges.size() > std::numeric_limits<std::uint32_t>::max() / RangeWords - RadixRangeWord) throw std::runtime_error("BDA table range count overflow");
    const auto count = static_cast<std::uint32_t>(ranges.size());
    const auto rangeBase = RangeBaseWord(translation);
    std::vector<std::uint32_t> words(rangeBase + static_cast<std::size_t>(count) * RangeWords, 0u);
    if (count != 0) std::memcpy(words.data() + rangeBase, ranges.data(), ranges.size_bytes());
    if (translation == Translation::Legacy) {
        const BdaAbi::Header header{BdaAbi::Version, count, sizeof(BdaAbi::Range), 0};
        std::memcpy(words.data(), &header, sizeof(header));
        return words;
    }
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index].begin < ranges[index - 1].begin) throw std::runtime_error("BDA radix table needs ranges sorted by begin");
    }
    const auto rootWord = static_cast<std::uint32_t>(words.size());
    const BdaAbi::Header header{RadixVersion, count, sizeof(BdaAbi::Range), rootWord};
    std::memcpy(words.data(), &header, sizeof(header));
    const std::uint64_t windowBase = count == 0 ? 0 : ranges.front().begin;
    const std::uint64_t windowLast = count == 0 ? 0 : ranges.back().begin - windowBase;
    std::uint32_t rootShift = 0;
    while ((windowLast >> rootShift) >= RootLimit) rootShift += ChildBits;
    const auto rootCount = count == 0 ? 0u : static_cast<std::uint32_t>(windowLast >> rootShift) + 1u;
    words[WindowBaseWord] = static_cast<std::uint32_t>(windowBase);
    words[WindowBaseWord + 1] = static_cast<std::uint32_t>(windowBase >> 32u);
    words[WindowLastWord] = static_cast<std::uint32_t>(windowLast);
    words[WindowLastWord + 1] = static_cast<std::uint32_t>(windowLast >> 32u);
    words[RootShiftWord] = rootShift;
    words[RootCountWord] = rootCount;
    words.resize(words.size() + static_cast<std::size_t>(rootCount) * EntryWords, 0u);
    Builder builder{ranges, windowBase, words};
    const auto rootSpan = std::uint64_t{1} << rootShift;
    for (std::uint32_t root = 0; root < rootCount; ++root) {
        const auto first = static_cast<std::uint64_t>(root) * rootSpan;
        const auto last = std::min(windowLast, first + (rootSpan - 1));
        builder.fill(rootWord + root * EntryWords, first, last, rootShift);
    }
    words[NodeWordsWord] = static_cast<std::uint32_t>(words.size() - rootWord - static_cast<std::size_t>(rootCount) * EntryWords);
    return words;
}

inline std::uint32_t RadixSelected(std::span<const std::uint32_t> words, std::uint64_t address) {
    const auto count = Word(words, 1);
    const auto rangeBase = RadixRangeWord;
    const auto base = Address(words, WindowBaseWord);
    const auto last = Address(words, WindowLastWord);
    if (count == 0 || address < base) return 0;
    const auto offset = address - base;
    if (offset > last) return count;
    auto shift = Word(words, RootShiftWord);
    auto entry = static_cast<std::uint64_t>(Word(words, 3)) + (offset >> shift) * EntryWords;
    while ((Word(words, entry + 1) & NodeBit) != 0u) {
        shift -= ChildBits;
        entry = static_cast<std::uint64_t>(Word(words, entry)) + ((offset >> shift) & (ChildEntries - 1u)) * EntryWords;
    }
    const auto selected = Word(words, entry);
    const auto begins = Word(words, entry + 1);
    std::uint32_t found = 0;
    for (std::uint32_t index = 0; index < SearchLimit; ++index) {
        if (index >= begins) continue;
        if (RangeAt(words, rangeBase, selected + index).begin <= address) ++found;
    }
    return selected + found;
}

inline void Validate(std::span<const std::uint32_t> words, Translation translation) {
    const auto fail = [](const std::string& message) { throw std::runtime_error("invalid BDA table: " + message); };
    if (words.size() < HeaderWords) fail("shorter than its header");
    if (LinearLayout(translation)) {
        if (words.size() < RadixRangeWord + LinearWords || words[0] != LinearVersion) fail("wrong linear header");
        const auto linearWord = static_cast<std::size_t>(words[RadixReservedWord]);
        if (linearWord + LinearWords != words.size()) fail("the linear block does not end the table");
        const auto at = [&](std::uint32_t word) { return static_cast<std::uint64_t>(words[linearWord + word]) | (static_cast<std::uint64_t>(words[linearWord + word + 1]) << 32u); };
        const auto low = at(LinearGuestLowWord);
        const auto high = at(LinearGuestHighWord);
        const auto shift = words[linearWord + LinearPageShiftWord];
        if (high < low || (high != low && (at(LinearDeviceBaseWord) == 0 || at(LinearBitmapWord) == 0)) || shift < 12u || shift > 32u || words[linearWord + LinearReservedWord] != 0) fail("invalid linear space");
        std::vector<std::uint32_t> radix(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(linearWord));
        radix[0] = RadixVersion;
        radix[RadixReservedWord] = 0;
        Validate(radix, Translation::Radix);
        return;
    }
    const auto count = words[1];
    const auto rangeBase = RangeBaseWord(translation);
    if (words[2] != sizeof(BdaAbi::Range)) fail("wrong range size");
    if (translation == Translation::Legacy) {
        if (words[0] != BdaAbi::Version || words[3] != 0) fail("wrong legacy header");
        if (words.size() != static_cast<std::size_t>(rangeBase) + static_cast<std::size_t>(count) * RangeWords) fail("legacy size does not match its range count");
        return;
    }
    if (words.size() < RadixRangeWord) fail("shorter than its radix header");
    if (words[0] != RadixVersion || words[RadixReservedWord] != 0) fail("wrong radix header");
    const auto rootWord = static_cast<std::uint64_t>(rangeBase) + static_cast<std::uint64_t>(count) * RangeWords;
    if (words[3] != rootWord) fail("root does not follow the ranges");
    std::vector<BdaAbi::Range> ranges(count);
    for (std::uint32_t index = 0; index < count; ++index) ranges[index] = RangeAt(words, rangeBase, index);
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index].begin < ranges[index - 1].begin) fail("ranges not sorted by begin");
    }
    const auto base = Address(words, WindowBaseWord);
    const auto last = Address(words, WindowLastWord);
    const auto rootShift = words[RootShiftWord];
    const auto rootCount = words[RootCountWord];
    if (count == 0) {
        if (base != 0 || last != 0 || rootShift != 0 || rootCount != 0 || words[NodeWordsWord] != 0 || words.size() != rootWord) fail("an empty table has a radix");
        return;
    }
    if (base != ranges.front().begin || last != ranges.back().begin - base) fail("window does not span the range begins");
    if (rootShift % ChildBits != 0 || rootShift >= 64u) fail("root shift is not a whole level");
    if (rootCount != (last >> rootShift) + 1u || rootCount > RootLimit) fail("root count does not cover the window");
    const auto nodesFirst = rootWord + static_cast<std::uint64_t>(rootCount) * EntryWords;
    if (nodesFirst + words[NodeWordsWord] != words.size() || words[NodeWordsWord] % NodeWords != 0) fail("node area size");
    const auto selectedAt = [&](std::uint64_t offset) { return SelectedByBegins(ranges, base + offset); };
    std::uint64_t visitedNodes = 0;
    const auto check = [&](auto&& self, std::uint64_t entry, std::uint64_t parent, std::uint64_t first, std::uint64_t lastOffset, std::uint32_t shift) -> void {
        const auto value = words[static_cast<std::size_t>(entry)];
        const auto info = words[static_cast<std::size_t>(entry + 1)];
        if ((info & NodeBit) != 0u) {
            if (info != NodeBit) fail("node entry with extra bits");
            if (shift < ChildBits) fail("node below byte level");
            if (value < nodesFirst || value <= parent || (value - nodesFirst) % NodeWords != 0 || static_cast<std::uint64_t>(value) + NodeWords > words.size()) fail("node offset out of place");
            ++visitedNodes;
            const auto childShift = shift - ChildBits;
            const auto childSpan = std::uint64_t{1} << childShift;
            for (std::uint32_t child = 0; child < ChildEntries; ++child) {
                const auto childFirst = first + static_cast<std::uint64_t>(child) * childSpan;
                if (childFirst > lastOffset) {
                    if (words[value + child * EntryWords + 1] != 0 || words[value + child * EntryWords] != selectedAt(lastOffset)) fail("entry past the window");
                    continue;
                }
                self(self, value + child * EntryWords, value, childFirst, std::min(lastOffset, childFirst + (childSpan - 1)), childShift);
            }
            return;
        }
        if (info > SearchLimit) fail("leaf searches too many begins");
        if (value != selectedAt(first) || info != selectedAt(lastOffset) - value) fail("leaf does not select the begins of its region");
    };
    const auto rootSpan = std::uint64_t{1} << rootShift;
    for (std::uint32_t root = 0; root < rootCount; ++root) {
        const auto first = static_cast<std::uint64_t>(root) * rootSpan;
        const auto lastOffset = std::min(last, first + (rootSpan - 1));
        check(check, rootWord + root * EntryWords, rootWord, first, lastOffset, rootShift);
    }
    if (visitedNodes * NodeWords != words[NodeWordsWord]) fail("unreferenced or shared nodes");
}

}

#endif

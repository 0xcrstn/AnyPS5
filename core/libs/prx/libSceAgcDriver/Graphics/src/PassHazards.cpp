#include "prx/libSceAgcDriver/Graphics/include/PassHazards.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {

bool ResolvePassRanges(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    bool known = true;
    for (std::size_t index = 0; index < ranges.size();) {
        const auto [begin, end] = ranges[index];
        if (begin == end) {
            ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        std::array<std::uintptr_t, 16> begins{}, ends{};
        std::array<std::uint64_t, 16> canonicals{};
        std::size_t count = 0;
        bool translated = end > begin && GuestArena::GuestArenaTryBackingRuns_nid_postfix(begin, static_cast<std::size_t>(end - begin), begins.data(), ends.data(), canonicals.data(), begins.size(), &count) && count != 0 && count <= begins.size();
        auto expected = begin;
        if (translated) {
            for (std::size_t run = 0; run < count; ++run) {
                if (begins[run] != expected || ends[run] <= begins[run] || ends[run] > end || ends[run] - begins[run] > std::numeric_limits<std::uint64_t>::max() - canonicals[run]) {
                    translated = false;
                    break;
                }
                expected = ends[run];
            }
            translated = translated && expected == end;
        }
        if (!translated) {
            known = false;
            ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ranges[index] = {canonicals[0], canonicals[0] + ends[0] - begins[0]};
        for (std::size_t run = 1; run < count; ++run) {
            ranges.insert(ranges.begin() + static_cast<std::ptrdiff_t>(index + run), {canonicals[run], canonicals[run] + ends[run] - begins[run]});
        }
        index += count;
    }
    return known;
}

ReadOnlyAddressPasses ParseReadOnlyAddressPasses(const char* text) {
    if (text == nullptr || std::strcmp(text, "1") == 0) return ReadOnlyAddressPasses::On;
    if (std::strcmp(text, "0") == 0) return ReadOnlyAddressPasses::Off;
    if (std::strcmp(text, "verify") == 0) return ReadOnlyAddressPasses::Verify;
    throw std::runtime_error(std::string("APS5_READONLY_ADDRESS_PASSES must be 0, 1 or verify, not ") + text);
}

DepthPassMerge ParseDepthPassMerge(const char* text) {
    if (text == nullptr || std::strcmp(text, "1") == 0) return DepthPassMerge::On;
    if (std::strcmp(text, "0") == 0) return DepthPassMerge::Off;
    if (std::strcmp(text, "verify") == 0) return DepthPassMerge::Verify;
    throw std::runtime_error(std::string("APS5_DEPTH_PASS_MERGE must be 0, 1 or verify, not ") + text);
}

DepthPassMerge DepthPassMergeSetting() {
    static const auto setting = ParseDepthPassMerge(std::getenv("APS5_DEPTH_PASS_MERGE"));
    return setting;
}

ReadOnlyAddressPasses ReadOnlyAddressPassesSetting() {
    static const auto setting = ParseReadOnlyAddressPasses(std::getenv("APS5_READONLY_ADDRESS_PASSES"));
    return setting;
}

void GuestRangeSet::Insert(std::uint64_t begin, std::uint64_t end) {
    if (begin >= end) return;
    auto first = std::lower_bound(ranges.begin(), ranges.end(), begin, [](const auto& range, std::uint64_t value) { return range.second < value; });
    auto last = first;
    while (last != ranges.end() && last->first <= end) {
        begin = std::min(begin, last->first);
        end = std::max(end, last->second);
        ++last;
    }
    if (first == last) {
        ranges.insert(first, {begin, end});
        return;
    }
    *first = {begin, end};
    ranges.erase(first + 1, last);
}

bool GuestRangeSet::Overlaps(std::uint64_t begin, std::uint64_t end) const {
    if (begin >= end) return false;
    const auto it = std::upper_bound(ranges.begin(), ranges.end(), begin, [](std::uint64_t value, const auto& range) { return value < range.second; });
    return it != ranges.end() && it->first < end;
}

bool PassHazards::overlapsAny(const GuestRangeSet& set, GuestRangeList ranges) const {
    if (set.Empty()) return false;
    return std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) { return set.Overlaps(range.first, range.second); });
}

PassHazard PassHazards::Check(const PassAccess& access, bool samePass) const {
    return check(access, samePass, access.anyWrites, anyWrites);
}

PassHazard PassHazards::CheckKnownAddressWrites(const PassAccess& access, bool samePass) const {
    return check(access, samePass, access.anyWrites && !access.knownAddressWrites, unknownAddressWrites);
}

PassHazard PassHazards::check(const PassAccess& access, bool samePass, bool accessWrites, bool earlierWrites) const {
    const auto touchesMemory = [](const PassAccess& use) {
        const auto nonEmpty = [](GuestRangeList ranges) { return std::any_of(ranges.begin(), ranges.end(), [](const auto& range) { return range.first < range.second; }); };
        return use.anyReads || use.anyWrites || nonEmpty(use.reads) || nonEmpty(use.writes);
    };
    const bool earlierTouches = anyReads || anyWrites || !reads.Empty() || !writes.Empty();
    if ((accessWrites && earlierTouches) || (earlierWrites && touchesMemory(access))) return PassHazard::UnknownWrite;
    const bool writesRanges = std::any_of(access.writes.begin(), access.writes.end(), [](const auto& range) { return range.first < range.second; });
    if ((access.anyReads && !writes.Empty()) || (anyReads && writesRanges)) return PassHazard::UnknownRead;
    if (overlapsAny(writes, access.reads)) return PassHazard::ReadAfterWrite;
    if (overlapsAny(writes, access.writes)) return PassHazard::WriteAfterWrite;
    if (overlapsAny(reads, access.writes)) return PassHazard::WriteAfterRead;
    const auto conflicts = [](bool earlier, bool later) { return earlier || later; };
    for (const auto& [image, written] : access.images) {
        if (image == VK_NULL_HANDLE) continue;
        if (std::any_of(images.begin(), images.end(), [&](const auto& seen) { return seen.first == image && conflicts(seen.second, written); })) return PassHazard::Image;
        if (std::any_of(attachments.begin(), attachments.end(), [&](const Attachment& seen) { return seen.image == image && conflicts(seen.written, written); })) return PassHazard::Image;
    }
    for (const auto& [image, written] : access.attachments) {
        if (image == VK_NULL_HANDLE) continue;
        if (std::any_of(images.begin(), images.end(), [&](const auto& seen) { return seen.first == image && conflicts(seen.second, written); })) return PassHazard::Image;
        if (std::any_of(attachments.begin(), attachments.end(), [&](const Attachment& seen) { return seen.image == image && !(samePass && seen.pass == pass) && conflicts(seen.written, written); })) return PassHazard::Image;
    }
    return PassHazard::None;
}

void PassHazards::Add(const PassAccess& access) {
    for (const auto& [begin, end] : access.reads) reads.Insert(begin, end);
    for (const auto& [begin, end] : access.writes) writes.Insert(begin, end);
    for (const auto& [image, written] : access.images) {
        if (image == VK_NULL_HANDLE) continue;
        const auto it = std::find_if(images.begin(), images.end(), [&](const auto& seen) { return seen.first == image; });
        if (it == images.end()) images.emplace_back(image, written);
        else it->second = it->second || written;
    }
    for (const auto& [image, written] : access.attachments) {
        if (image == VK_NULL_HANDLE) continue;
        const auto it = std::find_if(attachments.begin(), attachments.end(), [&](const Attachment& seen) { return seen.image == image && seen.pass == pass; });
        if (it == attachments.end()) attachments.push_back({image, written, pass});
        else it->written = it->written || written;
    }
    anyReads = anyReads || access.anyReads;
    anyWrites = anyWrites || access.anyWrites;
    unknownAddressWrites = unknownAddressWrites || (access.anyWrites && !access.knownAddressWrites);
}

void PassHazards::Clear() {
    reads.Clear();
    writes.Clear();
    images.clear();
    attachments.clear();
    anyReads = anyWrites = unknownAddressWrites = false;
}

bool PassHazards::Empty() const {
    return reads.Empty() && writes.Empty() && images.empty() && attachments.empty() && !anyReads && !anyWrites;
}

}

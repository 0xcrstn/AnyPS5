#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_BDATRANSLATIONFIXTURES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_BDATRANSLATIONFIXTURES_HPP

#include "BdaTable.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

struct BdaResolved {
    std::uint64_t result = 0;
    std::uint32_t reason = 0;
    bool operator==(const BdaResolved&) const = default;
};

struct BdaQuery {
    std::uint64_t address = 0;
    std::uint32_t bytes = 0;
};

inline BdaResolved ReferenceBdaResolve(std::span<const ShaderRecompiler::BdaAbi::Range> ranges, std::uint64_t address, std::uint32_t bytes, std::uint32_t permission) {
    using Reason = ShaderRecompiler::BdaAbi::FaultReason;
    const auto code = [](Reason reason) { return static_cast<std::uint32_t>(reason); };
    const auto end = address + bytes;
    if (bytes == 0 || end <= address) return {0, code(Reason::Overflow)};
    const auto index = ShaderRecompiler::BdaTable::SelectedByBegins(ranges, address);
    if (index == 0) return {0, code(Reason::Unmapped)};
    const auto& range = ranges[index - 1];
    const auto result = range.deviceAddress + (address - range.begin);
    const auto deviceEnd = result + bytes;
    if (range.reserved != 0 || range.begin >= range.end) return {0, code(Reason::InvalidTable)};
    if (end > range.end) return {0, code(Reason::Unmapped)};
    if ((range.permissions & permission) != permission) return {0, code(Reason::Permission)};
    if (range.deviceAddress == 0) return {0, code(Reason::InvalidTable)};
    if (result < range.deviceAddress || deviceEnd <= result) return {0, code(Reason::Overflow)};
    return {result, 0};
}

inline constexpr std::uint64_t BdaFixturePage = 0x7fff12345000ULL;
inline constexpr std::array<std::uint32_t, 7> BdaFixtureWidths{0u, 1u, 2u, 4u, 8u, 12u, 16u};

inline std::vector<ShaderRecompiler::BdaAbi::Range> BdaPageFixture() {
    namespace Abi = ShaderRecompiler::BdaAbi;
    constexpr auto page = BdaFixturePage;
    constexpr std::uint32_t rw = Abi::Read | Abi::Write;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::vector<Abi::Range> ranges{
        {page - 0x2000, page + 0x10, 0x100000001ULL, Abi::Read, 0},
        {page + 0x10, page + 0x11, 0x200000003ULL, rw, 0},
        {page + 0x11, page + 0x400, 0x300000000ULL, Abi::Read, 0},
        {page + 0x480, page + 0x1000, 0x400000002ULL, Abi::Write, 0},
        {page + 0x500, page + 0x504, 0x500000000ULL, rw, 0},
        {page + 0x508, page + 0x50c, 0x500001000ULL, Abi::Read, 0},
        {page + 0x510, page + 0x514, 0x500002000ULL, rw, 0},
        {page + 0x518, page + 0x51c, 0x500003000ULL, Abi::Write, 0},
        {page + 0x520, page + 0x524, 0x500004000ULL, rw, 0},
        {page + 0x521, page + 0x523, 0x500005000ULL, Abi::Read, 0},
        {page + 0xfff, page + 0x1800, 0x600000007ULL, rw, 0},
        {page + 0x1800, page + 0x1900, 0x700000000ULL, Abi::Read, 0},
        {page + 0x1800, page + 0x1810, 0x710000000ULL, rw, 0},
        {page + 0x1900, page + 0x1a00, max - 0x7f, rw, 0},
        {page + 0x1a00, page + 0x1a10, 0, rw, 0},
        {page + 0x1a10, page + 0x1a20, 0x800000000ULL, rw, 1},
        {page + 0x1a20, page + 0x1a20, 0x900000000ULL, rw, 0},
        {page + 0x1a30, page + 0x1a40, 0xa00000000ULL, rw, 0},
        {max - 0x100, max - 0x10, 0xb00000000ULL, rw, 0},
        {max - 0x10, max, 0xc00000000ULL, Abi::Read, 0},
    };
    return ranges;
}

inline std::vector<ShaderRecompiler::BdaAbi::Range> BdaSpreadFixture(std::uint32_t count) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    std::vector<Abi::Range> ranges;
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    const auto next = [&] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 17u;
    };
    std::uint64_t cursor = 0x100000000ULL;
    while (ranges.size() < count) {
        const auto kind = next() % 8u;
        if (kind == 0u) {
            const auto cluster = 5u + static_cast<std::uint32_t>(next() % 8u);
            for (std::uint32_t member = 0; member < cluster && ranges.size() < count; ++member) {
                const auto begin = cursor + member * (1u + next() % 9u);
                ranges.push_back({begin, begin + 1u + next() % 64u, 0x1000000000ULL + begin * 3u, static_cast<std::uint32_t>(1u + next() % 3u), 0});
            }
            cursor += 0x200u + next() % 0x10000u;
            continue;
        }
        const auto bytes = 1u + next() % 0x40000u;
        ranges.push_back({cursor, cursor + bytes, 0x2000000000ULL + cursor, static_cast<std::uint32_t>(1u + next() % 3u), 0});
        cursor += kind == 1u ? bytes / 2u + 1u : bytes + next() % 0x100000u;
        if (kind == 2u) cursor += next() % 0x10000000ULL;
    }
    std::stable_sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) { return left.begin < right.begin; });
    return ranges;
}

inline std::vector<std::vector<ShaderRecompiler::BdaAbi::Range>> BdaTableFixtures() {
    namespace Abi = ShaderRecompiler::BdaAbi;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::vector<Abi::Range>> tables;
    tables.push_back({});
    tables.push_back({{0x7fff12340001ULL, 0x7fff12340004ULL, 0x1000ULL, Abi::Read, 0}});
    tables.push_back({{0x7fff12340001ULL, 0x7fff12340004ULL, 0x1000ULL, Abi::Read, 0}, {0x7fff12340004ULL, 0x7fff12340006ULL, 0x2001ULL, Abi::Read, 0}});
    tables.push_back(BdaPageFixture());
    tables.push_back(BdaSpreadFixture(1500));
    tables.push_back({{0, 0x1000, 0x1000ULL, Abi::Read, 0}, {0x10, 0x20, 0x3000ULL, Abi::Read | Abi::Write, 0}, {0x123456789aULL, 0x123456789bULL, 0x5000ULL, Abi::Read, 0}, {max - 0x100, max, 0x7000ULL, Abi::Read, 0}});
    std::vector<Abi::Range> equal;
    for (std::uint32_t index = 0; index < 10; ++index) equal.push_back({0x5000000000ULL, 0x5000000000ULL + 0x10u * (index + 1u), 0x9000ULL + index * 0x100u, Abi::Read | (index % 2u == 0u ? Abi::Write : 0u), 0});
    tables.push_back(equal);
    return tables;
}

inline std::vector<BdaQuery> BdaFixtureQueries(std::span<const ShaderRecompiler::BdaAbi::Range> ranges, bool wholePage) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint64_t> addresses{0, 1, 0xfff, 0x1000, max - 16, max - 15, max - 8, max - 1, max};
    for (const auto& range : ranges) {
        for (std::uint64_t delta = 0; delta < 3u; ++delta) {
            for (const auto point : {range.begin, range.end}) {
                addresses.push_back(point - 1u + delta);
            }
        }
    }
    if (wholePage) {
        for (std::uint64_t address = BdaFixturePage - 16u; address < BdaFixturePage + 0x1a50u; ++address) addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    std::vector<BdaQuery> queries;
    queries.reserve(addresses.size() * BdaFixtureWidths.size());
    for (const auto address : addresses) {
        for (const auto bytes : BdaFixtureWidths) queries.push_back({address, bytes});
    }
    return queries;
}

#endif

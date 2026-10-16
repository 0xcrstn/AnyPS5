#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using AgcDriver::Graphics::Recorder;
using AgcDriver::DrawTrace::QueryStatus;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void AvailablePairs() {
    const std::array<std::uint64_t, 4> values{100, 1, 140, 1};
    const auto result = Recorder::DecodeTraceQuery(VK_SUCCESS, values, true, 64, 2.5);
    Check(result.status == QueryStatus::Available && result.beginNs == 250 && result.endNs == 350, "timestamp and availability words were confused or period conversion changed");
    Check(result.beginTicks == 100 && result.endTicks == 140 && result.periodNs == 2.5 && result.durationNs == 100 && result.validBits == 64, "raw timestamps and integer-delta duration were not preserved");
    const auto partial = Recorder::DecodeTraceQuery(VK_NOT_READY, values, true, 64, 2.5);
    Check(partial.status == QueryStatus::Available && partial.endNs == 350, "available query pair was discarded because another pool query was not ready");
    const std::array<std::uint64_t, 4> equal{100, 1, 100, 1};
    const auto zero = Recorder::DecodeTraceQuery(VK_SUCCESS, equal, true, 64, 1);
    Check(zero.status == QueryStatus::Available && zero.beginTicks == zero.endTicks && zero.durationNs == 0, "measured zero duration became unavailable");
}

void LargeTimestampPrecision() {
    constexpr std::uint64_t origin = 1792104620702061600ull;
    for (const auto delta : {1ull, 63ull}) {
        const std::array<std::uint64_t, 4> values{origin, 1, origin + delta, 1};
        const auto result = Recorder::DecodeTraceQuery(VK_SUCCESS, values, true, 64, 1);
        Check(result.status == QueryStatus::Available && result.beginTicks == origin && result.endTicks == origin + delta, "large raw timestamps were rounded before retention");
        Check(result.beginNs == result.endNs && result.durationNs == static_cast<double>(delta), "small nonzero duration was lost by subtracting rounded absolute endpoints");
    }
    const std::array<std::uint64_t, 4> fractional{origin, 1, origin + 3, 1};
    const auto fractionalResult = Recorder::DecodeTraceQuery(VK_NOT_READY, fractional, true, 64, 2.5);
    Check(fractionalResult.status == QueryStatus::Available && fractionalResult.durationNs == 7.5 && fractionalResult.periodNs == 2.5, "fractional timestamp period was applied before integer subtraction");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::array<std::uint64_t, 4> nearLimit{maximum - 1, 1, maximum, 1};
    const auto limitResult = Recorder::DecodeTraceQuery(VK_SUCCESS, nearLimit, true, 64, 1);
    Check(limitResult.status == QueryStatus::Available && limitResult.beginTicks == maximum - 1 && limitResult.endTicks == maximum && limitResult.durationNs == 1, "near-limit tick delta was rounded or overflowed");
    const std::array<std::uint64_t, 4> zero{origin, 1, origin, 1};
    const auto zeroResult = Recorder::DecodeTraceQuery(VK_SUCCESS, zero, true, 64, 1);
    Check(zeroResult.status == QueryStatus::Available && zeroResult.beginTicks == zeroResult.endTicks && zeroResult.durationNs == 0, "true equal raw timestamps did not retain measured zero");
}

void TimestampWrap() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::array<std::uint64_t, 4> wrapped{maximum - 1, 1, 1, 1};
    const auto result = Recorder::DecodeTraceQuery(VK_SUCCESS, wrapped, true, 64, 2.5);
    Check(result.status == QueryStatus::Available && result.beginTicks == maximum - 1 && result.endTicks == 1 && result.durationNs == 7.5 && result.validBits == 64, "small forward64-bit timestamp wrap was rejected or mismeasured");
    Check(std::isfinite(result.beginNs) && result.endNs >= result.beginNs, "wrapped absolute coordinates were not finite and unwrapped");
    const std::array<std::uint64_t, 4> equalHalf{0, 1, std::uint64_t{1} << 63, 1};
    Check(Recorder::DecodeTraceQuery(VK_SUCCESS, equalHalf, true, 64, 1).status == QueryStatus::InvalidTimestamp, "ambiguous half-clock timestamp difference became a measured interval");
    const std::array<std::uint64_t, 4> belowHalf{0, 1, (std::uint64_t{1} << 63) - 1, 1};
    Check(Recorder::DecodeTraceQuery(VK_SUCCESS, belowHalf, true, 64, 1).status == QueryStatus::Available, "valid modular difference below the half-clock boundary was rejected");
    Check(Recorder::DecodeTraceQuery(VK_SUCCESS, wrapped, true, 32, 1).status == QueryStatus::Unsupported, "narrow timestamp support was fabricated by modular64-bit decoding");
}

void MissingAndFailedPairs() {
    for (const auto& values : {std::array<std::uint64_t, 4>{100, 0, 140, 1}, std::array<std::uint64_t, 4>{100, 1, 140, 0}, std::array<std::uint64_t, 4>{100, 0, 140, 0}}) {
        Check(Recorder::DecodeTraceQuery(VK_SUCCESS, values, true, 64, 1).status == QueryStatus::ReadFailed, "unavailable timestamps became a measured interval");
        Check(Recorder::DecodeTraceQuery(VK_NOT_READY, values, true, 64, 1).status == QueryStatus::ReadFailed, "partially unavailable pair became a measured interval");
    }
    const std::array<std::uint64_t, 4> values{100, 1, 140, 1};
    const auto failed = Recorder::DecodeTraceQuery(VK_ERROR_DEVICE_LOST, values, true, 64, 1);
    Check(failed.status == QueryStatus::ReadFailed && failed.beginTicks == 0 && failed.endTicks == 0 && failed.periodNs == 0 && failed.durationNs == 0, "failed query read retained stale raw timestamp measurements");
    Check(Recorder::DecodeTraceQuery(VK_SUCCESS, values, false, 64, 1).status == QueryStatus::Uncollected, "unended command acquired a complete query interval");
}

void InvalidClocks() {
    const std::array<std::uint64_t, 4> reversed{140, 1, 100, 1};
    Check(Recorder::DecodeTraceQuery(VK_SUCCESS, reversed, true, 64, 1).status == QueryStatus::InvalidTimestamp, "reversed timestamps underflowed into a huge GPU interval");
    const std::array<std::uint64_t, 4> values{100, 1, 140, 1};
    for (const auto bits : {0u, 32u, 63u, 65u}) {
        Check(Recorder::DecodeTraceQuery(VK_SUCCESS, values, true, bits, 1).status == QueryStatus::Unsupported, "unsupported timestamp width was interpreted as a full GPU clock");
    }
    for (const auto period : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        Check(Recorder::DecodeTraceQuery(VK_SUCCESS, values, true, 64, period).status == QueryStatus::Unsupported, "invalid timestamp period became measured time");
    }
    const std::array<std::uint64_t, 4> huge{1, 1, 3, 1};
    Check(Recorder::DecodeTraceQuery(VK_SUCCESS, huge, true, 64, std::numeric_limits<double>::max()).status == QueryStatus::InvalidTimestamp, "overflowed timestamp conversion was accepted");
}

void HardwareStatisticsAvailability() {
    const std::array<std::uint64_t, 8> zero{0, 0, 0, 0, 0, 0, 0, 1};
    Check(Recorder::DecodeTraceStatistics(VK_SUCCESS, zero, true) == QueryStatus::Available, "available zero hardware statistics became missing work");
    Check(Recorder::DecodeTraceStatistics(VK_NOT_READY, zero, true) == QueryStatus::Available, "available row was discarded when another query was pending");
    auto unavailable = zero;
    unavailable.back() = 0;
    Check(Recorder::DecodeTraceStatistics(VK_SUCCESS, unavailable, true) == QueryStatus::ReadFailed, "unavailable hardware statistics became measured zero");
    Check(Recorder::DecodeTraceStatistics(VK_ERROR_DEVICE_LOST, zero, true) == QueryStatus::ReadFailed, "failed statistics read retained measurements");
    Check(Recorder::DecodeTraceStatistics(VK_SUCCESS, zero, false) == QueryStatus::Uncollected, "unfinished statistics query became available");
    Check(Recorder::TraceStatisticNames.size() == 7 && Recorder::TraceStatisticUnits.size() == 7, "statistics decoder shape differs from query-pool flags");
}

void RawHostClockConversion() {
    constexpr std::uint64_t origin = 1792106218872829760ull;
    const auto nanosecond = Recorder::DecodeHostClockOffsetNs(origin, origin + 1, 1000000000ull);
    Check(nanosecond && *nanosecond == 1, "raw host tick difference was rounded at large origins");
    const auto qpc = Recorder::DecodeHostClockOffsetNs(origin, origin + 3, 10000000ull);
    Check(qpc && *qpc == 300, "host counter frequency was omitted from calibration conversion");
    Check(!Recorder::DecodeHostClockOffsetNs(origin, origin - 1, 10000000ull), "reversed host conversion counter was accepted");
    Check(!Recorder::DecodeHostClockOffsetNs(origin, origin, 0), "unknown host frequency was accepted");
    const auto zero = Recorder::DecodeHostClockOffsetNs(origin, origin, 10000000ull);
    Check(zero && *zero == 0, "equal host counters lost valid zero offset");
}

void ExternalProfilerLabels() {
    constexpr std::string_view run = "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view capture = "22222222-2222-4222-8222-222222222222";
    std::array<char, 256> output{};
    Check(Recorder::FormatTraceLabel(output, run, capture, 123, 456, 789, 1011), "complete profiler label was refused");
    Check(std::string_view(output.data()) == "aps5 run=11111111-1111-4111-8111-111111111111 capture=22222222-2222-4222-8222-222222222222 op=123 batch=456 pipeline=789 command=1011", "profiler label IDs changed wire format");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    Check(Recorder::FormatTraceLabel(output, run, capture, maximum, maximum, maximum, maximum), "full uint64 profiler IDs did not fit the bounded stack buffer");
    Check(std::string_view(output.data()).ends_with("command=18446744073709551615"), "profiler command ID was truncated");
    std::array<char, 16> shortOutput{};
    Check(!Recorder::FormatTraceLabel(shortOutput, run, capture, 1, 2, 3, 4), "truncated profiler label was accepted for an external join");
    Check(!Recorder::FormatTraceLabel({}, run, capture, 1, 2, 3, 4), "empty label buffer was accepted");
}

void BoundedBatchOrigins() {
    using Result = Recorder::TraceContributorResult;
    std::array<AgcDriver::DrawTrace::Id, 4> table{};
    Check(Recorder::InsertTraceContributor(table, 0) == Result::Existing, "unknown origin entered the batch membership table");
    Check(Recorder::InsertTraceContributor(table, 4) == Result::Inserted, "first colliding batch origin was dropped");
    Check(Recorder::InsertTraceContributor(table, 8) == Result::Inserted, "linear probe lost a colliding origin");
    Check(Recorder::InsertTraceContributor(table, 4) == Result::Existing, "duplicate batch origin was emitted again");
    Check(Recorder::InsertTraceContributor(table, 12) == Result::Inserted && Recorder::InsertTraceContributor(table, 16) == Result::Inserted, "last bounded membership slots were lost");
    Check(Recorder::InsertTraceContributor(table, 20) == Result::CapacityDropped, "full origin ledger silently claimed complete lineage");
    Check(Recorder::InsertTraceContributor(table, 8) == Result::Existing, "full ledger forgot an already recorded origin");
    Check(Recorder::InsertTraceContributor({}, 1) == Result::CapacityDropped, "empty origin ledger claimed a recorded dependency");
}

void QueryBudgets() {
    Check(Recorder::TraceQueryPrecondition(QueryStatus::Pending, 511, 512) == QueryStatus::Pending, "last available query slot was refused");
    Check(Recorder::TraceQueryPrecondition(QueryStatus::Pending, 512, 512) == QueryStatus::CapacityDropped, "full pool did not record capacity loss");
    Check(Recorder::TraceQueryPrecondition(QueryStatus::Pending, 0, 0) == QueryStatus::CapacityDropped, "zero capacity became a usable query pool");
    Check(Recorder::TraceQueryPrecondition(QueryStatus::AllocationFailed, 0, 512) == QueryStatus::AllocationFailed, "allocation failure was confused with zero GPU work");
    Check(Recorder::TraceQueryPrecondition(QueryStatus::Unsupported, 0, 512) == QueryStatus::Unsupported, "unsupported query path was silently measured");
}

}

int main() {
    try {
        AvailablePairs();
        LargeTimestampPrecision();
        TimestampWrap();
        MissingAndFailedPairs();
        InvalidClocks();
        HardwareStatisticsAvailability();
        RawHostClockConversion();
        ExternalProfilerLabels();
        BoundedBatchOrigins();
        QueryBudgets();
        std::cout << "GPU attribution query decoding and preconditions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

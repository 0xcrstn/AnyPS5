#include "prx/libSceAgcDriver/Graphics/include/LabelVersions.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace AgcDriver::Graphics::LabelVersions {

namespace {

constexpr std::size_t SlotCount = 1u << 16u;
constexpr std::size_t Buckets = 40;

struct Slot {
    std::atomic<std::uint64_t> stamp{0};
    std::atomic<std::uint64_t> address{0};
    std::atomic<std::uint32_t> bytes{0};
    std::atomic<std::uint32_t> queue{0};
    std::array<std::atomic<std::int64_t>, MilestoneCount> ns{};
};

std::array<Slot, SlotCount>& Slots() {
    static std::array<Slot, SlotCount> slots;
    return slots;
}

std::atomic<int>& EnabledState() {
    static std::atomic<int> state{-1};
    return state;
}

struct Stage {
    std::array<std::uint64_t, Buckets> histogram{};
    std::uint64_t count = 0;
    double sumUs = 0;
    double maxUs = 0;

    void Add(double us) {
        ++count;
        sumUs += us;
        maxUs = std::max(maxUs, us);
        const auto bucket = us < 1 ? 0u : std::min<std::size_t>(Buckets - 1, std::bit_width(static_cast<std::uint64_t>(us)));
        ++histogram[bucket];
    }

    double Percentile(double fraction) const {
        if (count == 0) return 0;
        const auto target = static_cast<std::uint64_t>(fraction * static_cast<double>(count - 1)) + 1;
        std::uint64_t seen = 0;
        for (std::size_t bucket = 0; bucket < Buckets; ++bucket) {
            seen += histogram[bucket];
            if (seen >= target) return bucket == 0 ? 1.0 : static_cast<double>(1ull << bucket);
        }
        return maxUs;
    }
};

struct Pipeline {
    std::mutex mutex;
    std::array<Stage, 3> stages;
    std::uint64_t published = 0;
    std::uint64_t evicted = 0;
};

Pipeline& Stats() {
    static Pipeline pipeline;
    return pipeline;
}

Slot* Owned(std::uint64_t stamp) {
    if (stamp == 0) return nullptr;
    auto& slot = Slots()[stamp % SlotCount];
    return slot.stamp.load(std::memory_order_acquire) == stamp ? &slot : nullptr;
}

void Fold(const Slot& slot) {
    std::array<std::int64_t, MilestoneCount> at{};
    for (std::size_t i = 0; i < MilestoneCount; ++i) at[i] = slot.ns[i].load(std::memory_order_acquire);
    auto& stats = Stats();
    std::lock_guard lock(stats.mutex);
    ++stats.published;
    for (std::size_t stage = 0; stage < 3; ++stage) {
        if (at[stage] == 0 || at[stage + 1] == 0) continue;
        stats.stages[stage].Add(static_cast<double>(std::max<std::int64_t>(0, at[stage + 1] - at[stage])) / 1000.0);
    }
}

}

bool Enabled() {
    auto state = EnabledState().load(std::memory_order_relaxed);
    if (state < 0) {
        const auto set = [](const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && std::strcmp(value, "0") != 0;
        };
        state = set("APS5_TRACE_WAIT_CHAIN") || set("APS5_PROFILE_DRAW") ? 1 : 0;
        EnabledState().store(state, std::memory_order_relaxed);
    }
    return state == 1;
}

void SetEnabledForTest(bool enabled) {
    EnabledState().store(enabled ? 1 : 0, std::memory_order_relaxed);
}

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Record(std::uint64_t stamp, std::uint64_t address, std::uint32_t bytes, std::uint32_t queue) {
    if (stamp == 0 || !Enabled()) return;
    auto& slot = Slots()[stamp % SlotCount];
    if (slot.stamp.load(std::memory_order_acquire) == stamp) return;
    const auto published = [&](Milestone milestone) { return slot.ns[static_cast<std::size_t>(milestone)].load(std::memory_order_relaxed) != 0; };
    if (const auto previous = slot.stamp.exchange(0, std::memory_order_acq_rel); previous != 0 && published(Milestone::Submitted) && !published(Milestone::HostPublished)) {
        std::lock_guard lock(Stats().mutex);
        ++Stats().evicted;
    }
    slot.address.store(address, std::memory_order_relaxed);
    slot.bytes.store(bytes, std::memory_order_relaxed);
    slot.queue.store(queue, std::memory_order_relaxed);
    for (auto& at : slot.ns) at.store(0, std::memory_order_relaxed);
    slot.ns[static_cast<std::size_t>(Milestone::Recorded)].store(NowNs(), std::memory_order_relaxed);
    slot.stamp.store(stamp, std::memory_order_release);
}

void Mark(std::uint64_t stamp, Milestone milestone) {
    auto* slot = Owned(stamp);
    if (slot == nullptr) return;
    auto& at = slot->ns[static_cast<std::size_t>(milestone)];
    std::int64_t expected = 0;
    if (!at.compare_exchange_strong(expected, NowNs(), std::memory_order_acq_rel)) return;
    if (milestone == Milestone::HostPublished) Fold(*slot);
}

void MarkAll(std::span<const std::uint64_t> stamps, Milestone milestone) {
    for (const auto stamp : stamps) Mark(stamp, milestone);
}

void MarkHostWritten(std::uint64_t stamp) {
    auto* slot = Owned(stamp);
    if (slot == nullptr) return;
    const auto now = NowNs();
    for (std::size_t i = 1; i < MilestoneCount; ++i) {
        std::int64_t expected = 0;
        slot->ns[i].compare_exchange_strong(expected, now, std::memory_order_acq_rel);
    }
}

std::optional<Version> Find(std::uint64_t stamp) {
    auto* slot = Owned(stamp);
    if (slot == nullptr) return std::nullopt;
    Version version;
    version.stamp = stamp;
    version.address = slot->address.load(std::memory_order_relaxed);
    version.bytes = slot->bytes.load(std::memory_order_relaxed);
    version.queue = slot->queue.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < MilestoneCount; ++i) version.ns[i] = slot->ns[i].load(std::memory_order_acquire);
    if (slot->stamp.load(std::memory_order_acquire) != stamp) return std::nullopt;
    return version;
}

std::string Describe(const Version& version, std::int64_t originNs) {
    std::string text = "label version stamp " + std::to_string(version.stamp) + " by queue " + std::to_string(version.queue);
    for (std::size_t i = 0; i < MilestoneCount; ++i) {
        char part[64];
        if (version.ns[i] == 0) std::snprintf(part, sizeof(part), ", %s -", MilestoneNames[i]);
        else std::snprintf(part, sizeof(part), ", %s %+.2f ms", MilestoneNames[i], static_cast<double>(version.ns[i] - originNs) / 1e6);
        text += part;
    }
    return text;
}

std::string TakeReport() {
    auto& stats = Stats();
    std::lock_guard lock(stats.mutex);
    constexpr std::array<const char*, 3> names{"recorded->submitted", "submitted->device-ready", "device-ready->host-published"};
    char text[256];
    std::snprintf(text, sizeof(text), "%llu label versions published, %llu evicted unpublished", static_cast<unsigned long long>(stats.published), static_cast<unsigned long long>(stats.evicted));
    std::string report = text;
    for (std::size_t stage = 0; stage < 3; ++stage) {
        const auto& s = stats.stages[stage];
        std::snprintf(text, sizeof(text), "; %s n %llu mean %.0f us p50 <=%.0f us p90 <=%.0f us max %.0f us", names[stage], static_cast<unsigned long long>(s.count), s.count != 0 ? s.sumUs / static_cast<double>(s.count) : 0.0, s.Percentile(0.5), s.Percentile(0.9), s.maxUs);
        report += text;
    }
    stats.stages = {};
    stats.published = 0;
    stats.evicted = 0;
    return report;
}

}

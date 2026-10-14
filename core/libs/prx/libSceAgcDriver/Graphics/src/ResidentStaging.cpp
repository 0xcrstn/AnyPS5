#include "prx/libSceAgcDriver/Graphics/include/ResidentStaging.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <limits>
#include <mutex>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

constexpr std::size_t SiteCount = static_cast<std::size_t>(ResidentSite::Count);
constexpr const char* SiteNames[SiteCount] = {"hook", "scanout", "in-place", "copy-in", "address-based", "indirect", "copy", "label", "upload", "completion", "eviction", "retire"};

std::uint64_t residentBudget() {
    static const std::uint64_t bytes = [] {
        const auto* value = std::getenv("APS5_RESIDENT_STAGING_MIB");
        return (value != nullptr ? std::strtoull(value, nullptr, 10) : 256ull) << 20u;
    }();
    return bytes;
}

struct Entry {
    std::uint64_t end;
    std::shared_ptr<Buffer> shadow;
    std::uint64_t shadowBase;
    VkBuffer import;
    std::uint64_t importBase;
    std::uint64_t generation;
    VkDevice device;
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, Entry> entries;
    std::map<const Buffer*, std::size_t> shadows;
    std::atomic<std::size_t> count{0};
    std::uint64_t bytes = 0;
    std::uint64_t bufferBytes = 0;
};

void noteShadow(Registry& registry, const Entry& entry) {
    const auto [found, inserted] = registry.shadows.try_emplace(entry.shadow.get(), 0);
    if (inserted) registry.bufferBytes += entry.shadow->Size();
    ++found->second;
}

void forgetShadow(Registry& registry, const Entry& entry) {
    const auto found = registry.shadows.find(entry.shadow.get());
    Require(found != registry.shadows.end() && found->second != 0, "device-resident buffer accounting lost its shadow");
    if (--found->second != 0) return;
    registry.bufferBytes -= entry.shadow->Size();
    registry.shadows.erase(found);
}

Registry& Resident() {
    static Registry registry;
    return registry;
}

struct Statistics {
    std::atomic<std::uint64_t> noted{0}, notedBytes{0};
    std::array<std::atomic<std::uint64_t>, SiteCount> published{}, publishedBytes{};
    std::atomic<std::uint64_t> refusedStoredOver{0}, refusedStoredOverBytes{0};
    std::atomic<std::uint64_t> verified{0}, verifiedBytes{0}, verifyDiffering{0}, verifyDifferingBytes{0};
};

Statistics& Stats() {
    static Statistics statistics;
    return statistics;
}

struct Piece {
    std::uint64_t begin;
    std::uint64_t end;
    std::shared_ptr<Buffer> shadow;
    std::uint64_t shadowBase;
    VkBuffer import;
    std::uint64_t importBase;
    std::uint64_t generation;
    VkDevice device;
};

void cutLocked(Registry& registry, std::uint64_t begin, std::uint64_t end, std::vector<Piece>* taken) {
    auto it = registry.entries.upper_bound(begin);
    if (it != registry.entries.begin()) --it;
    while (it != registry.entries.end() && it->first < end) {
        const auto entryBegin = it->first;
        auto entry = it->second;
        if (entry.end <= begin) {
            ++it;
            continue;
        }
        const auto from = std::max(begin, entryBegin);
        const auto to = std::min(end, entry.end);
        if (taken != nullptr) taken->push_back({from, to, entry.shadow, entry.shadowBase, entry.import, entry.importBase, entry.generation, entry.device});
        registry.bytes -= entry.end - entryBegin;
        forgetShadow(registry, entry);
        it = registry.entries.erase(it);
        if (entryBegin < from) {
            auto head = entry;
            head.end = from;
            registry.entries.emplace(entryBegin, head);
            noteShadow(registry, head);
            registry.bytes += from - entryBegin;
        }
        if (to < entry.end) {
            it = registry.entries.emplace(to, entry).first;
            noteShadow(registry, entry);
            registry.bytes += entry.end - to;
            ++it;
        }
    }
    registry.count.store(registry.entries.size(), std::memory_order_release);
}

template<typename TSelect>
std::vector<Piece> selectLocked(const Registry& registry, const TSelect& select) {
    std::vector<Piece> selected;
    for (const auto& [begin, entry] : registry.entries) {
        if (select(begin, entry)) selected.push_back({begin, entry.end, entry.shadow, entry.shadowBase, entry.import, entry.importBase, entry.generation, entry.device});
    }
    return selected;
}

std::uint64_t rangeEnd(std::uint64_t address, std::size_t bytes) {
    Require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "device-resident buffer range wraps its address");
    return address + bytes;
}

std::uint64_t publishPieces(Recorder& recorder, const std::vector<Piece>& pieces, ResidentSite site) {
    if (pieces.empty()) return 0;
    const auto& context = recorder.GetContext();
    for (const auto& piece : pieces) {
        Require(piece.device == context.device, "publishing device-resident buffer results through another device is not implemented");
        const auto bytes = static_cast<std::size_t>(piece.end - piece.begin);
        static_cast<void>(GuestMemory::CollectWritesUncached(piece.begin, bytes));
        if (piece.generation == 0 || GuestMemory::StoredOver(piece.begin, bytes, piece.generation)) {
            Stats().refusedStoredOver.fetch_add(1, std::memory_order_relaxed);
            Stats().refusedStoredOverBytes.fetch_add(bytes, std::memory_order_relaxed);
            Require(false, "publishing device-resident buffer results after another writer changed the range is not implemented");
        }
    }
    const bool verify = LazyStagingVerify();
    const auto commands = recorder.Commands();
    const auto timing = recorder.BeginGpuTiming(Recorder::CommandClass::StagingOut);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    std::uint64_t total = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto& piece : pieces) {
        const auto bytes = piece.end - piece.begin;
        if (verify) {
            auto scratch = std::make_shared<Buffer>(context, static_cast<std::size_t>(bytes * 2), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            CopyBuffer(context, commands, piece.shadow->Handle(), piece.begin - piece.shadowBase, scratch->Handle(), 0, bytes);
            CopyBuffer(context, commands, piece.import, piece.begin - piece.importBase, scratch->Handle(), bytes, bytes);
            recorder.Keep(scratch);
            recorder.OnCompleteWithoutGuestAccess([scratch, bytes] {
                const auto held = scratch->Bytes();
                std::uint64_t differing = 0;
                for (std::uint64_t offset = 0; offset < bytes; ++offset) {
                    if (held[static_cast<std::size_t>(offset)] != held[static_cast<std::size_t>(bytes + offset)]) ++differing;
                }
                Stats().verified.fetch_add(1, std::memory_order_relaxed);
                Stats().verifiedBytes.fetch_add(bytes, std::memory_order_relaxed);
                if (differing != 0) {
                    Stats().verifyDiffering.fetch_add(1, std::memory_order_relaxed);
                    Stats().verifyDifferingBytes.fetch_add(differing, std::memory_order_relaxed);
                }
            });
        } else {
            CopyBuffer(context, commands, piece.shadow->Handle(), piece.begin - piece.shadowBase, piece.import, piece.begin - piece.importBase, bytes);
        }
        ranges.emplace_back(piece.begin, piece.end);
        recorder.Keep(piece.shadow);
        total += bytes;
        Stats().published[static_cast<std::size_t>(site)].fetch_add(1, std::memory_order_relaxed);
        Stats().publishedBytes[static_cast<std::size_t>(site)].fetch_add(bytes, std::memory_order_relaxed);
    }
    constexpr VkAccessFlags copiedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, copiedAccess);
    Recorder::CountBarriers(Recorder::CommandClass::StagingOut, 2);
    recorder.MarkCovered(copiedAccess);
    recorder.EndGpuTiming(timing, total);
    if (!ranges.empty()) {
        recorder.NotePendingWrites(ranges);
        for (const auto& [begin, end] : ranges) static_cast<void>(GuestMemory::MarkWritten(begin, static_cast<std::size_t>(end - begin)));
    }
    auto& registry = Resident();
    {
        std::lock_guard lock(registry.mutex);
        for (const auto& piece : pieces) cutLocked(registry, piece.begin, piece.end, nullptr);
    }
    recorder.RefreshPendingWrites();
    return total;
}

}

bool LazyStagingOut() {
    static const bool enabled = std::getenv("APS5_LAZY_STAGING_OUT") != nullptr && std::strcmp(std::getenv("APS5_LAZY_STAGING_OUT"), "0") != 0;
    return enabled;
}

bool LazyStagingVerify() {
    static const bool verify = LazyStagingOut() && std::getenv("APS5_LAZY_STAGING_VERIFY") != nullptr;
    return verify;
}

const char* ResidentSiteName(ResidentSite site) {
    const auto index = static_cast<std::size_t>(site);
    return index < SiteCount ? SiteNames[index] : "?";
}

void NoteDeviceResident(VkDevice device, std::uint64_t begin, std::uint64_t end, std::shared_ptr<Buffer> shadow, std::uint64_t shadowBase, VkBuffer import, std::uint64_t importBase, std::uint64_t generation) {
    if (end <= begin || shadow == nullptr) return;
    Require(generation != 0, "device-resident buffer results need a watched generation");
    auto& registry = Resident();
    bool overBudget = false;
    {
        std::lock_guard lock(registry.mutex);
        cutLocked(registry, begin, end, nullptr);
        const auto inserted = registry.entries.emplace(begin, Entry{end, std::move(shadow), shadowBase, import, importBase, generation, device}).first;
        noteShadow(registry, inserted->second);
        registry.bytes += end - begin;
        registry.count.store(registry.entries.size(), std::memory_order_release);
        overBudget = registry.bufferBytes > residentBudget();
    }
    Stats().noted.fetch_add(1, std::memory_order_relaxed);
    Stats().notedBytes.fetch_add(end - begin, std::memory_order_relaxed);
    if (auto* active = Recorder::Active(); active != nullptr && active->GetContext().device == device) active->RefreshPendingWrites();
    if (overBudget) {
        auto* recorder = Recorder::Active();
        Require(recorder != nullptr && recorder->GetContext().device == device, "evicting device-resident buffer results needs their active recorder");
        PublishAllDeviceResident(*recorder, ResidentSite::Eviction);
    }
}

void RefreshDeviceResident(std::uint64_t begin, std::uint64_t end, std::uint64_t since, std::uint64_t generation) {
    auto& registry = Resident();
    if (end <= begin || registry.count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard lock(registry.mutex);
    auto it = registry.entries.upper_bound(begin);
    if (it != registry.entries.begin()) --it;
    for (; it != registry.entries.end() && it->first < end; ++it) {
        if (it->second.end > begin && it->first >= begin && it->second.end <= end && (since == 0 || it->second.generation == since)) it->second.generation = generation;
    }
}

bool AnyDeviceResident() {
    return Resident().count.load(std::memory_order_acquire) != 0;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> DeviceResidentRanges(VkDevice device) {
    auto& registry = Resident();
    if (registry.count.load(std::memory_order_acquire) == 0) return {};
    std::lock_guard lock(registry.mutex);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(registry.entries.size());
    for (const auto& [begin, entry] : registry.entries) {
        if (entry.device == device) ranges.emplace_back(begin, entry.end);
    }
    return ranges;
}

bool AnyDeviceResidentOverlaps(std::uint64_t address, std::size_t bytes) {
    auto& registry = Resident();
    if (bytes == 0 || registry.count.load(std::memory_order_acquire) == 0) return false;
    const auto end = rangeEnd(address, bytes);
    std::lock_guard lock(registry.mutex);
    auto it = registry.entries.upper_bound(address);
    if (it != registry.entries.begin()) --it;
    for (; it != registry.entries.end() && it->first < end; ++it) {
        if (it->second.end > address) return true;
    }
    return false;
}

std::uint64_t PublishDeviceResident(Recorder& recorder, std::uint64_t address, std::size_t bytes, ResidentSite site) {
    auto& registry = Resident();
    if (bytes == 0 || registry.count.load(std::memory_order_acquire) == 0) return 0;
    const auto end = rangeEnd(address, bytes);
    std::vector<Piece> taken;
    {
        std::lock_guard lock(registry.mutex);
        taken = selectLocked(registry, [&](std::uint64_t begin, const Entry& entry) { return begin < end && address < entry.end; });
        if (site != ResidentSite::Hook && site != ResidentSite::Scanout) {
            for (auto& piece : taken) {
                piece.begin = std::max(piece.begin, address);
                piece.end = std::min(piece.end, end);
            }
        }
    }
    return publishPieces(recorder, taken, site);
}

std::uint64_t PublishAllDeviceResident(Recorder& recorder, ResidentSite site) {
    auto& registry = Resident();
    if (registry.count.load(std::memory_order_acquire) == 0) return 0;
    std::vector<Piece> taken;
    {
        std::lock_guard lock(registry.mutex);
        taken = selectLocked(registry, [&](std::uint64_t, const Entry& entry) { return entry.device == recorder.GetContext().device; });
    }
    return publishPieces(recorder, taken, site);
}

std::uint64_t PublishDeviceResidentOfImport(Recorder& recorder, VkBuffer import, ResidentSite site) {
    auto& registry = Resident();
    if (registry.count.load(std::memory_order_acquire) == 0) return 0;
    std::vector<Piece> taken;
    {
        std::lock_guard lock(registry.mutex);
        taken = selectLocked(registry, [&](std::uint64_t, const Entry& entry) { return entry.import == import && entry.device == recorder.GetContext().device; });
    }
    return publishPieces(recorder, taken, site);
}

void ClearDeviceResident(VkDevice device) {
    auto& registry = Resident();
    {
        std::lock_guard lock(registry.mutex);
        for (auto it = registry.entries.begin(); it != registry.entries.end();) {
            if (it->second.device != device) {
                ++it;
                continue;
            }
            registry.bytes -= it->second.end - it->first;
            forgetShadow(registry, it->second);
            it = registry.entries.erase(it);
        }
        registry.count.store(registry.entries.size(), std::memory_order_release);
    }
    if (auto* active = Recorder::Active(); active != nullptr && active->GetContext().device == device) active->RefreshPendingWrites();
}

ResidentCounters DeviceResidentTotals() {
    ResidentCounters totals;
    auto& registry = Resident();
    {
        std::lock_guard lock(registry.mutex);
        totals.live = registry.entries.size();
        totals.liveBytes = registry.bytes;
        totals.liveBufferBytes = registry.bufferBytes;
    }
    auto& stats = Stats();
    const auto take = [](const std::atomic<std::uint64_t>& value) { return value.load(std::memory_order_relaxed); };
    totals.noted = take(stats.noted);
    totals.notedBytes = take(stats.notedBytes);
    for (std::size_t site = 0; site < SiteCount; ++site) {
        totals.published[site] = take(stats.published[site]);
        totals.publishedBytes[site] = take(stats.publishedBytes[site]);
    }
    totals.refusedStoredOver = take(stats.refusedStoredOver);
    totals.refusedStoredOverBytes = take(stats.refusedStoredOverBytes);
    totals.verified = take(stats.verified);
    totals.verifiedBytes = take(stats.verifiedBytes);
    totals.verifyDiffering = take(stats.verifyDiffering);
    totals.verifyDifferingBytes = take(stats.verifyDifferingBytes);
    return totals;
}

std::string ResidentReport() {
    if (!LazyStagingOut()) return {};
    static ResidentCounters previous{};
    const auto totals = DeviceResidentTotals();
    const auto mib = [](std::uint64_t bytes) { return static_cast<double>(bytes) / 1048576.0; };
    char text[1024];
    std::uint64_t publishedBytes = 0;
    std::string sites;
    for (std::size_t site = 0; site < SiteCount; ++site) {
        const auto count = totals.published[site] - previous.published[site];
        const auto bytes = totals.publishedBytes[site] - previous.publishedBytes[site];
        publishedBytes += bytes;
        if (count == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu/%.1f", SiteNames[site], static_cast<unsigned long long>(count), mib(bytes));
        sites += text;
    }
    const auto notedBytes = totals.notedBytes - previous.notedBytes;
    std::snprintf(text, sizeof(text), "; device-resident buffers%s: %llu ranges live (%.1f MiB in %.1f MiB of retained buffers), noted %llu/%.1f, published by site {%s }; refused after another writer %llu/%.1f; verify %llu/%.1f checked, %llu/%.1f differing", LazyStagingVerify() ? " (verify)" : "", static_cast<unsigned long long>(totals.live), mib(totals.liveBytes), mib(totals.liveBufferBytes), static_cast<unsigned long long>(totals.noted - previous.noted), mib(notedBytes), sites.c_str(), static_cast<unsigned long long>(totals.refusedStoredOver - previous.refusedStoredOver), mib(totals.refusedStoredOverBytes - previous.refusedStoredOverBytes), static_cast<unsigned long long>(totals.verified - previous.verified), mib(totals.verifiedBytes - previous.verifiedBytes), static_cast<unsigned long long>(totals.verifyDiffering - previous.verifyDiffering), mib(totals.verifyDifferingBytes - previous.verifyDifferingBytes));
    previous = totals;
    return text;
}

}

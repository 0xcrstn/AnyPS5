#include "prx/libSceAgcDriver/Graphics/include/ResidentMirrors.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace AgcDriver::Graphics {

struct ResidentMirror {
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t base = 0;
    std::uint64_t bytes = 0;
    VkBuffer source = VK_NULL_HANDLE;
    std::shared_ptr<Buffer> buffer;
    VkDeviceAddress address = 0;
    std::shared_ptr<Buffer> pristine;
    VkDeviceAddress pristineAddress = 0;
    const Recorder* placedBy = nullptr;
    std::vector<std::uint64_t> generations;
    std::vector<std::uint8_t> unsynced;
    std::vector<std::uint8_t> written;
    bool anyUnsynced = false;
    bool anyWritten = false;
    const Recorder* usedBy = nullptr;
    std::uint64_t usedBatch = 0;
    std::uint32_t churn = 0;
    bool retired = false;
};

namespace {

constexpr std::uint64_t BlockBytes = 65536;

constexpr std::uint64_t DeniedBatches = 256;
constexpr std::uint64_t VerifyBytesPerBatch = 64ull << 20u;

struct Statistics {
    std::atomic<std::uint64_t> made{0}, refused{0}, uses{0}, refreshes{0}, refreshedBlocks{0}, refreshedBytes{0}, midBatchSyncs{0}, midBatchBytes{0}, invalidatedBlocks{0};
    std::atomic<std::uint64_t> collectNs{0};
    std::atomic<std::uint64_t> deferred{0}, denied{0}, deniedUses{0}, verified{0}, verifiedBytes{0}, verifyDiffering{0};
};

struct Seen {
    std::uint64_t lastBatch = 0;
    std::uint32_t batches = 0;
    std::uint64_t deniedUntil = 0;
};

Statistics& Stats() {
    static Statistics statistics;
    return statistics;
}

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<ResidentMirror>> byBase;
    std::vector<std::shared_ptr<ResidentMirror>> used;
    std::map<std::uint64_t, Seen> seen;
    std::vector<std::tuple<const Recorder*, std::uint64_t, std::uint64_t>> pointerWrites;
    std::uint64_t bytes = 0;
    std::atomic<bool> any{false};
};

Registry& Mirrors() {
    static Registry registry;
    return registry;
}

std::uint64_t BudgetBytes() {
    static const std::uint64_t bytes = [] {
        const char* text = std::getenv("APS5_RESIDENT_MIRROR_MIB");
        return (text != nullptr ? std::strtoull(text, nullptr, 10) : 4096ull) << 20u;
    }();
    return bytes;
}

std::uint32_t MirrorAfterBatches() {
    static const std::uint32_t batches = [] {
        const char* text = std::getenv("APS5_RESIDENT_MIRROR_AFTER");
        return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10)) : 2u;
    }();
    return batches;
}

std::uint32_t ChurnLimit() {
    static const std::uint32_t limit = [] {
        const char* text = std::getenv("APS5_RESIDENT_MIRROR_CHURN");
        return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10)) : 4u;
    }();
    return limit;
}

bool VerifyMirrors() {
    static const bool verify = [] {
        const char* text = std::getenv("APS5_VERIFY_RESIDENT_MIRRORS");
        return text != nullptr && std::strcmp(text, "0") != 0;
    }();
    return verify;
}

std::uint64_t Footprint(const ResidentMirror& mirror) {
    return mirror.pristine != nullptr ? mirror.bytes * 2u : mirror.bytes;
}

struct PlacementPipeline {
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
};

struct PlacementPipelines {
    std::mutex mutex;
    std::vector<PlacementPipeline> entries;
};

PlacementPipelines& Pipelines() {
    static PlacementPipelines pipelines;
    return pipelines;
}

PlacementPipeline CopyBackPipeline(const Context& context) {
    auto& pipelines = Pipelines();
    std::lock_guard lock(pipelines.mutex);
    for (const auto& entry : pipelines.entries) {
        if (entry.device == context.device) return entry;
    }
    PlacementPipeline entry;
    entry.device = context.device;
    entry.destroyPipeline = context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    entry.destroyLayout = context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    const auto code = ShaderRecompiler::BdaPlacement::BuildCopyBackShader();
    VkShaderModule module = VK_NULL_HANDLE;
    try {
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ShaderRecompiler::BdaPlacement::PushConstants)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &entry.layout), "vkCreatePipelineLayout BDA placement copy-back");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = code.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = code.data();
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule BDA placement copy-back");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = entry.layout;
        {
            auto pipelineCacheLock = context.LockPipelineCacheForUse();
            Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &pipelineInfo, nullptr, &entry.pipeline), "vkCreateComputePipelines BDA placement copy-back");
        }
        context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
    } catch (...) {
        if (module != VK_NULL_HANDLE) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (entry.layout != VK_NULL_HANDLE) entry.destroyLayout(context.device, entry.layout, nullptr);
        throw;
    }
    pipelines.entries.push_back(entry);
    return entry;
}

void DestroyCopyBackPipeline(VkDevice device) {
    auto& pipelines = Pipelines();
    std::lock_guard lock(pipelines.mutex);
    for (auto it = pipelines.entries.begin(); it != pipelines.entries.end();) {
        if (it->device != device) {
            ++it;
            continue;
        }
        it->destroyPipeline(device, it->pipeline, nullptr);
        it->destroyLayout(device, it->layout, nullptr);
        it = pipelines.entries.erase(it);
    }
}

std::size_t BlockCount(std::uint64_t base, std::uint64_t bytes) {
    return static_cast<std::size_t>((base + bytes - 1) / BlockBytes - base / BlockBytes + 1);
}

std::pair<std::uint64_t, std::uint64_t> BlockRange(const ResidentMirror& mirror, std::size_t block) {
    const auto first = mirror.base / BlockBytes * BlockBytes + block * BlockBytes;
    return {std::max(first, mirror.base), std::min(first + BlockBytes, mirror.base + mirror.bytes)};
}

void markBlocks(ResidentMirror& mirror, std::uint64_t begin, std::uint64_t end) {
    const auto from = std::max(begin, mirror.base);
    const auto to = std::min(end, mirror.base + mirror.bytes);
    if (from >= to) return;
    const auto first = static_cast<std::size_t>(from / BlockBytes - mirror.base / BlockBytes);
    const auto last = static_cast<std::size_t>((to - 1) / BlockBytes - mirror.base / BlockBytes);
    for (auto block = first; block <= last; ++block) {
        mirror.unsynced[block] = 1;
        mirror.written[block] = 1;
    }
    mirror.anyUnsynced = true;
    mirror.anyWritten = true;
}

std::vector<VkBufferCopy> blockCopies(const ResidentMirror& mirror, const std::vector<std::uint8_t>& selected) {
    std::vector<VkBufferCopy> copies;
    for (std::size_t block = 0; block < selected.size(); ++block) {
        if (selected[block] == 0) continue;
        const auto [from, to] = BlockRange(mirror, block);
        const VkDeviceSize offset = from - mirror.base;
        if (!copies.empty() && copies.back().srcOffset + copies.back().size == offset) copies.back().size += to - from;
        else copies.push_back({offset, offset, to - from});
    }
    return copies;
}

std::uint64_t copiedBytes(const std::vector<VkBufferCopy>& copies) {
    std::uint64_t total = 0;
    for (const auto& copy : copies) total += copy.size;
    return total;
}

void reportMirrors() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    static auto last = std::chrono::steady_clock::now();
    static ResidentMirrorCounters previous{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) return;
    last = now;
    const auto totals = MirrorTotals();
    const auto take = [](const std::atomic<std::uint64_t>& value, std::uint64_t& last) {
        const auto now = value.load(std::memory_order_relaxed);
        const auto delta = now - last;
        last = now;
        return static_cast<unsigned long long>(delta);
    };
    static std::uint64_t deferred = 0, denied = 0, deniedUses = 0, verified = 0, verifiedBytes = 0, verifyDiffering = 0;
    AgcDriver::ProfilePrint_nid_no_patch("[mirrors] reuse gate (after %u batches, churn limit %u): %llu deferred, %llu denied for churn, %llu uses refused while denied; verify%s: %llu checked (%llu MiB), %llu blocks differing\n", MirrorAfterBatches(), ChurnLimit(), take(Stats().deferred, deferred), take(Stats().denied, denied), take(Stats().deniedUses, deniedUses), VerifyMirrors() ? "" : " (off)", take(Stats().verified, verified), take(Stats().verifiedBytes, verifiedBytes) >> 20u, take(Stats().verifyDiffering, verifyDiffering));
    AgcDriver::ProfilePrint_nid_no_patch("[mirrors] resident read mirrors%s: %llu live (%.0f MiB), last 10 s: %llu made, %llu refused, %llu uses, %llu batch refreshes (%llu blocks, %.1f MiB copied in prologues, collect %.1f ms), %llu mid-batch syncs (%.1f MiB), %llu blocks invalidated by GPU writes\n", ResidentMirrorsEnabled() ? "" : " (off)", static_cast<unsigned long long>(totals.live), static_cast<double>(totals.liveBytes) / 1048576.0, static_cast<unsigned long long>(totals.made - previous.made), static_cast<unsigned long long>(totals.refused - previous.refused), static_cast<unsigned long long>(totals.uses - previous.uses), static_cast<unsigned long long>(totals.refreshes - previous.refreshes), static_cast<unsigned long long>(totals.refreshedBlocks - previous.refreshedBlocks), static_cast<double>(totals.refreshedBytes - previous.refreshedBytes) / 1048576.0, totals.collectMs - previous.collectMs, static_cast<unsigned long long>(totals.midBatchSyncs - previous.midBatchSyncs), static_cast<double>(totals.midBatchBytes - previous.midBatchBytes) / 1048576.0, static_cast<unsigned long long>(totals.invalidatedBlocks - previous.invalidatedBlocks));
    previous = totals;
}

}

bool ResidentMirrorsEnabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_RESIDENT_MIRRORS");
        if (text == nullptr || std::string_view(text) == "1") return BudgetBytes() != 0;
        if (std::string_view(text) == "0") return false;
        throw std::runtime_error("APS5_RESIDENT_MIRRORS must be 0 or 1");
    }();
    return enabled;
}

bool BufferMirrorsEnabled() {
    static const bool enabled = std::getenv("APS5_RESIDENT_BUFFER_MIRRORS") != nullptr && BudgetBytes() != 0;
    return enabled;
}

ShaderRecompiler::BdaPlacement::Mode PlacementSetting() {
    static const auto mode = ShaderRecompiler::BdaPlacement::ParseMode(std::getenv("APS5_BDA_PLACEMENT"));
    return mode;
}

namespace {

std::shared_ptr<ResidentMirror> mirrorOf(const Context& context, const HostImport& import, bool writable) {
    const bool enabled = writable ? PlacementSetting() != ShaderRecompiler::BdaPlacement::Mode::Host : ResidentMirrorsEnabled() || BufferMirrorsEnabled();
    if (!enabled || BudgetBytes() == 0 || import.bytes == 0 || import.unwatched || import.dmaBuf || !context.bufferDeviceAddress) return {};
    if (!GuestMemory::Watched(import.base, static_cast<std::size_t>(import.bytes))) return {};
    auto& registry = Mirrors();
    const auto* recorder = Recorder::Current();
    const auto batch = recorder != nullptr ? recorder->Submissions() + 1 : 0;
    std::lock_guard lock(registry.mutex);
    auto& seen = registry.seen[import.base];
    if (batch != 0 && batch < seen.deniedUntil) {
        Stats().deniedUses.fetch_add(1, std::memory_order_relaxed);
        if (const auto found = registry.byBase.find(import.base); found != registry.byBase.end() && found->second.use_count() == 1) {
            found->second->retired = true;
            registry.bytes -= Footprint(*found->second);
            registry.byBase.erase(found);
        }
        return {};
    }
    if (batch != 0 && seen.lastBatch != batch) {
        seen.lastBatch = batch;
        if (seen.batches < MirrorAfterBatches()) ++seen.batches;
    }
    if (const auto found = registry.byBase.find(import.base); found != registry.byBase.end()) {
        auto& existing = found->second;
        if (existing->device == context.device && existing->source == import.buffer && existing->bytes == import.bytes) {
            if (existing->placedBy != nullptr && existing->placedBy != recorder) return {};
            if (!writable) return existing;
            if (existing->usedBy != nullptr && existing->usedBy != recorder) return {};
            if (existing->pristine == nullptr) {
                if (registry.bytes + existing->bytes > BudgetBytes()) {
                    Stats().refused.fetch_add(1, std::memory_order_relaxed);
                    return {};
                }
                try {
                    existing->pristine = std::make_shared<Buffer>(context, static_cast<std::size_t>(existing->bytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                } catch (const std::exception&) {
                    Stats().refused.fetch_add(1, std::memory_order_relaxed);
                    return {};
                }
                existing->pristineAddress = existing->pristine->DeviceAddress();
                registry.bytes += existing->bytes;
                markBlocks(*existing, existing->base, existing->base + existing->bytes);
            }
            existing->placedBy = recorder;
            return existing;
        }
        registry.bytes -= Footprint(*existing);
        existing->retired = true;
        registry.byBase.erase(found);
    }
    if (batch != 0 && seen.batches < MirrorAfterBatches()) {
        Stats().deferred.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const auto footprint = writable ? import.bytes * 2u : import.bytes;
    if (registry.bytes + footprint > BudgetBytes()) {
        for (auto it = registry.byBase.begin(); it != registry.byBase.end() && registry.bytes + footprint > BudgetBytes();) {
            if (it->second.use_count() == 1) {
                it->second->retired = true;
                registry.bytes -= Footprint(*it->second);
                it = registry.byBase.erase(it);
            } else {
                ++it;
            }
        }
        if (registry.bytes + footprint > BudgetBytes()) {
            Stats().refused.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
    }
    auto mirror = std::make_shared<ResidentMirror>();
    try {
        mirror->buffer = std::make_shared<Buffer>(context, static_cast<std::size_t>(import.bytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (writable) mirror->pristine = std::make_shared<Buffer>(context, static_cast<std::size_t>(import.bytes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (const std::exception&) {
        Stats().refused.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    mirror->device = context.device;
    mirror->base = import.base;
    mirror->bytes = import.bytes;
    mirror->source = import.buffer;
    mirror->address = mirror->buffer->DeviceAddress();
    if (writable) {
        mirror->pristineAddress = mirror->pristine->DeviceAddress();
        mirror->placedBy = recorder;
    }
    const auto blocks = BlockCount(import.base, import.bytes);
    mirror->generations.assign(blocks, 0);
    mirror->unsynced.assign(blocks, 0);
    mirror->written.assign(blocks, 0);
    registry.byBase.emplace(import.base, mirror);
    registry.bytes += footprint;
    registry.any.store(true, std::memory_order_release);
    Stats().made.fetch_add(1, std::memory_order_relaxed);
    return mirror;
}

}

std::shared_ptr<ResidentMirror> MirrorOfImport(const Context& context, const HostImport& import) {
    return mirrorOf(context, import, false);
}

std::shared_ptr<ResidentMirror> PlacementOfImport(const Context& context, const HostImport& import) {
    return mirrorOf(context, import, true);
}

VkDeviceAddress MirrorPristineAddress(const ResidentMirror& mirror) {
    Require(mirror.pristine != nullptr, "a BDA placement mirror has no pristine copy");
    return mirror.pristineAddress;
}

void RecordPlacementCopyBack(const Context& context, VkCommandBuffer commands, VkDeviceAddress record, VkDeviceAddress table, bool verify) {
    const auto pipeline = CopyBackPipeline(context);
    using ShaderRecompiler::BdaPlacement::Pass;
    context.Resolved(&DeviceFunctions::cmdBindPipeline, "vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    ShaderRecompiler::BdaPlacement::PushConstants push{record, table, static_cast<std::uint32_t>(Pass::CopyBack), 0};
    context.Resolved(&DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    context.Resolved(&DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, ShaderRecompiler::BdaPlacement::DispatchX, ShaderRecompiler::BdaPlacement::DispatchY, 1);
    if (!verify) return;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    push.pass = static_cast<std::uint32_t>(Pass::Verify);
    context.Resolved(&DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    context.Resolved(&DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, ShaderRecompiler::BdaPlacement::DispatchX, ShaderRecompiler::BdaPlacement::DispatchY, 1);
}

VkDeviceAddress MirrorDeviceAddress(const ResidentMirror& mirror) {
    return mirror.address;
}

VkBuffer MirrorBuffer(const ResidentMirror& mirror) {
    return mirror.buffer->Handle();
}

void NoteMirroredWrite(std::uint64_t begin, std::uint64_t end) {
    auto& registry = Mirrors();
    if (!registry.any.load(std::memory_order_acquire) || end <= begin) return;
    std::lock_guard lock(registry.mutex);
    auto it = registry.byBase.upper_bound(begin);
    if (it != registry.byBase.begin()) --it;
    for (; it != registry.byBase.end() && it->first < end; ++it) markBlocks(*it->second, begin, end);
    for (const auto& mirror : registry.used) {
        if (registry.byBase.find(mirror->base) == registry.byBase.end() || registry.byBase.find(mirror->base)->second != mirror) markBlocks(*mirror, begin, end);
    }
}

void NotePointerWrite(const Recorder& recorder, std::uint64_t begin, std::uint64_t end) {
    if (end <= begin) return;
    NoteMirroredWrite(begin, end);
    auto& registry = Mirrors();
    std::lock_guard lock(registry.mutex);
    registry.pointerWrites.emplace_back(&recorder, begin, end);
}

void UseMirrors(Recorder& recorder, std::span<const std::shared_ptr<ResidentMirror>> mirrors) {
    if (mirrors.empty()) return;
    auto& registry = Mirrors();
    std::vector<ResidentMirror*> unsynced;
    std::vector<ResidentMirror*> fresh;
    for (const auto& mirror : mirrors) {
        if (mirror->usedBatch == 0) fresh.push_back(mirror.get());
    }
    for (auto* mirror : fresh) {
        const auto pending = recorder.PendingWritesOver(mirror->base, static_cast<std::size_t>(mirror->bytes));
        const bool whole = recorder.PendingLabelIn(mirror->base, static_cast<std::size_t>(mirror->bytes)) || recorder.QueuedStoreOverlaps(mirror->base, static_cast<std::size_t>(mirror->bytes));
        std::lock_guard lock(registry.mutex);
        for (const auto& write : pending) markBlocks(*mirror, write.begin, write.end);
        for (const auto& [writer, begin, end] : registry.pointerWrites) {
            if (writer == &recorder) markBlocks(*mirror, begin, end);
        }
        if (whole) markBlocks(*mirror, mirror->base, mirror->base + mirror->bytes);
    }
    {
        std::lock_guard lock(registry.mutex);
        const auto batch = recorder.Submissions() + 1;
        for (const auto& mirror : mirrors) {
            if (mirror->usedBy != &recorder || mirror->usedBatch != batch) {
                mirror->usedBy = &recorder;
                mirror->usedBatch = batch;
                registry.used.push_back(mirror);
            }
            if (mirror->anyUnsynced && !mirror->retired) unsynced.push_back(mirror.get());
        }
    }
    Stats().uses.fetch_add(1, std::memory_order_relaxed);
    if (unsynced.empty()) return;
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(recorder.GetContext(), commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    std::uint64_t bytes = 0;
    {
        std::lock_guard lock(registry.mutex);
        for (auto* mirror : unsynced) {
            const auto copies = blockCopies(*mirror, mirror->unsynced);
            if (!copies.empty()) recorder.GetContext().Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, mirror->source, mirror->buffer->Handle(), static_cast<std::uint32_t>(copies.size()), copies.data());
            if (!copies.empty() && mirror->pristine != nullptr) recorder.GetContext().Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, mirror->source, mirror->pristine->Handle(), static_cast<std::uint32_t>(copies.size()), copies.data());
            bytes += copiedBytes(copies);
            std::fill(mirror->unsynced.begin(), mirror->unsynced.end(), std::uint8_t{0});
            mirror->anyUnsynced = false;
        }
    }
    RecordMemoryBarrier(recorder.GetContext(), commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    recorder.MarkCovered(0);
    Stats().midBatchSyncs.fetch_add(1, std::memory_order_relaxed);
    Stats().midBatchBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void RefreshMirrorsForDirectUse(Recorder& recorder, std::span<const std::shared_ptr<ResidentMirror>> mirrors) {
    if (mirrors.empty()) return;
    recorder.Submit();
    recorder.Sync();
    static_cast<void>(recorder.Commands());
    UseMirrors(recorder, mirrors);
    recorder.Submit();
    recorder.Sync();
}

void SubmitMirrors(Recorder& recorder) {
    auto& registry = Mirrors();
    if (!registry.any.load(std::memory_order_acquire)) {
        std::lock_guard lock(registry.mutex);
        registry.pointerWrites.erase(std::remove_if(registry.pointerWrites.begin(), registry.pointerWrites.end(), [&](const auto& write) { return std::get<0>(write) == &recorder; }), registry.pointerWrites.end());
        return;
    }
    const auto batch = recorder.Submissions() + 1;
    std::vector<std::shared_ptr<ResidentMirror>> used;
    {
        std::lock_guard lock(registry.mutex);
        for (auto& mirror : registry.used) {
            if (mirror->usedBy == &recorder && mirror->usedBatch == batch) used.push_back(mirror);
        }
        registry.used.erase(std::remove_if(registry.used.begin(), registry.used.end(), [&](const std::shared_ptr<ResidentMirror>& mirror) { return mirror->usedBy == &recorder; }), registry.used.end());
        registry.pointerWrites.erase(std::remove_if(registry.pointerWrites.begin(), registry.pointerWrites.end(), [&](const auto& write) { return std::get<0>(write) == &recorder; }), registry.pointerWrites.end());
    }
    std::vector<std::uint8_t> changed;
    VkCommandBuffer prologue = VK_NULL_HANDLE;
    std::vector<const ResidentMirror*> verified;
    std::uint64_t verifyBytes = 0;
    for (const auto& mirror : used) {
        if (mirror->retired) continue;
        const auto started = std::chrono::steady_clock::now();
        const auto generation = GuestMemory::CollectWrites(mirror->base, static_cast<std::size_t>(mirror->bytes));
        changed.assign(mirror->generations.size(), GuestMemory::BlockWritten);
        if (generation != 0) GuestMemory::ChangedBlocks(mirror->base, static_cast<std::size_t>(mirror->bytes), mirror->generations, changed);
        Stats().collectNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
        const auto copies = blockCopies(*mirror, changed);
        if (!copies.empty()) {
            if (prologue == VK_NULL_HANDLE) prologue = recorder.PrologueCommands();
            recorder.GetContext().Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(prologue, mirror->source, mirror->buffer->Handle(), static_cast<std::uint32_t>(copies.size()), copies.data());
            if (mirror->pristine != nullptr) recorder.GetContext().Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(prologue, mirror->source, mirror->pristine->Handle(), static_cast<std::uint32_t>(copies.size()), copies.data());
            Stats().refreshedBlocks.fetch_add(static_cast<std::uint64_t>(std::count(changed.begin(), changed.end(), GuestMemory::BlockWritten)), std::memory_order_relaxed);
            Stats().refreshedBytes.fetch_add(copiedBytes(copies), std::memory_order_relaxed);
        }
        std::fill(mirror->generations.begin(), mirror->generations.end(), generation);
        recorder.Keep(mirror);
        Stats().refreshes.fetch_add(1, std::memory_order_relaxed);
        if (ChurnLimit() != 0) {
            mirror->churn = copiedBytes(copies) * 2 > mirror->bytes ? mirror->churn + 1 : 0;
            if (mirror->churn >= ChurnLimit()) {
                mirror->churn = 0;
                std::lock_guard lock(registry.mutex);
                auto& seen = registry.seen[mirror->base];
                seen.deniedUntil = batch + DeniedBatches;
                seen.batches = 0;
                Stats().denied.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (VerifyMirrors() && (verified.empty() || verifyBytes + mirror->bytes <= VerifyBytesPerBatch)) {
            verifyBytes += mirror->bytes;
            verified.push_back(mirror.get());
        }
    }
    if (!verified.empty()) {
        if (prologue == VK_NULL_HANDLE) prologue = recorder.PrologueCommands();
        RecordMemoryBarrier(recorder.GetContext(), prologue, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (const auto* mirror : verified) {
            const auto bytes = mirror->bytes;
            auto scratch = std::make_shared<Buffer>(recorder.GetContext(), static_cast<std::size_t>(bytes * 2), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            const VkBufferCopy held{0, 0, bytes};
            const VkBufferCopy source{0, bytes, bytes};
            recorder.GetContext().Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(prologue, mirror->buffer->Handle(), scratch->Handle(), 1, &held);
            recorder.GetContext().Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(prologue, mirror->source, scratch->Handle(), 1, &source);
            recorder.Keep(scratch);
            const auto base = mirror->base;
            recorder.OnCompleteWithoutGuestAccess([scratch, bytes, base] {
                const auto contents = scratch->Bytes();
                std::uint64_t differing = 0;
                for (std::uint64_t block = 0; block < BlockCount(base, bytes); ++block) {
                    const auto first = base / BlockBytes * BlockBytes + block * BlockBytes;
                    const auto from = std::max(first, base) - base;
                    const auto to = std::min(first + BlockBytes, base + bytes) - base;
                    if (std::memcmp(contents.data() + from, contents.data() + bytes + from, static_cast<std::size_t>(to - from)) != 0) ++differing;
                }
                Stats().verified.fetch_add(1, std::memory_order_relaxed);
                Stats().verifiedBytes.fetch_add(bytes, std::memory_order_relaxed);
                Stats().verifyDiffering.fetch_add(differing, std::memory_order_relaxed);
                static std::atomic<std::uint32_t> reports{0};
                if (differing != 0 && reports.fetch_add(1, std::memory_order_relaxed) < 20) std::fprintf(stderr, "[mirrors] verify: the resident mirror of 0x%llx+0x%llx differs from its import in %llu blocks at the start of its batch\n", static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(differing));
            });
        }
        RecordMemoryBarrier(recorder.GetContext(), prologue, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    std::lock_guard lock(registry.mutex);
    const auto settle = [&](ResidentMirror& mirror) {
        if (!mirror.anyWritten) return;
        for (std::size_t block = 0; block < mirror.written.size(); ++block) {
            if (mirror.written[block] == 0) continue;
            mirror.generations[block] = 0;
            mirror.written[block] = 0;
            mirror.unsynced[block] = 0;
            Stats().invalidatedBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        mirror.anyWritten = false;
        mirror.anyUnsynced = false;
    };
    for (const auto& [base, mirror] : registry.byBase) settle(*mirror);
    for (const auto& mirror : used) settle(*mirror);
    reportMirrors();
}

void ForgetMirrors(VkDevice device, std::uint64_t begin, std::uint64_t end) {
    auto& registry = Mirrors();
    if (!registry.any.load(std::memory_order_acquire)) return;
    std::lock_guard lock(registry.mutex);
    for (auto it = registry.byBase.begin(); it != registry.byBase.end();) {
        const auto& mirror = *it->second;
        if (mirror.device == device && mirror.base < end && begin < mirror.base + mirror.bytes) {
            it->second->retired = true;
            registry.bytes -= Footprint(mirror);
            it = registry.byBase.erase(it);
        } else {
            ++it;
        }
    }
}

void ClearMirrors(VkDevice device) {
    ForgetMirrors(device, 0, ~std::uint64_t{0});
    DestroyCopyBackPipeline(device);
    auto& registry = Mirrors();
    std::lock_guard lock(registry.mutex);
    registry.used.clear();
}

ResidentMirrorCounters MirrorTotals() {
    auto& registry = Mirrors();
    ResidentMirrorCounters totals;
    {
        std::lock_guard lock(registry.mutex);
        totals.live = registry.byBase.size();
        totals.liveBytes = registry.bytes;
    }
    auto& stats = Stats();
    totals.made = stats.made.load(std::memory_order_relaxed);
    totals.refused = stats.refused.load(std::memory_order_relaxed);
    totals.uses = stats.uses.load(std::memory_order_relaxed);
    totals.refreshes = stats.refreshes.load(std::memory_order_relaxed);
    totals.refreshedBlocks = stats.refreshedBlocks.load(std::memory_order_relaxed);
    totals.refreshedBytes = stats.refreshedBytes.load(std::memory_order_relaxed);
    totals.midBatchSyncs = stats.midBatchSyncs.load(std::memory_order_relaxed);
    totals.midBatchBytes = stats.midBatchBytes.load(std::memory_order_relaxed);
    totals.invalidatedBlocks = stats.invalidatedBlocks.load(std::memory_order_relaxed);
    totals.deferred = stats.deferred.load(std::memory_order_relaxed);
    totals.denied = stats.denied.load(std::memory_order_relaxed);
    totals.verified = stats.verified.load(std::memory_order_relaxed);
    totals.verifyDiffering = stats.verifyDiffering.load(std::memory_order_relaxed);
    totals.collectMs = static_cast<double>(stats.collectNs.load(std::memory_order_relaxed)) / 1e6;
    return totals;
}

}

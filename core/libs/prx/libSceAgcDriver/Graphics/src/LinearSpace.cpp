#include "prx/libSceAgcDriver/Graphics/include/LinearSpace.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace AgcDriver::Graphics {

namespace {

constexpr VkDeviceSize ChunkBytes = VkDeviceSize{64} << 20u;
constexpr std::uint64_t MaxWindowBytes = std::uint64_t{1} << 40u;
constexpr std::uint64_t MinWindowBytes = std::uint64_t{16} << 30u;
constexpr std::uint32_t PagesPerChunk = static_cast<std::uint32_t>(ChunkBytes / LinearPageBytes);
constexpr std::uint32_t CodesPerWord = 16;

struct Chunk {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<std::uint32_t> free;
};

struct Page {
    std::uint32_t chunk = 0;
    std::uint32_t slot = 0;
    std::uint32_t refs = 0;
};

struct Space {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress base = 0;
    std::uint64_t bytes = 0;
    std::uint32_t memoryType = 0;
    bool failed = false;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    std::vector<Chunk> chunks;
    std::unordered_map<std::uint64_t, Page> pages;
    std::uint64_t usedPages = 0;
    std::atomic<std::uint64_t> binds{0}, unbinds{0}, refused{0}, tables{0}, codedPages{0};
};

Space& Spaces() {
    static Space space;
    return space;
}

bool bindPages(const Context& context, Space& space, std::span<const VkSparseMemoryBind> binds) {
    if (binds.empty()) return true;
    const bool ownQueue = context.sparseQueue != VK_NULL_HANDLE && context.sparseQueueMutex != nullptr;
    if (!ownQueue && !GuestMemory::GpuMutex().HeldByThisThread()) return false;
    const VkSparseBufferMemoryBindInfo bufferBinds{space.buffer, static_cast<std::uint32_t>(binds.size()), binds.data()};
    VkBindSparseInfo bind{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    bind.bufferBindCount = 1;
    bind.pBufferBinds = &bufferBinds;
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &fenceInfo, nullptr, &fence), "vkCreateFence linear space");
    VkResult bound = VK_SUCCESS;
    VkResult waited = VK_SUCCESS;
    {
        std::unique_lock<std::mutex> queueLock;
        if (ownQueue) queueLock = std::unique_lock(*context.sparseQueueMutex);
        bound = context.Function<PFN_vkQueueBindSparse>("vkQueueBindSparse")(ownQueue ? context.sparseQueue : context.queue, 1, &bind, fence);
        if (bound == VK_SUCCESS) waited = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
    Check(bound, "vkQueueBindSparse linear space");
    Check(waited, "vkWaitForFences linear space");
    return true;
}

void destroySpace(Space& space) {
    if (space.device == VK_NULL_HANDLE) return;
    if (space.buffer != VK_NULL_HANDLE) space.destroyBuffer(space.device, space.buffer, nullptr);
    for (auto& chunk : space.chunks) {
        DeviceMemoryPlacements::Untrack(chunk.memory);
        space.freeMemory(space.device, chunk.memory, nullptr);
    }
    space.chunks.clear();
    space.pages.clear();
    space.usedPages = 0;
    space.buffer = VK_NULL_HANDLE;
    space.base = 0;
    space.bytes = 0;
    space.device = VK_NULL_HANDLE;
    space.failed = false;
}

bool createSpace(const Context& context, Space& space) {
    if (space.device == context.device) return !space.failed && space.buffer != VK_NULL_HANDLE;
    destroySpace(space);
    space.device = context.device;
    space.freeMemory = context.Function<PFN_vkFreeMemory>("vkFreeMemory");
    space.destroyBuffer = context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer");
    auto bytes = std::min<std::uint64_t>(context.limits.sparseAddressSpaceSize, MaxWindowBytes) / LinearPageBytes * LinearPageBytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const std::uint32_t families[] = {context.queueFamily, context.sparseFamily};
    const bool concurrent = context.sparseQueue != VK_NULL_HANDLE && context.sparseFamily != context.queueFamily;
    info.sharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    info.queueFamilyIndexCount = concurrent ? 2u : 0u;
    info.pQueueFamilyIndices = concurrent ? families : nullptr;
    const auto createBuffer = context.Function<PFN_vkCreateBuffer>("vkCreateBuffer");
    for (; bytes >= MinWindowBytes; bytes /= 2u) {
        info.size = bytes;
        if (createBuffer(context.device, &info, nullptr, &space.buffer) == VK_SUCCESS) break;
        space.buffer = VK_NULL_HANDLE;
    }
    if (space.buffer == VK_NULL_HANDLE) {
        space.failed = true;
        return false;
    }
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, space.buffer, &requirements);
    if (requirements.alignment == 0 || LinearPageBytes % requirements.alignment != 0) {
        destroySpace(space);
        space.device = context.device;
        space.failed = true;
        return false;
    }
    space.memoryType = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, space.buffer};
    space.base = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &addressInfo);
    space.bytes = bytes;
    if (space.base == 0) {
        destroySpace(space);
        space.device = context.device;
        space.failed = true;
        return false;
    }
    return true;
}

bool takeSlot(const Context& context, Space& space, std::uint64_t budget, Page& page) {
    for (std::uint32_t index = 0; index < space.chunks.size(); ++index) {
        auto& chunk = space.chunks[index];
        if (chunk.free.empty()) continue;
        page.chunk = index;
        page.slot = chunk.free.back();
        chunk.free.pop_back();
        return true;
    }
    if ((space.chunks.size() + 1) * ChunkBytes > budget) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = ChunkBytes;
    allocation.memoryTypeIndex = space.memoryType;
    Chunk chunk;
    if (context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &chunk.memory) != VK_SUCCESS) return false;
    DeviceMemoryPlacements::Track(chunk.memory, DeviceMemoryKind::LinearSpace, context.HostPlaced(allocation.memoryTypeIndex), ChunkBytes);
    for (std::uint32_t slot = PagesPerChunk; slot > 1; --slot) chunk.free.push_back(slot - 1);
    page.chunk = static_cast<std::uint32_t>(space.chunks.size());
    page.slot = 0;
    space.chunks.push_back(std::move(chunk));
    return true;
}

void reclaimFreePages(const Context& context, Space& space) {
    std::vector<VkSparseMemoryBind> unbinds;
    std::vector<std::uint64_t> released;
    for (const auto& [index, page] : space.pages) {
        if (page.refs != 0) continue;
        unbinds.push_back({index * LinearPageBytes, LinearPageBytes, VK_NULL_HANDLE, 0, 0});
        released.push_back(index);
    }
    if (unbinds.empty() || !bindPages(context, space, unbinds)) return;
    for (const auto index : released) {
        const auto page = space.pages.at(index);
        space.chunks[page.chunk].free.push_back(page.slot);
        space.pages.erase(index);
    }
    space.unbinds.fetch_add(unbinds.size(), std::memory_order_relaxed);
}

}

bool ParseLinearSpace(const char* text) {
    if (text == nullptr || std::strcmp(text, "1") == 0) return true;
    if (std::strcmp(text, "0") == 0) return false;
    throw std::runtime_error(std::string("APS5_LINEAR_ADDRESS_SPACE must be 0 or 1, not ") + text);
}

bool LinearSpaceEnabled(const Context& context) {
    static const bool requested = ParseLinearSpace(std::getenv("APS5_LINEAR_ADDRESS_SPACE"));
    return requested && context.sparseBinding && context.sparseResidencyBuffer && context.bufferDeviceAddress;
}

std::optional<LinearSpaceWindow> LinearSpace(const Context& context) {
    if (!LinearSpaceEnabled(context)) return std::nullopt;
    auto& space = Spaces();
    std::lock_guard lock(space.mutex);
    if (!createSpace(context, space)) return std::nullopt;
    return LinearSpaceWindow{space.buffer, space.base, space.bytes};
}

bool AcquireLinearRange(const Context& context, std::uint64_t begin, std::uint64_t end, std::uint64_t budget) {
    if (end <= begin || !LinearSpaceEnabled(context)) return false;
    auto& space = Spaces();
    std::lock_guard lock(space.mutex);
    if (!createSpace(context, space) || end > space.bytes) return false;
    const auto first = begin / LinearPageBytes;
    const auto last = (end - 1) / LinearPageBytes;
    std::vector<VkSparseMemoryBind> binds;
    std::vector<std::uint64_t> fresh;
    const auto undo = [&] {
        for (const auto index : fresh) {
            const auto page = space.pages.at(index);
            space.chunks[page.chunk].free.push_back(page.slot);
            space.pages.erase(index);
        }
    };
    for (auto index = first; index <= last; ++index) {
        if (space.pages.contains(index)) continue;
        Page page;
        if (!takeSlot(context, space, budget, page)) {
            reclaimFreePages(context, space);
            if (!takeSlot(context, space, budget, page)) {
                undo();
                space.refused.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        space.pages.emplace(index, page);
        fresh.push_back(index);
        binds.push_back({index * LinearPageBytes, LinearPageBytes, space.chunks[page.chunk].memory, static_cast<VkDeviceSize>(page.slot) * LinearPageBytes, 0});
    }
    if (!bindPages(context, space, binds)) {
        undo();
        space.refused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    space.binds.fetch_add(binds.size(), std::memory_order_relaxed);
    for (auto index = first; index <= last; ++index) {
        auto& page = space.pages.at(index);
        if (page.refs++ == 0) ++space.usedPages;
    }
    return true;
}

void ReleaseLinearRange(VkDevice device, std::uint64_t begin, std::uint64_t end) {
    if (end <= begin) return;
    auto& space = Spaces();
    std::lock_guard lock(space.mutex);
    if (space.device != device) return;
    for (auto index = begin / LinearPageBytes; index <= (end - 1) / LinearPageBytes; ++index) {
        const auto found = space.pages.find(index);
        if (found == space.pages.end() || found->second.refs == 0) continue;
        if (--found->second.refs == 0) --space.usedPages;
    }
}

void ClearLinearSpace(VkDevice device) {
    auto& space = Spaces();
    std::lock_guard lock(space.mutex);
    if (space.device == device) destroySpace(space);
}

LinearSpaceCounters LinearSpaceTotals() {
    auto& space = Spaces();
    std::lock_guard lock(space.mutex);
    return {space.pages.size(), space.usedPages, space.binds.load(std::memory_order_relaxed), space.unbinds.load(std::memory_order_relaxed), space.refused.load(std::memory_order_relaxed), space.tables.load(std::memory_order_relaxed), space.codedPages.load(std::memory_order_relaxed)};
}

void CountLinearTable(const LinearPageCodes& codes) {
    std::uint64_t pages = 0;
    for (const auto word : codes.words) {
        for (std::uint32_t shift = 0; shift < 32u; shift += 2u) pages += ((word >> shift) & 3u) != 0 ? 1u : 0u;
    }
    Spaces().tables.fetch_add(1, std::memory_order_relaxed);
    Spaces().codedPages.fetch_add(pages, std::memory_order_relaxed);
}

LinearPageCodes BuildLinearPageCodes(std::span<const ShaderRecompiler::BdaAbi::Range> ranges, VkDeviceAddress deviceBase) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    std::vector<const Abi::Range*> sorted;
    sorted.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (range.end > range.begin) sorted.push_back(&range);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Abi::Range* left, const Abi::Range* right) { return left->begin < right->begin; });
    struct Span {
        std::uint64_t first;
        std::uint64_t last;
        LinearPageCode code;
    };
    std::vector<Span> spans;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const auto& range = *sorted[i];
        if ((i + 1 < sorted.size() && sorted[i + 1]->begin == range.begin) || (i > 0 && sorted[i - 1]->begin == range.begin)) continue;
        if (deviceBase == 0 || range.deviceAddress != deviceBase + range.begin) continue;
        const bool read = (range.permissions & Abi::Read) != 0;
        const bool write = (range.permissions & Abi::Write) != 0;
        if (!read) continue;
        const auto code = write ? LinearPageCode::ReadWrite : LinearPageCode::Read;
        const auto selectedEnd = i + 1 < sorted.size() ? std::min(range.end, sorted[i + 1]->begin) : range.end;
        const auto firstPage = (range.begin + LinearPageBytes - 1) / LinearPageBytes;
        const auto endPage = selectedEnd / LinearPageBytes;
        if (endPage > firstPage) spans.push_back({firstPage, endPage, code});
    }
    LinearPageCodes codes;
    if (spans.empty()) return codes;
    const auto lowPage = spans.front().first / CodesPerWord * CodesPerWord;
    auto highPage = lowPage;
    for (const auto& span : spans) highPage = std::max(highPage, span.last);
    highPage = (highPage + CodesPerWord - 1) / CodesPerWord * CodesPerWord;
    codes.guestLow = lowPage * LinearPageBytes;
    codes.guestHigh = highPage * LinearPageBytes;
    codes.words.assign(static_cast<std::size_t>((highPage - lowPage) / CodesPerWord), 0u);
    for (const auto& span : spans) {
        for (auto page = span.first; page < span.last; ++page) {
            const auto relative = page - lowPage;
            codes.words[static_cast<std::size_t>(relative / CodesPerWord)] |= static_cast<std::uint32_t>(span.code) << ((relative % CodesPerWord) * 2u);
        }
    }
    return codes;
}

LinearPageCode LinearPageCodeAt(const LinearPageCodes& codes, std::uint64_t address) {
    if (address < codes.guestLow || address >= codes.guestHigh) return LinearPageCode::Fallback;
    const auto relative = (address - codes.guestLow) / LinearPageBytes;
    const auto word = codes.words[static_cast<std::size_t>(relative / CodesPerWord)];
    const auto code = (word >> ((relative % CodesPerWord) * 2u)) & 3u;
    return code == 3u ? LinearPageCode::Fallback : static_cast<LinearPageCode>(code);
}

}

#include "prx/libSceAgcDriver/Execution/include/Coherence.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

namespace {
using namespace AgcDriver::Graphics;
namespace Memory = AgcDriver::GuestMemory;
namespace Coherence = AgcDriver::Coherence;
constexpr std::uint32_t Side = 256;
constexpr std::size_t Block = 65536;
constexpr std::size_t Bytes = 4 * Block;
constexpr std::size_t Page = 4096;

class Surface {
public:
    explicit Surface(bool native = false) : native(native) {
#ifdef _WIN32
        if (native) {
            data = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(Bytes, Block));
            Require(data != nullptr, "store over untracked: native reservation failed");
            GuestArena::GuestArenaCommit_nid_postfix(data, Bytes, PAGE_READWRITE, Bytes);
        } else
#endif
        {
            storage.resize(Bytes + Block);
            data = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(storage.data()) + Block - 1) & ~(Block - 1));
        }
        std::memset(data, 0x31, Bytes);
        GuestAllocations::Mutation().Add(data, Bytes, true, true, true);
    }
    ~Surface() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        if (native) {
            GuestArena::GuestArenaReset_nid_postfix(data, Bytes);
            GuestArena::GuestArenaRelease_nid_postfix(data, Bytes);
        }
#endif
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(data); }
    std::span<std::byte> Data() { return {data, Bytes}; }
private:
    bool native;
    std::vector<std::byte> storage;
    std::byte* data = nullptr;
};

GuestTextureResource Resource(const Surface& surface, bool linear = false) {
    GuestTextureResource resource{};
    resource.baseAddress = surface.Address();
    resource.width = Side;
    resource.height = Side;
    resource.mipCount = 1;
    resource.tileMode = linear ? TextureTileMode::kLinear : TextureTileMode::kR64KBX;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    const auto geometry = DescribeSurface(resource);
    Require(geometry.guestBytes == Bytes, "store over untracked: unexpected surface extent");
    if (!linear) Require(ThinBlockLayout(resource.tileMode, 4) == std::array<std::uint32_t, 3>{Block, Side / 2, Side / 2} && geometry.mips[0].blocksPerRow == 2, "store over untracked: tiled surface does not contain four independent64KiB blocks");
    return resource;
}

struct PendingCleanup {
    std::uint64_t address;
    ~PendingCleanup() { StorageTexture::DiscardPendingInside(address, Bytes); }
};

void Clear(const Context& context, Recorder& recorder, const std::shared_ptr<StorageTexture>& image, bool blue = false) {
    image->PrepareWrite();
    recorder.Keep(image);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue color = blue ? VkClearColorValue{{0.0f, 0.0f, 1.0f, 1.0f}} : VkClearColorValue{{1.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    image->MarkDirty(&recorder);
    recorder.Sync();
}

std::vector<std::byte> Pixels(const Context& context, Recorder& recorder, const std::shared_ptr<StorageTexture>& image) {
    auto readback = std::make_shared<Buffer>(context, Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    recorder.Keep(readback);
    const auto commands = recorder.Commands();
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {Side, Side, 1};
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, readback->Handle(), 1, &copy);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Sync();
    readback->Invalidate();
    const auto bytes = readback->Bytes();
    return {bytes.begin(), bytes.end()};
}

void RequireColor(std::span<const std::byte> bytes, bool blue = false) {
    Require(bytes.size() % 4 == 0, "store over untracked: invalid pixel extent");
    for (std::size_t i = 0; i < bytes.size(); i += 4) Require(bytes[i] == std::byte(blue ? 0u : 255u) && bytes[i + 1] == std::byte{0} && bytes[i + 2] == std::byte(blue ? 255u : 0u) && bytes[i + 3] == std::byte{255}, "store over untracked: actual GPU pixels changed");
}

void Pattern(std::span<std::byte> bytes) {
    for (std::size_t i = 0; i != bytes.size(); ++i) bytes[i] = std::byte(0x40 + i % 0x3b);
}

enum class Kind { ReadAfter, ReadBefore, Filled, Copied, NeighbourCopied, SecondStore };

void Refusal(const Context& context, Recorder& recorder, Kind kind) {
    Surface surface;
    PendingCleanup cleanup{surface.Address()};
    auto image = StorageTexture::Create(context, *context.detiler, Resource(surface), 0);
    Require(!Memory::Watched(surface.Address(), Bytes), "store over untracked: host control unexpectedly became watched");
    std::array<std::byte, Page> read{};
    Clear(context, recorder, image);
    if (kind == Kind::ReadBefore) {
        Memory::Read(surface.Address() + Block + Page, read);
        RequireColor(read);
        Require(StorageTexture::FindPending(surface.Address(), Block) == image, "store over untracked: partial read discarded another pending unit");
    }
    if (kind == Kind::SecondStore) {
        image->Flush();
        recorder.Sync();
        RequireColor(surface.Data());
        image->Refresh();
        Clear(context, recorder, image, true);
    } else if (kind == Kind::Filled) Clear(context, recorder, image, true);
    else if (kind == Kind::Copied || kind == Kind::NeighbourCopied) {
        Surface source;
        PendingCleanup sourceCleanup{source.Address()};
        auto from = StorageTexture::Create(context, *context.detiler, Resource(source), 0);
        Clear(context, recorder, from, true);
        image->PrepareWrite();
        const auto commands = recorder.Commands();
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {Side, Side, 1};
        if (kind == Kind::NeighbourCopied) {
            copy.extent = {Side / 2, Side / 2, 1};
            copy.dstOffset = {Side / 2, Side / 2, 0};
        }
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdCopyImage>("vkCmdCopyImage")(commands, from->Image(), VK_IMAGE_LAYOUT_GENERAL, image->Image(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        image->MarkDirty(&recorder);
        recorder.Sync();
    }
    const auto expected = Pixels(context, recorder, image);
    if (kind == Kind::NeighbourCopied) {
        for (std::uint32_t y = 0; y != Side; ++y) {
            const auto row = std::span(expected).subspan(y * Side * 4, Side * 4);
            RequireColor(row.first(Side * 2));
            RequireColor(row.subspan(Side * 2), y >= Side / 2);
        }
    } else RequireColor(expected, kind == Kind::Filled || kind == Kind::Copied || kind == Kind::SecondStore);
    Require(StorageTexture::FindPending(surface.Address(), Bytes) == image, "store over untracked: fixture did not retain pending output");
    Pattern(surface.Data().first(Page));
    if (kind == Kind::ReadAfter) {
        Memory::Read(surface.Address() + Block + Page, read);
        RequireColor(read);
        std::array<std::byte, Page> pattern;
        Pattern(pattern);
        Require(std::equal(pattern.begin(), pattern.end(), surface.Data().begin()), "store over untracked: unrelated partial publication lost CPU bytes");
        Require(StorageTexture::FindPending(surface.Address(), Block) == image, "store over untracked: unrelated partial publication lost pending survivor output");
        Require(Pixels(context, recorder, image) == expected, "store over untracked: partial publication damaged GPU output");
    }
    const auto cpu = std::vector<std::byte>(surface.Data().begin(), surface.Data().end());
    const auto refuses = [&](const auto& operation) {
        bool refused = false;
        try { operation(); }
        catch (const std::runtime_error& error) {
            if (std::string_view(error.what()).find("unknown provenance") == std::string_view::npos) throw;
            refused = true;
        }
        Require(refused, "store over untracked: ambiguous CPU intent did not refuse publication");
        Require(StorageTexture::FindPending(surface.Address(), Bytes) == image, "store over untracked: refusal lost the pending registry entry");
        Require(std::equal(cpu.begin(), cpu.end(), surface.Data().begin()), "store over untracked: refusal partially changed guest bytes");
        Require(Pixels(context, recorder, image) == expected, "store over untracked: refusal damaged actual GPU output");
    };
    if (kind == Kind::ReadBefore) refuses([&] { Memory::Read(surface.Address() + 0xe000, read); });
    refuses([&] { image->Refresh(); });
    refuses([&] { StorageTexture::FlushPending(surface.Address(), Bytes); });
    refuses([&] { image->Flush(); });
}

void PublishedControl(const Context& context, Recorder& recorder) {
    Surface surface;
    PendingCleanup cleanup{surface.Address()};
    auto image = StorageTexture::Create(context, *context.detiler, Resource(surface, true), 0);
    Clear(context, recorder, image);
    image->Flush();
    recorder.Sync();
    RequireColor(surface.Data());
    Require(StorageTexture::FindPending(surface.Address(), Bytes) == nullptr, "store over untracked: publication left pending output");
    Pattern(surface.Data().first(Page));
    const auto expected = std::vector<std::byte>(surface.Data().begin(), surface.Data().begin() + Page);
    image->Refresh();
    recorder.Sync();
    Require(std::equal(expected.begin(), expected.end(), surface.Data().begin()), "store over untracked: ordered CPU bytes changed");
    RequireColor(surface.Data().subspan(Page));
    const auto pixels = Pixels(context, recorder, image);
    Require(std::equal(expected.begin(), expected.end(), pixels.begin()), "store over untracked: ordered refresh missed CPU bytes");
    RequireColor(std::span(pixels).subspan(Page));
    std::vector<std::byte> fill(Page, std::byte{0x72});
    Memory::StoreOwnBytes(surface.Address(), fill);
    image->Refresh();
    recorder.Sync();
    Require(std::equal(fill.begin(), fill.end(), surface.Data().begin()), "store over untracked: ordered driver fill was lost");
    std::vector<std::byte> copied(Page);
    Pattern(copied);
    Memory::StoreOwnBytes(surface.Address() + Page, copied);
    image->Refresh();
    recorder.Sync();
    const auto final = Pixels(context, recorder, image);
    Require(std::equal(fill.begin(), fill.end(), final.begin()) && std::equal(copied.begin(), copied.end(), final.begin() + Page), "store over untracked: ordered fill/copy refresh missed driver bytes");
    RequireColor(std::span(final).subspan(2 * Page));
}

void ProductionCopy(const Context& context, Recorder& recorder, bool neighbour) {
    Surface surface;
    Surface source;
    PendingCleanup cleanup{surface.Address()};
    PendingCleanup sourceCleanup{source.Address()};
    auto copiedResource = Resource(source);
    auto destinationResource = Resource(surface);
    if (neighbour) {
        copiedResource.width = copiedResource.height = Side / 2;
        destinationResource = copiedResource;
        destinationResource.baseAddress = surface.Address() + 3 * Block;
        Require(DescribeSurface(destinationResource).guestBytes == Block, "store over untracked: neighbor copy has the wrong extent");
    }
    auto from = StorageTexture::Create(context, *context.detiler, copiedResource, 0);
    auto to = StorageTexture::Create(context, *context.detiler, destinationResource, 0);
    from->SetCached(true);
    to->SetCached(true);
    const std::array<std::uint32_t, 4> blue{0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u};
    const char* refusal = nullptr;
    Require(from->FillClear(blue, StorageTexture::WholeImage, refusal), "store over untracked: production source FillClear was refused");
    recorder.Sync();
    std::shared_ptr<StorageTexture> whole = to;
    if (neighbour) whole = StorageTexture::Create(context, *context.detiler, Resource(surface), 0);
    Clear(context, recorder, whole);
    Require(to->CopyFrom(*from, refusal), "store over untracked: production CopyFrom was refused");
    recorder.Sync();
    Require(StorageTexture::FindPending(destinationResource.baseAddress, neighbour ? Block : Bytes) == to, "store over untracked: production copy did not retain destination output");
    if (neighbour) Require(StorageTexture::FindPending(surface.Address(), Block) == whole, "store over untracked: neighboring copy discarded the first pending unit");
    const auto before = Pixels(context, recorder, whole);
    if (!neighbour) RequireColor(before, true);
    else RequireColor(before);
    Pattern(surface.Data().first(Page));
    const auto guest = std::vector<std::byte>(surface.Data().begin(), surface.Data().end());
    bool refused = false;
    try { whole->Flush(); }
    catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("unknown provenance") == std::string_view::npos) throw;
        refused = true;
    }
    Require(refused && StorageTexture::FindPending(surface.Address(), neighbour ? Block : Bytes) == whole, "store over untracked: production copy refusal lost pending survivor output");
    Require(std::equal(guest.begin(), guest.end(), surface.Data().begin()), "store over untracked: production copy refusal changed guest bytes");
    Require(Pixels(context, recorder, whole) == before, "store over untracked: production copy refusal damaged surviving GPU pixels");
    if (neighbour) {
        Require(StorageTexture::FindPending(destinationResource.baseAddress, Block) == to, "store over untracked: refusal lost neighboring pending copy");
        auto readback = std::make_shared<Buffer>(context, Block, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        recorder.Keep(readback);
        const auto commands = recorder.Commands();
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {Side / 2, Side / 2, 1};
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, to->Image(), VK_IMAGE_LAYOUT_GENERAL, readback->Handle(), 1, &copy);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Sync();
        readback->Invalidate();
        RequireColor(readback->Bytes(), true);
    }
}

#ifdef _WIN32
void LeftWatch(const Context& context, Recorder& recorder, unsigned kind) {
    Surface surface(true);
    PendingCleanup cleanup{surface.Address()};
    Require(Memory::Watched(surface.Address(), Bytes), "store over untracked: native control is not watched");
    auto image = StorageTexture::Create(context, *context.detiler, Resource(surface, true), 0);
    Clear(context, recorder, image);
    image->Flush();
    recorder.Sync();
    RequireColor(surface.Data());
    if (kind == 1) Pattern(surface.Data().first(Page));
    if (kind == 2) Require(Memory::ImportWatched(surface.Address(), Bytes, [] { return true; }, false), "store over untracked: explicit import transition refused");
    else Memory::Unwatch(surface.Address(), Bytes);
    Require(!Memory::Watched(surface.Address(), Bytes), "store over untracked: explicit transition retained tracking");
    if (kind != 1) Pattern(surface.Data().first(Page));
    const auto expected = std::vector<std::byte>(surface.Data().begin(), surface.Data().end());
    image->Refresh();
    recorder.Sync();
    Require(Pixels(context, recorder, image) == expected, "store over untracked: published output was lost across the unwatch transition");
    Clear(context, recorder, image, true);
    Require(StorageTexture::FindPending(surface.Address(), Bytes) == image, "store over untracked: unwatched native output is not pending");
    Pattern(surface.Data().subspan(Page, Page));
    const auto held = std::vector<std::byte>(surface.Data().begin(), surface.Data().end());
    bool refused = false;
    try { image->Flush(); }
    catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("unknown provenance") == std::string_view::npos) throw;
        refused = true;
    }
    Require(refused && StorageTexture::FindPending(surface.Address(), Bytes) == image, "store over untracked: ambiguous unwatched native output was not retained");
    Require(std::equal(held.begin(), held.end(), surface.Data().begin()), "store over untracked: refused native publication changed guest bytes");
    RequireColor(Pixels(context, recorder, image), true);
}
#endif
}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard gpu(Memory::GpuMutex());
        auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        const auto importContext = context;
        context.hostImportAlignment = 0;
        TextureDetiler detiler(context);
        context.detiler = &detiler;
        auto& recorder = device->RecorderFor(0);
        for (const auto kind : {Kind::ReadAfter, Kind::ReadBefore, Kind::Filled, Kind::Copied, Kind::NeighbourCopied, Kind::SecondStore}) Refusal(context, recorder, kind);
        PublishedControl(context, recorder);
        auto copyContext = importContext;
        copyContext.detiler = &detiler;
        Require(copyContext.hostImportAlignment != 0, "store over untracked: production copy requires host import support");
        ProductionCopy(copyContext, recorder, false);
        ProductionCopy(copyContext, recorder, true);
#ifdef _WIN32
        for (unsigned kind = 0; kind != 3; ++kind) LeftWatch(context, recorder, kind);
#endif
        recorder.Sync();
        std::puts("store over untracked: refusals retained pending GPU output; ordered publication controls passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

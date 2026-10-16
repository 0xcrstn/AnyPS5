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
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver {
struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};
}

#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <charconv>
#include <system_error>
#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace {
bool UnknownProvenanceRefusal(const std::runtime_error& error, std::uint64_t address, std::size_t bytes) {
    constexpr std::string_view prefix = "AGC guest memory: cannot preserve GPU results across a guest store of unknown provenance or overwrite extent at address=";
    const std::string_view message(error.what());
    if (!message.starts_with(prefix)) return false;
    const auto end = message.data() + message.size();
    std::uint64_t refusedAddress = 0;
    const auto parsedAddress = std::from_chars(message.data() + prefix.size(), end, refusedAddress);
    if (parsedAddress.ec != std::errc{} || !std::string_view(parsedAddress.ptr, static_cast<std::size_t>(end - parsedAddress.ptr)).starts_with(" bytes=")) return false;
    std::uint64_t refusedBytes = 0;
    const auto parsedBytes = std::from_chars(parsedAddress.ptr + 7, end, refusedBytes);
    return parsedBytes.ec == std::errc{} && std::string_view(parsedBytes.ptr, static_cast<std::size_t>(end - parsedBytes.ptr)).starts_with(" since=") && refusedAddress >= address && refusedBytes != 0 && refusedBytes <= bytes && refusedAddress - address <= bytes - refusedBytes;
}
}

namespace DeterministicStorage {
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
            if (!UnknownProvenanceRefusal(error, surface.Address(), Bytes)) throw;
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
        if (!UnknownProvenanceRefusal(error, surface.Address(), Bytes)) throw;
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
        if (!UnknownProvenanceRefusal(error, surface.Address(), Bytes)) throw;
        refused = true;
    }
    Require(refused && StorageTexture::FindPending(surface.Address(), Bytes) == image, "store over untracked: ambiguous unwatched native output was not retained");
    Require(std::equal(held.begin(), held.end(), surface.Data().begin()), "store over untracked: refused native publication changed guest bytes");
    RequireColor(Pixels(context, recorder, image), true);
}
#endif

int Run() {
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

}

namespace GuestStorage {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
constexpr std::uint32_t Side = 256;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t TileLinear = 0;
std::uint32_t surfaceTile = TileR64KBX;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t Page = 0x1000;
constexpr std::size_t Block = 0x10000;
constexpr std::size_t SurfaceBytes = 4 * Block;
constexpr std::uint32_t Untouched = 0xdeadbeefu;
constexpr std::uint32_t Stored = 0x12345678u;
constexpr std::uint32_t Filled = 0x9abcdef0u;
constexpr std::uint32_t StoredAgain = 0x0badf00du;

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    0x7e040300, 0x7e060280, 0x7e0202ff, Stored, 0xf0200108, 0x00020102, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> StoreAgainCode{
    0x7e040300, 0x7e060280, 0x7e0202ff, StoredAgain, 0xf0200108, 0x00020102, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 9> FillCode{
    0xd7460004, 0x04010c08, 0x7e000204, 0x7e020205, 0x7e040206, 0x7e060207, 0xe01c2000, 0x80000004, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 9> ReadCode{
    0x7e020280, 0x7e04020c, 0xf0001108, 0x00010300, 0xbf8c3f70, 0x34080082, 0xe0701000, 0x80000304, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint32_t* texels, std::uint32_t side = Side) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((side - 1u) & 3u) << 30u),
        ((side - 1u) >> 2u) | ((side - 1u) << 14u),
        0xfacu | (surfaceTile << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

void Dispatch(std::span<const std::uint32_t> code, std::uint32_t threads, std::uint32_t resource, std::span<const std::uint32_t> userData, std::uint32_t groups) {
    Require(!AgcDriver::GuestMemory::GpuMutex().HeldByThisThread(), "store over untracked: a dispatch completion wait cannot hold the GPU mutex");
    std::vector<std::uint32_t> words;
    const auto registers = [&](const std::uint32_t first, const std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{threads, 1, 1};
    registers(0x207, shape);
    const auto address = AddressOf(code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> layout{resource};
    registers(0x213, layout);
    registers(0x240, userData);
    words.insert(words.end(), {0xc0031500u, groups, 1, 1, 0x41u});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
}

void Store(const std::uint32_t* texels, std::uint32_t side = Side, std::span<const std::uint32_t> code = StoreCode) {
    std::array<std::uint32_t, 16> userData{};
    const auto texture = TextureDescriptor(texels, side);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    Dispatch(code, 8, static_cast<std::uint32_t>(userData.size()) << 1u, userData, 1);
}

void Fill(const std::uint32_t* texels) {
    const auto address = AddressOf(texels);
    const std::array<std::uint32_t, 8> userData{
        static_cast<std::uint32_t>(address),
        static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u),
        static_cast<std::uint32_t>(SurfaceBytes / 16u),
        0x4bfacu,
        Filled,
        Filled,
        Filled,
        Filled,
    };
    Dispatch(FillCode, 64, (static_cast<std::uint32_t>(userData.size()) << 1u) | 0x80u, userData, static_cast<std::uint32_t>(SurfaceBytes / 1024u));
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::size_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), static_cast<std::uint32_t>(bytes), 0x31016facu};
}

void ReadImage(const std::uint32_t* texels, std::uint32_t* output) {
    std::array<std::uint32_t, 13> userData{};
    const auto buffer = BufferDescriptor(output, SurfaceBytes);
    const auto texture = TextureDescriptor(texels);
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    Dispatch(ReadCode, 32, static_cast<std::uint32_t>(userData.size()) << 1u, userData, 1);
}

class Surface {
public:
    explicit Surface(bool watched = false, std::size_t lead = 0) : watched(watched), regionBytes(lead == 0 ? SurfaceBytes : SurfaceBytes + Block) {
        if (watched) {
#ifdef _WIN32
            region = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(regionBytes, Block));
            if (region != nullptr) GuestArena::GuestArenaCommit_nid_postfix(region, regionBytes, PAGE_READWRITE, regionBytes);
#else
            void* mapped = mmap(nullptr, regionBytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapped != MAP_FAILED) {
                const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
                const auto aligned = (begin + Block - 1) & ~std::uintptr_t{Block - 1};
                if (aligned != begin) munmap(mapped, aligned - begin);
                if (aligned != begin + Block) munmap(reinterpret_cast<void*>(aligned + regionBytes), begin + Block - aligned);
                region = reinterpret_cast<std::uint8_t*>(aligned);
                GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(region, regionBytes);
            }
#endif
            if (region != nullptr) texels = reinterpret_cast<std::uint32_t*>(region + lead);
        } else {
            storage.resize((SurfaceBytes + Block) / 4u);
            texels = reinterpret_cast<std::uint32_t*>((reinterpret_cast<std::uintptr_t>(storage.data()) + Block - 1) & ~std::uintptr_t{Block - 1});
        }
        Require(texels != nullptr, "store over untracked: cannot allocate the guest surface");
        std::fill(texels, texels + SurfaceBytes / 4u, Untouched);
        allocation = region != nullptr ? static_cast<void*>(region) : static_cast<void*>(texels);
        GuestAllocations::Mutation().Add(allocation, region != nullptr ? regionBytes : SurfaceBytes, true, true, true);
    }

    ~Surface() {
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            StorageTexture::DiscardPendingInside(Address(), SurfaceBytes);
        }
        GuestAllocations::Mutation().Remove(allocation);
        if (!watched) return;
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(region, regionBytes);
        GuestArena::GuestArenaRelease_nid_postfix(region, regionBytes);
#else
        munmap(region, regionBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(region, regionBytes);
#endif
    }

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    std::uint32_t* Texels() const { return texels; }
    std::uint8_t* Bytes() const { return reinterpret_cast<std::uint8_t*>(texels); }
    std::uint64_t Address() const { return AddressOf(texels); }

private:
    bool watched;
    std::size_t regionBytes;
    std::vector<std::uint32_t> storage;
    std::uint8_t* region = nullptr;
    void* allocation = nullptr;
    std::uint32_t* texels = nullptr;
};

std::uint8_t Pattern(std::size_t offset) {
    return static_cast<std::uint8_t>(0x40u + offset % 0x3bu);
}

std::size_t LostBytes(const std::uint8_t* bytes) {
    std::size_t lost = 0;
    for (std::size_t offset = 0; offset < Page; ++offset) {
        if (static_cast<const volatile std::uint8_t*>(bytes)[offset] != Pattern(offset)) ++lost;
    }
    return lost;
}

bool Holds(const std::uint32_t* texels, std::uint32_t value) {
    const auto* words = static_cast<const volatile std::uint32_t*>(texels);
    for (std::size_t index = 0; index < SurfaceBytes / 4u; ++index) {
        if (words[index] == value) return true;
    }
    return false;
}

template <typename THold>
bool HoldPending(THold& hold, const Surface& surface, std::uint32_t result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        hold.lock();
        if (StorageTexture::FindPending(surface.Address(), SurfaceBytes) != nullptr) return true;
        hold.unlock();
        if (Holds(surface.Texels(), result)) return false;
        Require(std::chrono::steady_clock::now() < deadline, "store over untracked: the result neither became pending nor reached guest memory");
        std::this_thread::yield();
    }
}

template <typename THold>
void Settle(THold& hold, const Surface& surface, std::uint32_t result) {
    if (!HoldPending(hold, surface, result)) return;
    StorageTexture::FlushAllPending("store over untracked fixture");
    hold.unlock();
}

enum class Kind { ReadAfter, ReadBefore, Filled, Copied, LeftWatch, WrittenThenLeftWatch, ImportLeftWatch, NeighbourCopied, NeighbourLeftWatch, UntrackedRepeat };

// One 64 KiB block of the surface: the image of its last block, and the source copied into it.
constexpr std::uint32_t BlockSide = 128;
constexpr std::size_t LastBlock = SurfaceBytes - Block;

struct Case {
    const char* name;
    Kind kind;
    std::size_t access;
};

enum class Outcome { Kept, Lost, Missed };
enum class ExpectedRefusal { UnknownProvenance, PartialCoverageAtUnwatch };

// The surface is outside the write watch from the start. Its first results are stored, which
// leaves their bytes saved for the next store to compare with; the CPU writes another block, then
// the next results land and are stored: the CPU's block was written after the first store, so it
// keeps the CPU's bytes as the write watch would keep them.

template <typename TOperation>
bool CheckPublicationRefusal(const char* name, const Surface& surface, const std::uint8_t* written, TOperation&& operation, ExpectedRefusal expected = ExpectedRefusal::UnknownProvenance) {
    const auto address = surface.Address();
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{address, address + SurfaceBytes}}};
    const auto image = StorageTexture::FindLive(address, SurfaceBytes);
    if (image == nullptr || !StorageTexture::AnyPendingOverlaps(range)) {
        operation();
        return false;
    }
    auto* recorder = AgcDriver::Graphics::Recorder::Current();
    Require(recorder != nullptr, "store over untracked: refusal verification has no recorder");
    const auto pixels = DeterministicStorage::Pixels(recorder->GetContext(), *recorder, image);
    Require(LostBytes(written) == 0, "store over untracked: CPU bytes changed before publication");
    const auto cpu = std::vector<std::uint8_t>(surface.Bytes(), surface.Bytes() + SurfaceBytes);
    if (expected == ExpectedRefusal::PartialCoverageAtUnwatch) Require(AgcDriver::GuestMemory::Watched(address, SurfaceBytes), "store over untracked: partial-coverage unwatch fixture was not watched");
    try {
        operation();
        return false;
    } catch (const std::runtime_error& error) {
        if (expected == ExpectedRefusal::UnknownProvenance) {
            if (!UnknownProvenanceRefusal(error, address, SurfaceBytes)) throw;
        } else {
            const auto prefix = "AGC guest memory: cannot discard pending GPU results without full exact newer-store coverage at address=" + std::to_string(address) + " bytes=" + std::to_string(Block) + " since=";
            if (!std::string_view(error.what()).starts_with(prefix)) throw;
            Require(AgcDriver::GuestMemory::Watched(address, SurfaceBytes), "store over untracked: refused partial-coverage unwatch removed write tracking");
        }
        Require(StorageTexture::FindLive(address, SurfaceBytes) == image && StorageTexture::AnyPendingOverlaps(range), "store over untracked: refusal lost pending GPU output");
        Require(std::equal(cpu.begin(), cpu.end(), surface.Bytes()), "store over untracked: refusal changed CPU bytes");
        Require(DeterministicStorage::Pixels(recorder->GetContext(), *recorder, image) == pixels, "store over untracked: refusal changed actual GPU pixels");
        Require(std::equal(cpu.begin(), cpu.end(), surface.Bytes()) && StorageTexture::AnyPendingOverlaps(range), "store over untracked: GPU verification disturbed retained output");
        std::fprintf(stderr, "[store over untracked] %s: explicit %s refusal retained CPU bytes and pending GPU pixels%s\n", name, expected == ExpectedRefusal::UnknownProvenance ? "unknown-provenance" : "partial-coverage unwatch", expected == ExpectedRefusal::UnknownProvenance ? "" : "; write tracking retained");
        return true;
    }
}

Outcome RunSecondStore(const char* name) {
    Surface surface(true);
    struct WaitForWork {
        ~WaitForWork() { AgcDriverWaitIdle_nid_postfix(); }
    } waitForWork;
    const auto address = surface.Address();
    if (!AgcDriver::GuestMemory::Watched(address, SurfaceBytes)) {
        std::fprintf(stderr, "[store over untracked] %s: not run, the surface is not write-watched\n", name);
        return Outcome::Kept;
    }
    AgcDriver::GuestMemory::Unwatch(address, SurfaceBytes);
    Require(!AgcDriver::GuestMemory::Watched(address, SurfaceBytes), "store over untracked: the surface is still write-watched");
    std::unique_lock hold(AgcDriver::GuestMemory::GpuMutex(), std::defer_lock);
    const auto heldLive = [&](std::uint32_t result) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (true) {
            hold.lock();
            const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{address, address + SurfaceBytes}}};
            if (StorageTexture::FindLive(address, SurfaceBytes) != nullptr && StorageTexture::AnyPendingOverlaps(range)) return true;
            hold.unlock();
            if (Holds(surface.Texels(), result)) return false;
            Require(std::chrono::steady_clock::now() < deadline, "store over untracked: the result neither became pending nor reached guest memory");
            std::this_thread::yield();
        }
    };
    Store(surface.Texels());
    if (!heldLive(Stored)) return Outcome::Missed;
    StorageTexture::FlushAllPending("store over untracked fixture");
    hold.unlock();
    {
        std::vector<std::byte> first(SurfaceBytes);
        AgcDriver::GuestMemory::Read(address, first);
    }
    Require(static_cast<const volatile std::uint32_t*>(surface.Texels())[0] == Stored, "store over untracked: the first result was not stored");
    hold.lock();
    const auto image = StorageTexture::FindLive(address, SurfaceBytes);
    const bool saved = image != nullptr && image->GuestSnapshotValid();
    hold.unlock();
    Require(image != nullptr, "store over untracked: the surface has no storage image");
    if (!saved) {
        std::fprintf(stderr, "[store over untracked] %s: the store left no saved guest bytes\n", name);
        return Outcome::Lost;
    }
    auto* written = surface.Bytes() + 2 * Block;
    for (std::size_t offset = 0; offset < Page; ++offset) static_cast<volatile std::uint8_t*>(written)[offset] = Pattern(offset);
    Store(surface.Texels(), Side, StoreAgainCode);
    if (!heldLive(StoredAgain)) return Outcome::Missed;
    if (CheckPublicationRefusal(name, surface, written, [&] { StorageTexture::FlushAllPending("store over untracked fixture"); })) return Outcome::Kept;
    hold.unlock();
    std::vector<std::byte> whole(SurfaceBytes);
    AgcDriver::GuestMemory::Read(address, whole);
    Require(static_cast<const volatile std::uint32_t*>(surface.Texels())[0] == StoredAgain, "store over untracked: the second result did not reach a block the CPU left alone");
    const auto lost = LostBytes(written);
    if (lost == 0) return Outcome::Kept;
    std::fprintf(stderr, "[store over untracked] %s: of 0x%zx bytes the CPU wrote between the two stores, %zu were lost\n", name, Page, lost);
    return Outcome::Lost;
}

Outcome Run(const Case& test) {
    std::optional<Surface> output;
    std::optional<Surface> source;
    if (test.kind == Kind::Copied || test.kind == Kind::NeighbourCopied) source.emplace();
    const bool neighbour = test.kind == Kind::NeighbourLeftWatch;
    const bool leaves = test.kind == Kind::LeftWatch || test.kind == Kind::WrittenThenLeftWatch || test.kind == Kind::ImportLeftWatch || neighbour;
    surfaceTile = neighbour ? TileLinear : TileR64KBX;
    Surface surface(leaves, neighbour ? Page : 0);
    struct WaitForWork {
        ~WaitForWork() { AgcDriverWaitIdle_nid_postfix(); }
    } waitForWork;
    auto* bytes = surface.Bytes();
    const auto address = surface.Address();
    std::unique_lock hold(AgcDriver::GuestMemory::GpuMutex(), std::defer_lock);
    if (test.kind == Kind::Copied) {
        Store(source->Texels());
        Settle(hold, *source, Stored);
        Fill(source->Texels());
        Settle(hold, *source, Filled);
    }
    Store(surface.Texels());
    if (test.kind == Kind::UntrackedRepeat) {
        Settle(hold, surface, Stored);
        output.emplace();
        ReadImage(surface.Texels(), output->Texels());
        hold.lock();
        const auto image = StorageTexture::FindLive(surface.Address(), SurfaceBytes);
        Require(image != nullptr, "store over untracked: the sampled surface has no live storage image");
        image->MarkDirty();
        hold.unlock();
    }
    if (test.kind == Kind::Filled || leaves) {
        Settle(hold, surface, Stored);
        if (leaves) {
            std::vector<std::byte> settled(SurfaceBytes);
            AgcDriver::GuestMemory::Read(address, settled);
        }
        Fill(surface.Texels());
        if (!HoldPending(hold, surface, Filled)) return Outcome::Missed;
    } else if (test.kind == Kind::Copied) {
        Settle(hold, surface, Stored);
        hold.lock();
        const auto from = StorageTexture::FindLive(source->Address(), SurfaceBytes);
        const auto to = StorageTexture::FindLive(address, SurfaceBytes);
        Require(from != nullptr && to != nullptr, "store over untracked: the surfaces have no storage images to copy between");
        const char* refusal = nullptr;
        if (!to->CopyFrom(*from, refusal)) {
            Require(refusal != nullptr && std::string_view(refusal) == "not imported", "store over untracked: the image copy was refused");
            std::fprintf(stderr, "[store over untracked] %s: not run, the surface has no host import\n", test.name);
            return Outcome::Kept;
        }
    } else if (test.kind == Kind::NeighbourCopied) {
        // The surface's image has results pending in its first block when a copy replaces the
        // image of its last block: the copy supersedes the saved bytes there only.
        const auto settledLive = [&](std::uint64_t at, std::size_t size) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (true) {
                hold.lock();
                StorageTexture::FlushAllPending("store over untracked fixture");
                const bool live = StorageTexture::FindLive(at, size) != nullptr;
                hold.unlock();
                if (live) return;
                Require(std::chrono::steady_clock::now() < deadline, "store over untracked: the block's storage image was never created");
                std::this_thread::yield();
            }
        };
        Store(surface.Texels() + LastBlock / 4u, BlockSide);
        settledLive(address + LastBlock, Block);
        Store(source->Texels(), BlockSide);
        settledLive(source->Address(), Block);
        std::fill(surface.Texels(), surface.Texels() + SurfaceBytes / 4u, Untouched);
        Store(surface.Texels());
        // Pending results are the surface image's alone once it exists.
        hold.lock();
        const auto live = StorageTexture::FindLive(address, SurfaceBytes);
        Require(live != nullptr, "store over untracked: the completed surface dispatch has no live storage image");
        if (StorageTexture::FindPending(address, SurfaceBytes) != live) {
            hold.unlock();
            return Outcome::Missed;
        }
        const auto from = StorageTexture::FindLive(source->Address(), Block);
        const auto to = StorageTexture::FindLive(address + LastBlock, Block);
        Require(from != nullptr && to != nullptr, "store over untracked: the blocks have no storage images to copy between");
        const char* refusal = nullptr;
        if (!to->CopyFrom(*from, refusal)) {
            Require(refusal != nullptr && std::string_view(refusal) == "not imported", "store over untracked: the image copy was refused");
            StorageTexture::FlushAllPending("store over untracked fixture");
            std::fprintf(stderr, "[store over untracked] %s: not run, the block has no host import\n", test.name);
            return Outcome::Kept;
        }
    } else if (!HoldPending(hold, surface, Stored)) {
        return Outcome::Missed;
    }
    const auto writePattern = [&] {
        for (std::size_t offset = 0; offset < Page; ++offset) static_cast<volatile std::uint8_t*>(bytes)[offset] = Pattern(offset);
    };
    if (test.kind == Kind::WrittenThenLeftWatch) writePattern();
    if (leaves) {
        if (!AgcDriver::GuestMemory::Watched(address, SurfaceBytes)) {
            StorageTexture::FlushAllPending("store over untracked fixture");
            std::fprintf(stderr, "[store over untracked] %s: not run, the surface is not write-watched\n", test.name);
            return Outcome::Kept;
        }
        const auto image = StorageTexture::FindLive(address, SurfaceBytes);
        Require(image != nullptr, "store over untracked: the surface has no storage image");
        Require(!image->GuestSnapshotValid(), "store over untracked: the pending result had saved guest bytes while its memory was write-watched");
        const auto leaveWatch = [&] {
            if (test.kind == Kind::ImportLeftWatch) Require(AgcDriver::GuestMemory::ImportWatched(address, SurfaceBytes, [] { return true; }, false), "store over untracked: the import that leaves the write watch failed");
            else if (neighbour) Require(AgcDriver::GuestMemory::ImportWatched(address - Page, Page, [] { return true; }, false), "store over untracked: the import of the page before the surface failed");
            else AgcDriver::GuestMemory::Unwatch(address, SurfaceBytes);
        };
        if (test.kind == Kind::WrittenThenLeftWatch) {
            if (CheckPublicationRefusal(test.name, surface, bytes, leaveWatch, ExpectedRefusal::PartialCoverageAtUnwatch)) return Outcome::Kept;
        } else leaveWatch();
        if (!neighbour) Require(!AgcDriver::GuestMemory::Watched(address, SurfaceBytes), "store over untracked: the surface is still write-watched");
    }
    std::array<std::byte, Page> read{};
    if (test.kind == Kind::ReadBefore) AgcDriver::GuestMemory::Read(address + Block + Page, read);
    if (test.kind != Kind::WrittenThenLeftWatch) writePattern();
    if (CheckPublicationRefusal(test.name, surface, bytes, [&] { AgcDriver::GuestMemory::Read(address + test.access, read); })) return Outcome::Kept;
    const auto lostAtAccess = LostBytes(bytes);
    if (CheckPublicationRefusal(test.name, surface, bytes, [&] { StorageTexture::FlushAllPending("store over untracked fixture"); })) return Outcome::Kept;
    std::vector<std::byte> whole(SurfaceBytes);
    AgcDriver::GuestMemory::Read(address, whole);
    const auto lostAtEnd = LostBytes(bytes);
    if (test.kind == Kind::Filled || test.kind == Kind::Copied || leaves) Require(static_cast<const volatile std::uint32_t*>(surface.Texels())[2 * Block / 4u] == Filled, "store over untracked: the result did not reach a block the CPU left alone");
    if (lostAtAccess == 0 && lostAtEnd == 0) return Outcome::Kept;
    std::fprintf(stderr, "[store over untracked] %s: of 0x%zx bytes the CPU wrote while the result was pending, %zu were lost after the access at +0x%zx and %zu after every result was stored\n", test.name, Page, lostAtAccess, test.access, lostAtEnd);
    return Outcome::Lost;
}


int Run() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        struct ShutdownGuard {
            ~ShutdownGuard() { try { AgcDriverShutdown_nid_postfix(); } catch (...) {} }
        } shutdown;
        {
            Surface probe;
            Require(AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(TextureDescriptor(probe.Texels()))).guestBytes == SurfaceBytes, "store over untracked: unexpected surface size");
        }
        constexpr std::array<Case, 10> cases{{
            {"another block read after the CPU write", Kind::ReadAfter, Block + Page},
            {"another block read before the CPU write", Kind::ReadBefore, 0xe000},
            {"a fill of the surface", Kind::Filled, 0xe000},
            {"an image copy into the surface", Kind::Copied, 0xe000},
            {"a surface that left the write watch", Kind::LeftWatch, 0xe000},
            {"a surface written before it left the write watch", Kind::WrittenThenLeftWatch, 0xe000},
            {"a surface whose import left the write watch", Kind::ImportLeftWatch, 0xe000},
            {"an image copy into another block of the surface", Kind::NeighbourCopied, 0xe000},
            {"a surface whose first block left the write watch with the page before it", Kind::NeighbourLeftWatch, 0xe000},
            {"untracked storage results written again after an image read", Kind::UntrackedRepeat, 0xe000},
        }};
        bool kept = true;
        for (const auto& test : cases) {
            auto outcome = Outcome::Missed;
            for (std::uint32_t attempt = 0; attempt < 256u && outcome == Outcome::Missed; ++attempt) outcome = Run(test);
            Require(outcome != Outcome::Missed, "store over untracked: the result was never held pending");
            kept = outcome == Outcome::Kept && kept;
        }
        {
            constexpr auto name = "a second store over a surface outside the write watch";
            auto outcome = Outcome::Missed;
            for (std::uint32_t attempt = 0; attempt < 256u && outcome == Outcome::Missed; ++attempt) outcome = RunSecondStore(name);
            Require(outcome != Outcome::Missed, "store over untracked: the result was never held pending");
            kept = outcome == Outcome::Kept && kept;
        }
        Require(kept, "a pending storage result was stored over bytes the CPU wrote to untracked memory");
        AgcDriverShutdown_nid_postfix();
        std::puts("store over untracked tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

}

int main() {
    const auto deterministic = DeterministicStorage::Run();
    if (deterministic != 0) return deterministic;
    return GuestStorage::Run();
}

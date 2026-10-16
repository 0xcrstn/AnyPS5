#pragma once

#include "prx/libSceAgcDriver/Execution/include/Coherence.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ProducerMappingIncarnationTests {

using namespace AgcDriver::Graphics;
namespace Memory = AgcDriver::GuestMemory;
namespace Coherence = AgcDriver::Coherence;

#ifdef _WIN32
constexpr std::size_t Bytes = 65536;

class NativeMode {
public:
    NativeMode() : faults(Coherence::FaultsMode()), backend(Coherence::CurrentProtectionBackend()), materializer(Coherence::CurrentMaterializer()), resolveHostWrites(GuestArena::GuestArenaResolveHostWritesEnabled_nid_postfix()) {
        Coherence::SetFaultsModeForTests(Coherence::Mode::New);
        Coherence::SetProtectionBackend({&GuestArena::GuestArenaTryProtectAccess_nid_postfix, &GuestArena::GuestArenaReleaseAccess_nid_postfix, &GuestArena::GuestArenaOpenAccessForRead_nid_postfix, &GuestArena::GuestArenaTryProtectMapping_nid_postfix});
        Coherence::SetMaterializer([](Coherence::Interval range) { StorageTexture::MaterializeForNativeAccess(range.begin, static_cast<std::size_t>(range.end - range.begin)); });
        GuestArena::GuestArenaSetResolveHostWrites_nid_postfix(true);
    }

    ~NativeMode() {
        GuestArena::GuestArenaSetResolveHostWrites_nid_postfix(resolveHostWrites);
        Coherence::SetFaultsModeForTests(faults);
        Coherence::SetProtectionBackend(backend);
        Coherence::SetMaterializer(materializer);
    }
    NativeMode(const NativeMode&) = delete;
    NativeMode& operator=(const NativeMode&) = delete;
private:
    Coherence::Mode faults;
    Coherence::ProtectionBackend backend;
    Coherence::Materializer materializer;
    bool resolveHostWrites;
};

class Mapping {
public:
    Mapping() {
        try {
            base = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(3 * Bytes, Bytes));
            Require(base != nullptr, "producer incarnation: guest reservation failed");
            x = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Bytes, nullptr);
            y = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, Bytes, nullptr);
            Require(x != nullptr && y != nullptr, "producer incarnation: shared section allocation failed");
            GuestArena::GuestArenaMap_nid_postfix(base, Bytes, x, 0, PAGE_READWRITE);
            GuestArena::GuestArenaMap_nid_postfix(base + Bytes, Bytes, x, 0, PAGE_READWRITE);
            GuestArena::GuestArenaMap_nid_postfix(base + 2 * Bytes, Bytes, y, 0, PAGE_READWRITE);
            for (; registered != 3; ++registered) GuestAllocations::Mutation().Add(base + registered * Bytes, Bytes, true, true, true);
            std::vector<std::byte> initial(Bytes, std::byte{0x31});
            Memory::StoreOwnBytes(Address(), initial);
            std::fill(initial.begin(), initial.end(), std::byte{0x59});
            Memory::StoreOwnBytes(Other(), initial);
            for (std::size_t i = 0; i != 3; ++i) Memory::CollectWritesUncached(Address() + i * Bytes, Bytes);
        } catch (...) {
            release();
            throw;
        }
    }

    ~Mapping() { release(); }

    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(base); }
    std::uint64_t Alias() const { return Address() + Bytes; }
    std::uint64_t Other() const { return Address() + 2 * Bytes; }
    void Replace(bool restore) {
        GuestArena::GuestArenaMap_nid_postfix(base, Bytes, y, 0, PAGE_READWRITE);
        if (restore) GuestArena::GuestArenaMap_nid_postfix(base, Bytes, x, 0, PAGE_READWRITE);
    }

private:
    void release() {
        for (std::size_t i = 0; i != registered; ++i) GuestAllocations::Mutation().Remove(base + i * Bytes);
        if (base != nullptr) {
            GuestArena::GuestArenaReset_nid_postfix(base, 3 * Bytes);
            GuestArena::GuestArenaRelease_nid_postfix(base, 3 * Bytes);
        }
        if (y != nullptr) CloseHandle(y);
        if (x != nullptr) CloseHandle(x);
    }

    std::byte* base = nullptr;
    HANDLE x = nullptr;
    HANDLE y = nullptr;
    std::size_t registered = 0;
};

GuestTextureResource Resource(std::uint64_t address) {
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = 128;
    resource.height = 128;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kLinear;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    Require(DescribeSurface(resource).guestBytes == Bytes, "producer incarnation: image extent changed");
    return resource;
}

std::vector<std::byte> Read(std::uint64_t address) {
    std::vector<std::byte> result(Bytes);
    Memory::CopyDriverMemory(result.data(), reinterpret_cast<const void*>(address), result.size());
    return result;
}

void RequireRed(std::span<const std::byte> pixels) {
    Require(pixels.size() == Bytes, "producer incarnation: readback has the wrong extent");
    for (std::size_t i = 0; i < pixels.size(); i += 4) Require(pixels[i] == std::byte{255} && pixels[i + 1] == std::byte{0} && pixels[i + 2] == std::byte{0} && pixels[i + 3] == std::byte{255}, "producer incarnation: actual red GPU pixels were lost");
}

void PaintAndRead(const Context& context, Recorder& recorder, const std::shared_ptr<StorageTexture>& image) {
    auto result = std::make_shared<Buffer>(context, Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    recorder.Keep(result);
    const auto commands = recorder.Commands();
    recorder.Keep(image);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue red{{1.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, &red, 1, &range);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {128, 128, 1};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, result->Handle(), 1, &copy);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    recorder.Sync();
    result->Invalidate();
    RequireRed(result->Bytes());
}

template <typename Operation>
bool RefusesMapping(const Operation& operation) {
    try { operation(); }
    catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("mapping incarnation") != std::string_view::npos) return true;
        throw;
    }
    return false;
}

void DccOnlyReplacement(const Context& context, Recorder& recorder) {
    for (const unsigned mode : {0u, 1u, 2u}) {
        Mapping texels;
        Mapping keys;
        auto resource = Resource(texels.Address());
        resource.tileMode = TextureTileMode::kR64KBX;
        resource.dccAddress = keys.Address();
        resource.dccAlphaOnMsb = true;
        resource.dccPipeAligned = true;
        Require(DescribeSurface(resource).guestBytes == Bytes, "producer incarnation: tiled DCC fixture extent changed");
        const auto keyBytes = DccKeyCount(resource, Bytes);
        Require(keyBytes == 4096, "producer incarnation: DCC fixture key extent changed");
        std::vector<std::byte> initialKeys(Bytes, std::byte{0x31});
        std::fill_n(initialKeys.begin(), keyBytes, std::byte{0});
        Memory::StoreOwnBytes(keys.Address(), initialKeys);
        Memory::CollectWritesUncached(keys.Address(), Bytes);
        Memory::CollectWritesUncached(keys.Alias(), Bytes);
        auto image = StorageTexture::Create(context, *context.detiler, resource, 0);
        Require(image->UploadedKeys() == DccKeys::Clear0000, "producer incarnation: DCC fixture did not upload a 0000-cleared surface");
        PaintAndRead(context, recorder, image);
        Require(Read(texels.Address()) == std::vector<std::byte>(Bytes, std::byte{0x31}) && Read(keys.Address()) == initialKeys, "producer incarnation: DCC GPU clear published before pending install");
        if (mode == 0) {
            image->MarkDirty(&recorder);
            image->WriteBack();
            recorder.Sync();
            RequireRed(Read(texels.Address()));
            RequireRed(Read(texels.Alias()));
            auto expectedKeys = initialKeys;
            std::fill_n(expectedKeys.begin(), keyBytes, std::byte{255});
            Require(Read(keys.Address()) == expectedKeys && Read(keys.Alias()) == expectedKeys, "producer incarnation: supported DCC first write did not publish only the actual key extent");
            Require(Read(keys.Other()) == std::vector<std::byte>(Bytes, std::byte{0x59}), "producer incarnation: supported DCC first write changed unrelated key backing");
            continue;
        }
        keys.Replace(mode == 2);
        const auto before = Read(keys.Address());
        const auto oldAlias = Read(keys.Alias());
        const auto other = Read(keys.Other());
        const auto oldTexels = Read(texels.Address());
        Require(before == (mode == 2 ? initialKeys : std::vector<std::byte>(Bytes, std::byte{0x59})), "producer incarnation: DCC-only replacement did not retain its backing bytes");
        Require(RefusesMapping([&] { image->MarkDirty(&recorder); }), "producer incarnation: late first write accepted replaced DCC metadata");
        static_cast<void>(RefusesMapping([&] { image->WriteBack(); }));
        static_cast<void>(RefusesMapping([&] { image->Flush(); }));
        static_cast<void>(RefusesMapping([&] { StorageTexture::FlushPending(texels.Address(), Bytes); }));
        recorder.Sync();
        const auto unchanged = [&] {
            return Read(keys.Address()) == before && Read(keys.Alias()) == oldAlias && Read(keys.Other()) == other && Read(texels.Address()) == oldTexels && Read(texels.Alias()) == oldTexels;
        };
        Require(unchanged(), "producer incarnation: cleanup published old pixels or keys after DCC-only replacement");
        image.reset();
        Require(unchanged(), "producer incarnation: destruction changed backing after DCC-only replacement");
    }
}

bool Run(const Context& base, Recorder& recorder) {
    NativeMode native;
    auto context = base;
    context.hostImportAlignment = 0;
    TextureDetiler detiler(context);
    context.detiler = &detiler;
    for (const unsigned mode : {0u, 1u, 2u, 3u}) {
        Mapping mapping;
        auto image = StorageTexture::Create(context, detiler, Resource(mapping.Address()), 0);
        PaintAndRead(context, recorder, image);
        Require(Read(mapping.Address()) == std::vector<std::byte>(Bytes, std::byte{0x31}), "producer incarnation: GPU clear unexpectedly published before pending install");
        if (mode == 0) {
            image->MarkDirty(&recorder);
            image->WriteBack();
            recorder.Sync();
            RequireRed(Read(mapping.Address()));
            RequireRed(Read(mapping.Alias()));
            Require(Read(mapping.Other()) == std::vector<std::byte>(Bytes, std::byte{0x59}), "producer incarnation: normal publication changed unrelated backing");
            continue;
        }
        if (mode == 3) image->MarkDirty(&recorder);
        mapping.Replace(mode == 2);
        const auto before = Read(mapping.Address());
        const auto oldAlias = Read(mapping.Alias());
        const auto other = Read(mapping.Other());
        Require(before == std::vector<std::byte>(Bytes, mode == 2 ? std::byte{0x31} : std::byte{0x59}), "producer incarnation: replacement did not retain its backing bytes");
        if (mode == 3) RequireRed(oldAlias);
        Require(RefusesMapping([&] { image->MarkDirty(&recorder); }), "producer incarnation: late pending install accepted a changed mapping incarnation");
        static_cast<void>(RefusesMapping([&] { image->WriteBack(); }));
        static_cast<void>(RefusesMapping([&] { image->Flush(); }));
        static_cast<void>(RefusesMapping([&] { StorageTexture::FlushPending(mapping.Address(), Bytes); }));
        recorder.Sync();
        Require(Read(mapping.Address()) == before && Read(mapping.Alias()) == oldAlias && Read(mapping.Other()) == other, "producer incarnation: cleanup published old GPU output into replacement backing");
        image.reset();
        Require(Read(mapping.Address()) == before && Read(mapping.Alias()) == oldAlias && Read(mapping.Other()) == other, "producer incarnation: producer destruction changed replacement backing");
    }
    DccOnlyReplacement(context, recorder);
    return true;
}
#else
inline bool Run(const Context&, Recorder&) { return false; }
#endif

}

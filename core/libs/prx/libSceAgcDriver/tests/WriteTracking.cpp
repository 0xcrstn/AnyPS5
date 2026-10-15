#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::GuestMemory;

constexpr std::size_t Block = 65536;
#ifdef _WIN32
constexpr wchar_t HostHoleMode[] = L"--host-hole";
#endif

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void* AllocateWatched(std::size_t bytes, bool crossLeaf = false) {
#ifdef _WIN32
    static_cast<void>(crossLeaf);
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    const std::size_t alignment = crossLeaf ? std::size_t{1} << 32 : Block;
    const auto prefix = crossLeaf ? Block : 0;
    void* raw = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = ((begin + prefix + alignment - 1) & ~static_cast<std::uintptr_t>(alignment - 1)) - prefix;
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(Watched(reinterpret_cast<std::uint64_t>(block), bytes), "the test block is not watched");
    return block;
}

void CheckSharedBlock() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto boundary = base + 2 * Block - 0xb800;
    const auto firstBegin = base;
    const auto secondEnd = base + 3 * Block;
    const auto sharedFirst = boundary - (base + Block);
    const auto sharedSecond = base + 2 * Block - boundary;
    std::memset(memory, 0x11, 3 * Block);

    const auto synced = CollectWrites(base, 3 * Block);
    Require(synced != 0, "the test block is not collected");

    Require(MarkWritten(firstBegin, static_cast<std::size_t>(boundary - firstBegin)) != 0, "a driver store into watched memory is not stamped");
    std::array<std::uint64_t, 2> generations{synced, synced};
    std::array<std::uint8_t, 2> changed{};
    Require(ChangedBlocks(base + Block, 2 * Block, generations, changed) && changed[0] == BlockWritten && changed[1] == BlockUnchanged, "the shared block does not read as written");
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), synced), "a store of the neighbour's bytes counts for the second surface's part of the shared block");
    Require(StoredOver(base + Block, static_cast<std::size_t>(sharedFirst), synced), "the first surface's own store is not seen over its part of the shared block");
    Require(!StoredOver(boundary, static_cast<std::size_t>(secondEnd - boundary), synced), "the second surface reads as stored over");

    const auto touched = MarkWritten(boundary + 0x100, 4);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), synced), "a store over the second surface's bytes is not seen");
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), touched), "a store older than the generation is seen");

    const auto whole = TrackerGeneration();
    MarkWritten(base + Block, Block);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), whole), "a store over the whole block is not seen");
    const auto afterWhole = TrackerGeneration();
    MarkWritten(base + Block, static_cast<std::size_t>(sharedFirst));
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), afterWhole), "a partial store after a whole-block store is taken for the whole block");

    const auto many = TrackerGeneration();
    for (int store = 0; store < 6; ++store) MarkWritten(base + Block + static_cast<std::uint64_t>(store) * 64, 4);
    Require(StoredOver(boundary, static_cast<std::size_t>(sharedSecond), many), "forgotten driver stores are taken as missing the range");

    const auto beforeCpu = CollectWrites(base, 3 * Block);
    static_cast<volatile std::uint8_t*>(memory)[Block + 8] = 0x22;
    CollectWritesUncached(base, 3 * Block);
    Require(!StoredOver(boundary, static_cast<std::size_t>(sharedSecond), beforeCpu), "a CPU store outside the shared range counts for it");

    std::array<std::uint8_t, 64> unwatched{};
    const auto outside = reinterpret_cast<std::uint64_t>(unwatched.data());
    if (!Watched(outside, unwatched.size())) Require(StoredOver(outside, unwatched.size(), TrackerGeneration()), "an unwatched range reads as not stored over");
}

void CheckOwnStore() {
    void* memory = AllocateWatched(2 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto second = base + Block;
    std::memset(memory, 0x11, 2 * Block);
    const auto synced = CollectWrites(base, 2 * Block);
    Require(synced != 0, "the own-store block is not collected");

    std::array<std::byte, 8192> bytes{};
    bytes.fill(std::byte{0x44});
    Write(second, bytes);
    const auto stored = TrackerGeneration();
    Require(!UnchangedSince(second, bytes.size(), synced), "the driver store is not stamped");
    CollectWritesUncached(base, 2 * Block);
    Require(UnchangedSince(second, Block, stored), "a walk after a driver store reports the store again as a newer write");
    Require(UnchangedSinceCollected(second, bytes.size(), synced), "the driver store's own page faults are stamped as a CPU write");

    static_cast<volatile std::uint8_t*>(memory)[Block + 3 * 4096 + 8] = 0x22;
    const std::array<std::byte, 64> label{};
    Write(second + 3 * 4096 + 64, label);
    Require(!UnchangedSinceCollected(second + 3 * 4096, 4096, stored), "a CPU write before a driver store in its page is not stamped as one");

    const auto beforeCpu = TrackerGeneration();
    static_cast<volatile std::uint8_t*>(memory)[16] = 0x33;
    CollectWritesUncached(base, 2 * Block);
    Require(!UnchangedSince(base, 64, beforeCpu), "a CPU write after the driver store is not seen");
}

void CheckSharedDriverStoreLog() {
    void* memory = AllocateWatched(2 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto second = base + Block;
    std::memset(memory, 0x11, 2 * Block);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> first{{base, base + 4096}};
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> stored{{second + 4096, second + 8192}};
    Require(DriverStoresSinceSequence(DriverStoreSequence(), stored) == DriverStoredSince::Unknown, "a driver store log that is off answers for a range");
    EnableDriverStoreLog();
    const auto before = DriverStoreSequence();
    Require(DriverStoresSinceSequence(before, stored) == DriverStoredSince::No, "a driver store log reports a store before any was made");
    std::thread writer([&] {
        std::array<std::byte, 64> bytes{};
        bytes.fill(std::byte{0x55});
        Write(second + 4096 + 128, bytes);
    });
    writer.join();
    Require(DriverStoresSinceSequence(before, stored) == DriverStoredSince::Stored, "a driver store made on another thread is not in the shared log");
    Require(DriverStoresSinceSequence(before, first) == DriverStoredSince::No, "a driver store is reported over a range it did not reach");
    Require(DriverStoresSinceSequence(DriverStoreSequence(), stored) == DriverStoredSince::No, "a driver store is reported again after the sequence it was made under");
}

void CheckCollectEpochSharing() {
    void* memory = AllocateWatched(4 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, 4 * Block);
    BumpCollectEpoch();
    const auto epoch = CollectEpoch();
    const auto first = CollectWrites(base, Block);
    const auto second = CollectWrites(base + Block, Block);
    Require(first != 0 && second != 0, "the epoch-shared ranges are not collected");
    std::uint64_t inside = 0;
    std::thread reader([&] {
        const auto previous = ExchangeCollectEpoch(epoch);
        inside = CollectWrites(base + Block / 2, Block);
        ExchangeCollectEpoch(previous);
    });
    reader.join();
    Require(inside != 0, "a range inside two ranges collected in the same epoch on another thread is not answered");
    const auto before = TrackerGeneration();
    static_cast<volatile std::uint8_t*>(memory)[Block + 64] = 0x22;
    BumpCollectEpoch();
    CollectWrites(base + Block, Block);
    Require(!UnchangedSince(base + Block, Block, before), "a CPU store is not seen by the first collect of the next epoch");
    Require(UnchangedSince(base + 2 * Block, Block, CollectWrites(base + 2 * Block, Block)), "an untouched range is reported written");
}

void CheckUnwatch() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto imported = base + Block;
    std::memset(memory, 0x11, 3 * Block);
    BumpCollectEpoch();
    const auto before = CollectWrites(base, 3 * Block);
    Require(before != 0 && UnchangedSince(imported, Block, before), "the import range was not initially watched");
    Unwatch(imported, Block);
    Require(!Watched(imported, Block) && !UnchangedSince(imported, Block, before), "unwatch kept trusting import stamps");
    Require(CollectWrites(base, 3 * Block) == 0 && CollectWritesUncached(imported, Block) == 0, "unwatch did not invalidate a cached collect");
    Require(Watched(base, Block) && Watched(base + 2 * Block, Block), "unwatch disabled unrelated memory");
    const auto beforeDriverWrite = TrackerGeneration();
    Require(MarkWritten(imported, Block) == 0, "an unwatched import still produces trusted stamps");
    Require(TrackerGeneration() > beforeDriverWrite, "an unwatched driver write did not invalidate byte comparisons");
    const auto beforeOwnWrite = TrackerGeneration();
    StoreOwnBytes(imported, 1, [&] { *reinterpret_cast<std::uint8_t*>(imported) = 0x42; });
    Require(TrackerGeneration() > beforeOwnWrite, "an unwatched CPU store did not invalidate byte comparisons");
    std::array<std::uint64_t, 1> generations{before};
    std::array<std::uint8_t, 1> changed{};
    Require(!ChangedBlocks(imported, Block, generations, changed), "an unwatched import still uses block stamps");
    Unwatch(imported, Block);
    const auto adjacent = CollectWrites(base, Block);
    Require(adjacent != 0, "unwatch broke the adjacent block's collect");
    static_cast<volatile std::uint8_t*>(memory)[0] = 0x33;
    CollectWritesUncached(base, Block);
    Require(!UnchangedSince(base, Block, adjacent), "unwatch hid a CPU edit in unrelated memory");
}

void CheckNewestStamp() {
    void* memory = AllocateWatched(4 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto first = base + Block;
    const auto second = base + 2 * Block;
    std::memset(memory, 0x11, 4 * Block);
    BumpCollectEpoch();
    CollectWrites(base, 4 * Block);
    Unwatch(first, 2 * Block);
    const auto firstBefore = NewestStamp(first, Block);
    const auto secondBefore = NewestStamp(second, Block);
    MarkWritten(second + 16, 4);
    Require(NewestStamp(second, Block) > secondBefore, "a driver store into unwatched memory did not advance its stamp");
    Require(NewestStamp(first, Block) == firstBefore, "a driver store advanced the stamp of other unwatched memory");
    const auto generation = TrackerGeneration();
    static_cast<volatile std::uint8_t*>(memory)[0] = 0x22;
    CollectWritesUncached(base, Block);
    Require(TrackerGeneration() > generation && NewestStamp(first, Block) == firstBefore, "a CPU store into watched memory advanced the stamp of unwatched memory");
    StoreOwnBytes(first, 1, [&] { *reinterpret_cast<std::uint8_t*>(first) = 0x42; });
    Require(NewestStamp(first, Block) > firstBefore, "an own store into unwatched memory did not advance its stamp");
    const auto partial = NewestStamp(base + 3 * Block - 8, Block + 8);
    static_cast<volatile std::uint8_t*>(memory)[3 * Block] = 0x33;
    CollectWritesUncached(base + 3 * Block, Block);
    Require(NewestStamp(base + 3 * Block - 8, Block + 8) > partial, "a CPU store into the watched part of a partly watched range did not advance its stamp");
}

#ifdef _WIN32
void CheckDeferredSharedImports() {
    constexpr std::size_t bytes = 4 * Block;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, bytes, nullptr);
    Require(section != nullptr, "cannot create shared import backing");
    auto* memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block));
    Require(memory != nullptr, "cannot allocate shared import address");
    GuestArena::GuestArenaMap_nid_postfix(memory, bytes, section, 0, PAGE_READWRITE);
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    const auto protection = [&](std::size_t offset) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(memory + offset, &info, sizeof(info)) == sizeof(info), "cannot query shared import protection");
        return info.Protect;
    };
    Require(ImportWatched(base, bytes, [] { return true; }), "fresh shared import failed");
    Require(protection(0) == PAGE_READWRITE && protection(bytes - 1) == PAGE_READWRITE, "fresh import armed shared pages");
    const auto gpu = MarkWritten(base + 4096, 4);
    Require(gpu != 0 && protection(0) == PAGE_READONLY && protection(Block) == PAGE_READWRITE, "GPU write did not activate only its block");
    CollectWritesUncached(base, Block);
    Require(!WrittenSince(base + 4096, 4, gpu) && !StoredOver(base + 4096, 4, gpu), "first collect overwrote a GPU generation");
    static_cast<volatile std::uint8_t*>(memory)[4096] = 0x44;
    CollectWritesUncached(base, Block);
    Require(WrittenSince(base + 4096, 4, gpu) && StoredOver(base + 4096, 4, gpu), "CPU store after GPU write was missed");

    const auto second = base + Block;
    const auto before = CollectWritesUncached(second, 4096);
    Require(MarkWritten(second, 4) != 0, "GPU write next to fresh pages failed");
    Require(UnchangedSinceCollected(second, 4, before), "fresh neighbors were stamped as new CPU writes over a collected page");
    Require(StoredOver(second + 8 * 4096, 4096, before), "fresh neighbor history was lost");
    const std::array<std::uint64_t, 1> generations{before};
    std::array<std::uint8_t, 1> changed{};
    std::array<std::uint8_t, 1> cpu{};
    Require(ChangedBlocks(second, Block, generations, changed, cpu) && changed[0] == BlockWritten && cpu[0] == 0, "fresh collection set the block's CPU stamp");
    const auto generation = TrackerGeneration();
    Require(MarkWritten(second, 4) == generation + 1, "an already collected GPU block advanced twice");

    const auto third = base + 2 * Block;
    const auto cached = CollectWritesUncached(third, Block);
    Require(ImportWatched(third, Block, [&] {
        static_cast<volatile std::uint8_t*>(memory)[2 * Block] = 0x55;
        return true;
    }), "tracked import window failed");
    Require(!UnchangedSince(third, Block, cached) && WrittenSince(third, Block, cached), "a store of new bytes within a tracked import window was missed");
    const auto afterImport = CollectWritesUncached(third, Block);
    static_cast<volatile std::uint8_t*>(memory)[2 * Block] = 0x66;
    Require(ImportWatched(third, Block, [] { return true; }) && WrittenSince(third, Block, afterImport), "pre-import CPU store was not collected");

    GuestArena::GuestArenaReset_nid_postfix(memory + 3 * Block, 16384);
    const auto edge = base + 3 * Block + 16384;
    Require(ImportWatched(edge, Block - 16384, [] { return true; }), "unaligned shared import failed");
    const auto edgeGpu = MarkWritten(edge, 4);
    Require(edgeGpu != 0, "fresh collection rejected an unmapped block neighbor");
    CollectWritesUncached(edge, Block - 16384);
    Require(!WrittenSince(edge, 4, edgeGpu), "edge block collection overwrote GPU results");
    GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
    CloseHandle(section);
}


void CheckPrivateMappingReuse() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 0x11, 3 * Block);
    BumpCollectEpoch();
    const auto before = CollectWrites(base, 3 * Block);
    Require(before != 0, "the original backing was not collected");
    Unwatch(base + Block, Block);
    Require(!Watched(base + Block, Block), "the imported block stayed watched");
    GuestArena::GuestArenaCommit_nid_postfix(memory, 3 * Block, PAGE_READWRITE, 3 * Block);
    Require(!Watched(base + Block, Block), "a protection-only commit restored an imported block");
    const auto neighbour = CollectWrites(base, Block);
    Require(neighbour != 0, "the neighbouring block was not collected before remapping");
    GuestArena::GuestArenaReset_nid_postfix(memory, 3 * Block);
    GuestArena::GuestArenaRelease_nid_postfix(memory, 3 * Block);
    auto* replacement = GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(base, 3 * Block, Block);
    Require(replacement == memory, "the released arena address was not reused");
    GuestArena::GuestArenaCommit_nid_postfix(replacement, 3 * Block, PAGE_READWRITE, 3 * Block);
    Require(Watched(base, 3 * Block), "fresh backing at an excluded address is not watched");
    Require(!UnchangedSince(base + Block, Block, before), "fresh backing reused an old import generation");
    Require(!UnchangedSince(base, Block, neighbour), "fresh backing reused an adjacent block's old generation");
    const auto after = CollectWrites(base, 3 * Block);
    Require(after > before && UnchangedSince(base, 3 * Block, after), "fresh backing was not collected");
    static_cast<volatile std::uint8_t*>(replacement)[Block + 8] = 0x22;
    CollectWritesUncached(base, 3 * Block);
    Require(!UnchangedSince(base + Block, Block, after), "a CPU edit in fresh backing was missed");
    Unwatch(base + Block, Block);
    Require(!Watched(base + Block, Block) && CollectWrites(base + Block, Block) == 0, "a second import of fresh backing stayed watched");
    GuestArena::GuestArenaReset_nid_postfix(replacement, 3 * Block);
    GuestArena::GuestArenaRelease_nid_postfix(replacement, 3 * Block);
}

void CheckUntrackedHint() {
    constexpr std::size_t bytes = 4 * Block;
    auto* memory = static_cast<volatile std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block));
    GuestArena::GuestArenaCommit_nid_postfix(const_cast<std::uint8_t*>(memory), bytes, PAGE_READWRITE, 16384);
    struct Cleanup {
        void* memory;
        ~Cleanup() {
            GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
        }
    } cleanup{const_cast<std::uint8_t*>(memory)};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto hinted = base + Block;
    constexpr std::size_t hintedBytes = 2 * Block;
    for (std::size_t offset = 0; offset < bytes; offset += 4096) memory[offset] = 0x11;
    BumpCollectEpoch();
    Require(CollectWrites(base, bytes) != 0 && UntrackedHint(base, bytes) == 0, "watched memory has an untracked hint");
    Unwatch(hinted, hintedBytes);
    const auto stamp = NewestStamp(hinted, hintedBytes);
    const auto first = UntrackedHint(hinted, hintedBytes);
    Require(first != 0, "memory that left the watch has no untracked hint");
    Require(UntrackedHint(base, bytes) == 0 && UntrackedHint(hinted - 4096, 8192) == 0, "a range with watched blocks has an untracked hint");
    std::array<std::uint8_t, 64> host{};
    Require(UntrackedHint(reinterpret_cast<std::uint64_t>(host.data()), host.size()) == 0, "memory outside the arena has an untracked hint");
    Require(!Watched(hinted, hintedBytes) && CollectWrites(hinted, hintedBytes) == 0 && !UnchangedSince(hinted, hintedBytes, TrackerGeneration()), "a hinted range reads as tracked");
    std::array<std::uint64_t, 2> generations{first, first};
    std::array<std::uint8_t, 2> changed{};
    Require(!ChangedBlocks(hinted, hintedBytes, generations, changed), "a hinted range uses block stamps");
    for (unsigned epoch = 0; epoch < 8; ++epoch) {
        BumpCollectEpoch();
        Require(UntrackedHint(hinted, hintedBytes) == first, "an untracked hint moved without a store");
    }
    memory[Block + 3 * 16384 + 8] ^= 1;
    Require(UntrackedHint(hinted, hintedBytes) == first, "an untracked hint was walked twice in one epoch");
    BumpCollectEpoch();
    const auto stored = UntrackedHint(hinted, hintedBytes);
    Require(stored > first, "a CPU store into a hinted range did not move its hint");
    Require(UntrackedHint(hinted + Block, Block) == first, "a CPU store moved the hint of another block");
    BumpCollectEpoch();
    Require(UntrackedHint(hinted, hintedBytes) == stored, "an untracked hint moved again without a store");
    memory[Block + 8] = memory[Block + 8];
    BumpCollectEpoch();
    const auto rewritten = UntrackedHint(hinted, hintedBytes);
    Require(rewritten > stored, "a store of the bytes already there did not move the hint");
    std::thread([&] { memory[2 * Block + 5 * 4096] ^= 1; }).join();
    BumpCollectEpoch();
    const auto threaded = UntrackedHint(hinted, hintedBytes);
    Require(threaded > rewritten, "a store from another thread did not move the hint");
    Require(UntrackedHint(hinted + Block, Block) == threaded && UntrackedHint(hinted, Block) == rewritten, "a walk stamped a store into the wrong block");
    Require(NewestStamp(hinted, hintedBytes) == stamp, "untracked hint walks advanced the block stamps");
    Unwatch(hinted, hintedBytes);
    BumpCollectEpoch();
    Require(UntrackedHint(hinted, hintedBytes) == threaded, "unwatching a hinted range again moved its hint");
    Unwatch(base, Block);
    const auto own = UntrackedHint(base, Block);
    Require(own > threaded && UntrackedHint(hinted, hintedBytes) == threaded, "a range leaving the watch did not start a hint of its own");
    GuestArena::GuestArenaReset_nid_postfix(const_cast<std::uint8_t*>(memory) + Block, hintedBytes);
    GuestArena::GuestArenaCommit_nid_postfix(const_cast<std::uint8_t*>(memory) + Block, hintedBytes, PAGE_READWRITE, 16384);
    BumpCollectEpoch();
    const auto fresh = CollectWrites(hinted, hintedBytes);
    Require(Watched(hinted, hintedBytes) && fresh != 0 && UnchangedSince(hinted, hintedBytes, fresh), "fresh backing of a hinted range is not watched");
    memory[Block + 8] = 0x5a;
    BumpCollectEpoch();
    Require(UntrackedHint(hinted, hintedBytes) == 0, "watched fresh backing has an untracked hint");
    CollectWritesUncached(hinted, hintedBytes);
    Require(!UnchangedSince(hinted, Block, fresh), "an untracked hint walk took a CPU store from watched memory");
    Unwatch(hinted, hintedBytes);
    Require(UntrackedHint(hinted, hintedBytes) > own, "fresh backing leaving the watch did not start a new hint");
}

std::uint64_t ChildHostHole() {
    std::uintptr_t arena = 0;
    std::size_t arenaBytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arena, &arenaBytes);
    return (arena + arenaBytes / 2) & ~(std::uint64_t{Block} - 1);
}

void CheckHostHoleInChild() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Require(length != 0 && length < executable.size(), "cannot name the test executable");
    const std::wstring command = L"\"" + std::wstring(executable.data(), length) + L"\" " + HostHoleMode;
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.data(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process) != 0, "cannot create the host-hole child");
    const bool reserved = VirtualAllocEx(process.hProcess, reinterpret_cast<void*>(ChildHostHole()), Block, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
    const bool resumed = reserved && ResumeThread(process.hThread) != static_cast<DWORD>(-1);
    const bool exited = resumed && WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0;
    if (!exited) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exitCode = 1;
    const bool reported = GetExitCodeProcess(process.hProcess, &exitCode) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require(reserved, "cannot reserve a host range in the child's arena interval");
    Require(resumed, "cannot resume the host-hole child");
    Require(exited, "the host-hole child did not exit");
    Require(reported && exitCode == 0, "the host-hole child failed");
}

void CheckHostHole() {
    constexpr std::size_t bytes = 64;
    const auto hole = ChildHostHole();
    Require(GuestArena::GuestArenaHostRegionOverlaps_nid_postfix(hole, bytes), "the range reserved before the arena is not an arena host region");
    const auto before = TrackerGeneration();
    Require(!Watched(hole, bytes), "an arena host hole reads as watched");
    Require(CollectWrites(hole, bytes) == 0 && CollectWritesUncached(hole, bytes) == 0, "an arena host hole was collected");
    auto* watched = static_cast<std::uint8_t*>(AllocateWatched(Block, true));
    struct Cleanup {
        void* memory;
        ~Cleanup() { GuestArena::GuestArenaRelease_nid_postfix(memory, Block); }
    } cleanup{watched};
    const auto base = reinterpret_cast<std::uint64_t>(watched);
    for (unsigned epoch = 0; epoch < 2; ++epoch) {
        BumpCollectEpoch();
        watched[7] ^= 1;
        Require(CollectWrites(base + 7, 1) != 0, "a warm host-hole check could not collect its watched range");
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            Require(CollectWrites(base + 7, 1) != 0, "a warm host-hole check lost its watched range");
            Require(CollectWrites(hole, bytes) == 0, "a repeated host-hole query hit a collect memo");
            Require(CollectWrites(hole + 7, 1) == 0, "a page-rounded host-hole query hit a collect memo");
        }
    }
    Require(StoredOver(hole, bytes, before), "an arena host hole reads as not stored over");
    Require(!UnchangedSince(hole, bytes, before), "an arena host hole reads as unchanged");
    const std::array queries{UnchangedQuery{hole, bytes, before}};
    Require(!UnchangedSinceAll(queries), "an arena host hole reads as unchanged in a batch");
    Require(!UnchangedSinceCollected(hole, bytes, before), "an arena host hole reads as unchanged since a collect");
    Require(MarkWritten(hole, bytes) == 0, "a driver store into an arena host hole is stamped");
    Require(StoreOwnBytes(hole, bytes, [] {}) == 0, "an own store into an arena host hole is stamped");
    Require(!WrittenSince(hole, bytes, before), "an arena host hole reads its stamps as written");
    std::array<std::uint64_t, 1> generations{before};
    std::array<std::uint8_t, 1> changed{};
    std::array<std::uint8_t, 1> cpu{};
    Require(!ChangedBlocks(hole, bytes, generations, changed, cpu) && changed[0] == BlockWritten && cpu[0] == 1, "an arena host hole uses block stamps");
    bool imported = false;
    Require(ImportWatched(hole, bytes, [&] {
        imported = true;
        return true;
    }) && imported, "an import into an arena host hole did not run");
    Require(!Watched(hole, bytes), "an import made an arena host hole watched");
}
#endif

void CheckVersionRanges() {
    constexpr std::size_t count = 80;
    constexpr auto bytes = count * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes, true));
    struct Cleanup {
        void* memory;
        ~Cleanup() {
#ifdef _WIN32
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
#else
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, bytes);
            munmap(memory, bytes);
#endif
        }
    } cleanup{memory};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    Require(UnchangedSince(base, bytes, TrackerGeneration()), "a new range has newer version stamps");
    std::memset(memory, 17, bytes);
    const auto initial = CollectWritesUncached(base, bytes);
    Require(initial != 0, "version ranges could not be collected");
    struct Write { std::size_t block; std::uint64_t generation; };
    std::vector<Write> history;
    std::vector<std::uint64_t> checkpoints{initial};
    std::uint32_t random = 0x61387a29;
    const auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    const auto expected = [&](const UnchangedQuery& query) {
        if (query.generation == 0) return false;
        for (const auto& write : history) {
            const auto begin = base + write.block * Block;
            if (write.generation > query.generation && begin < query.address + query.bytes && query.address < begin + Block) return false;
        }
        return true;
    };
    for (unsigned step = 0; step < 512; ++step) {
        const auto block = step < 3 ? step : next() % count;
        std::uint64_t generation;
        if (step % 3 == 0) {
            memory[block * Block + 9] ^= 1;
            generation = CollectWritesUncached(base + block * Block, Block);
        } else generation = MarkWritten(base + block * Block + 13, 4);
        Require(generation > checkpoints.back(), "version range write did not advance the tracker");
        history.push_back({block, generation});
        checkpoints.push_back(generation);
        for (unsigned check = 0; check < 16; ++check) {
            std::array<UnchangedQuery, 3> queries;
            bool all = true;
            for (auto& query : queries) {
                const auto offset = next() % bytes;
                query = {base + offset, 1 + next() % (bytes - offset), checkpoints[next() % checkpoints.size()]};
                const auto same = expected(query);
                Require(UnchangedSince(query.address, query.bytes, query.generation) == same, "version range disagrees with write history");
                all = all && same;
            }
            Require(UnchangedSinceAll(queries) == all, "batched version ranges disagree with write history");
        }
    }
    Require(!UnchangedSince(base, bytes, 0), "an unknown generation was accepted");
    Require(UnchangedSince(base, bytes, checkpoints.back()), "the latest checkpoint was rejected");
}

void CheckCollectBoundaries() {
    constexpr std::size_t bytes = 3 * Block;
    auto* memory = static_cast<std::uint8_t*>(AllocateWatched(bytes));
    struct Cleanup {
        void* memory;
        ~Cleanup() {
#ifdef _WIN32
            GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
#else
            Unwatch(reinterpret_cast<std::uint64_t>(memory), bytes);
            munmap(memory, bytes);
#endif
        }
    } cleanup{memory};
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::memset(memory, 17, bytes);
    bool unboundedObserved = false;
    std::thread observer([&] {
        const auto before = CollectWrites(base + 7, 1);
        memory[7] = 19;
        const auto after = CollectWrites(base + 7, 1);
        unboundedObserved = before != 0 && after > before && !UnchangedSince(base + 7, 1, before);
    });
    observer.join();
    Require(unboundedObserved, "a thread without collect epochs reused a stale observation");
    for (const auto offset : {7u, 4096u + 3u, static_cast<unsigned>(Block - 8), static_cast<unsigned>(Block + 3)}) {
        BumpCollectEpoch();
        const auto before = CollectWrites(base + offset, 16);
        Require(before != 0, "an epoch lost watched coverage");
        memory[offset] ^= 1;
        BumpCollectEpoch();
        Require(CollectWrites(base + offset, 16) > before && !UnchangedSince(base + offset, 16, before), "a new epoch missed a CPU write");
        const auto observed = TrackerGeneration();
        memory[offset + 1] ^= 1;
        Require(CollectWritesUncached(base + offset, 16) > observed && !UnchangedSince(base + offset, 16, observed), "an uncached observation reused an epoch result");
        const auto stored = MarkWritten(base + offset + 2, 1);
        Require(CollectWrites(base + offset, 16) >= stored && !UnchangedSince(base + offset, 16, observed), "a memoized observation hid a driver write");
    }
#ifndef _WIN32
    BumpCollectEpoch();
    Require(CollectWrites(base, 1) != 0, "initial partial-block observation failed");
    Unwatch(base + 4096, 4096);
    Require(CollectWrites(base + 4096, 1) == 0, "an unwatched neighbor retained an epoch observation");
    BumpCollectEpoch();
    const auto before = CollectWrites(base, 16);
    Require(before != 0, "an unwatched neighbor prevented collecting a watched prefix");
    memory[0] ^= 1;
    BumpCollectEpoch();
    Require(CollectWrites(base, 16) > before && !UnchangedSince(base, 16, before), "partial-block collection missed a CPU write");
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(memory + 4096, 4096);
    Require(CollectWrites(base + 4096, 1) != 0, "a re-registered neighbor stayed unwatched");
#endif
}
}

void CheckImportWindow() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 3 * Block);
    const auto synced = CollectWritesUncached(base, 3 * Block);
    Require(synced != 0, "the import window block is not collected");
    Require(ImportWatched(base, 3 * Block, [&] {
        bytes[0x100] = 0x11;
        bytes[Block + 0x2000] = 0x22;
        return true;
    }), "the import window import failed");
    Require(Watched(base, 3 * Block), "an import window left its range unwatched");
    const std::array<std::uint64_t, 3> generations{synced, synced, synced};
    std::array<std::uint8_t, 3> changed{};
    std::array<std::uint8_t, 3> cpu{};
    Require(ChangedBlocks(base, 3 * Block, generations, changed, cpu), "the import window block is not tracked");
    Require(changed[0] == BlockUnchanged && UnchangedSince(base, Block, synced), "a page an import window resolved without changing its bytes reads as written");
    Require(changed[1] == BlockWritten && WrittenSince(base + Block, Block, synced) && cpu[1] == 1, "a store of new bytes within an import window was missed");
    Require(changed[2] == BlockUnchanged && cpu[2] == 0, "a block the import window did not touch reads as written");
    Require(cpu[0] == 1, "a page an import window resolved no longer counts as possibly touched by the CPU");
    const auto after = CollectWritesUncached(base, 3 * Block);
    bytes[0x100] = 0x33;
    static_cast<void>(CollectWritesUncached(base, 3 * Block));
    Require(!UnchangedSince(base, Block, after), "a store after the import window was missed");
}

void CheckImportWindowZeroPages() {
    void* memory = AllocateWatched(5 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    auto* plain = static_cast<std::uint8_t*>(memory);
    std::memset(plain, 0x11, Block);
    std::memset(plain + Block, 0, 2 * Block);
    std::memset(plain + 3 * Block, 0x44, Block);
    std::memset(plain + 4 * Block, 0x66, Block);
    const auto synced = CollectWritesUncached(base, 5 * Block);
    Require(synced != 0, "the zero page import window block is not collected");
    Require(ImportWatched(base, 5 * Block, [&] {
        bytes[0x100] = 0x11;
        bytes[Block + 0x100] = 0;
        bytes[2 * Block + 0x2000] = 0x22;
        bytes[3 * Block + 0x3000] = 0x44;
        bytes[4 * Block + 0x100] = 0x55;
        return true;
    }), "the zero page import window import failed");
    const std::array<std::uint64_t, 5> generations{synced, synced, synced, synced, synced};
    std::array<std::uint8_t, 5> changed{};
    std::array<std::uint8_t, 5> cpu{};
    Require(ChangedBlocks(base, 5 * Block, generations, changed, cpu), "the zero page import window block is not tracked");
    Require(changed[0] == BlockUnchanged && cpu[0] == 1, "a page before the zero pages of an import window reads as written");
    Require(changed[1] == BlockUnchanged && UnchangedSince(base + Block, Block, synced), "a zero page an import window resolved without changing its bytes reads as written");
    Require(changed[2] == BlockWritten && WrittenSince(base + 2 * Block, Block, synced), "a store of new bytes to a zero page within an import window was missed");
    Require(changed[3] == BlockUnchanged && UnchangedSince(base + 3 * Block, Block, synced), "a page after the zero pages of an import window was compared with other bytes");
    Require(changed[4] == BlockWritten && WrittenSince(base + 4 * Block, Block, synced), "a store of new bytes after the zero pages of an import window was missed");
}

void ProtectPage(std::uint64_t address, bool readable) {
#ifdef _WIN32
    DWORD previous = 0;
    Require(VirtualProtect(reinterpret_cast<void*>(address), 4096, readable ? PAGE_READWRITE : PAGE_NOACCESS, &previous) != 0, "cannot change the protection of a test page");
#else
    Require(mprotect(reinterpret_cast<void*>(address), 4096, readable ? PROT_READ | PROT_WRITE : PROT_NONE) == 0, "cannot change the protection of a test page");
#endif
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(address), 4096);
}

void CheckUnwatchedImport() {
    void* memory = AllocateWatched(3 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 3 * Block);
    const auto synced = CollectWritesUncached(base, 3 * Block);
    Require(synced != 0, "the unwatched import block is not collected");
    const auto imported = base + Block / 2;
    const std::size_t importedBytes = Block + Block / 2;
    const auto unreadable = base + Block + 0x3000;
    ProtectPage(unreadable, false);
    bool threw = false;
    bool ran = false;
    try {
        static_cast<void>(ImportWatched(imported, importedBytes, [&] {
            ran = true;
            return true;
        }));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw && !ran, "an import that stays watched did not read its range before the import");
    threw = false;
    try {
        Require(ImportWatched(imported, importedBytes, [&] {
            bytes[Block / 2 + 0x100] = 0x22;
            ran = true;
            return true;
        }, false), "an import that leaves the watch failed");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(!threw && ran, "an import that leaves the watch read its range");
    ProtectPage(unreadable, true);
    Require(!Watched(imported, importedBytes) && !Watched(imported, 4096) && !Watched(base + Block, Block), "an import that leaves the watch is still watched");
    Require(Watched(base + 2 * Block, Block), "an import that leaves the watch unwatched a block outside its range");
    Require(UnchangedSince(base + 2 * Block, Block, synced), "an import that leaves the watch stamped a block outside its range");
#ifndef _WIN32
    Require(Watched(base, Block / 2), "an import that leaves the watch unwatched the pages before its range");
    const std::array<std::uint64_t, 1> generations{synced};
    std::array<std::uint8_t, 1> changed{};
    std::array<std::uint8_t, 1> cpu{};
    Require(ChangedBlocks(base, Block / 2, generations, changed, cpu), "the pages before an import that leaves the watch are not tracked");
    Require(changed[0] == BlockUnchanged && UnchangedSince(base, Block / 2, synced) && !WrittenSince(base, Block / 2, synced), "a store to a page that left the watch reads as a store to the watched pages of its block");
    Require(cpu[0] == 1, "a page resolved while an import left the watch no longer counts as possibly touched by the CPU");
    const auto after = CollectWritesUncached(base, Block / 2);
    bytes[0x100] = 0x33;
    static_cast<void>(CollectWritesUncached(base, Block / 2));
    Require(after != 0 && !UnchangedSince(base, Block / 2, after), "a store beside an import that left the watch was missed");
#endif
}

class PartlyCommitted {
public:
    static constexpr std::size_t Bytes = 4 * Block;

    PartlyCommitted() {
#ifdef _WIN32
        memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocateAtOrAbove_nid_postfix(GuestArena::GuestArenaHighestFree_nid_no_patch(Bytes, Block), Bytes, Block));
#else
        void* raw = mmap(nullptr, Bytes + Block, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        Require(raw != MAP_FAILED, "cannot reserve the partly committed range");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
        if (aligned != begin) munmap(raw, aligned - begin);
        if (aligned + Bytes != begin + Bytes + Block) munmap(reinterpret_cast<void*>(aligned + Bytes), begin + Block - aligned);
        memory = reinterpret_cast<std::uint8_t*>(aligned);
#endif
        Require(memory != nullptr, "cannot reserve the partly committed range");
    }

    ~PartlyCommitted() {
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(memory, Bytes);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(), Bytes);
        GuestArena::GuestArenaRelease_nid_postfix(memory, Bytes);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, Bytes);
        munmap(memory, Bytes);
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(), Bytes);
#endif
    }

    PartlyCommitted(const PartlyCommitted&) = delete;
    PartlyCommitted& operator=(const PartlyCommitted&) = delete;

    void Commit(std::size_t block) {
#ifdef _WIN32
        GuestArena::GuestArenaCommit_nid_postfix(memory + block * Block, Block, PAGE_READWRITE, Block);
#else
        Require(mmap(memory + block * Block, Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED, "cannot commit a block of the partly committed range");
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(memory + block * Block, Block);
#endif
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(block), Block);
    }

    void Decommit(std::size_t block) {
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(memory + block * Block, Block);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory + block * Block, Block);
        Require(mmap(memory + block * Block, Block, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) != MAP_FAILED, "cannot decommit a block of the partly committed range");
#endif
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(Address(block), Block);
    }

    std::uint8_t* Data() const { return memory; }
    std::uint64_t Address(std::size_t block = 0) const { return reinterpret_cast<std::uint64_t>(memory) + block * Block; }

private:
    std::uint8_t* memory = nullptr;
};

using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

std::uint64_t CollectCommitted(const PartlyCommitted& range, Runs& runs) {
    BumpCollectEpoch();
    return CollectCommittedWrites(range.Address(), PartlyCommitted::Bytes, runs);
}

void CheckEpochDirtyImportFailure() {
    struct Restore {
        bool enabled = EpochDirtySetEnabled();
        ~Restore() { SetEpochDirtySet(enabled); }
    } restore;
    struct ImportFailure {};
    SetEpochDirtySet(true);
    PartlyCommitted range;
    range.Commit(0);
    range.Commit(1);
    const auto base = range.Address();
    auto* bytes = static_cast<volatile std::uint8_t*>(range.Data());
    std::memset(range.Data(), 0x11, 2 * Block);
    for (const bool staysWatched : {true, false}) {
        for (const bool throws : {false, true}) {
            BumpCollectEpoch();
            const auto generation = CollectWritesUncached(base, 2 * Block);
            Require(generation != 0, "the failed import block is not collected");
            const auto serial = DirtySerial();
            bytes[0x100] = static_cast<std::uint8_t>(bytes[0x100] + 1u);
            bool ran = false;
            bool threw = false;
            try {
                Require(!ImportWatched(base, Block, [&] {
                    ran = true;
                    if (throws) throw ImportFailure{};
                    return false;
                }, staysWatched), "a failed import callback returned success");
            } catch (const ImportFailure&) {
                threw = true;
            }
            Require(ran && threw == throws, "a failed import callback did not preserve its result or exception");
            Require(Watched(base, Block), "a failed import removed write watching");
            Require(!UnchangedSince(base, Block, generation), "a failed import did not stamp the preceding CPU store");
            Require(!UnchangedSinceDirtySerial(base, Block, serial), "a failed import hid its preceding CPU store from the dirty log");
            Require(UnchangedSince(base + Block, Block, generation) && UnchangedSinceDirtySerial(base + Block, Block, serial), "a failed import invalidated memory outside its range");
        }
    }
}

void CheckEpochDirtyUnstampedBatch() {
    struct Restore {
        bool enabled = EpochDirtySetEnabled();
        ~Restore() { SetEpochDirtySet(enabled); }
    } restore;
    SetEpochDirtySet(true);
    PartlyCommitted range;
    range.Commit(0);
    range.Commit(1);
    range.Commit(2);
    const auto base = range.Address();
    std::memset(range.Data(), 0x11, 3 * Block);
    BumpCollectEpoch();
    const auto generation = CollectWritesUncached(base, 3 * Block);
    Require(generation != 0, "the mixed write batch is not collected");
    Unwatch(base + Block, Block);
    Require(Watched(base, Block) && !Watched(base + Block, Block), "the mixed write batch does not span watched and unwatched blocks");
    const auto serial = DirtySerial();
    const std::array<DirtyRange, 4> ranges{{
        {base + Block, base + Block + 64},
        {base, base + 64},
        {base + 2 * Block, base + 2 * Block},
        {base + 2 * Block + 64, base + 2 * Block},
    }};
    const auto marked = MarkWrittenUnlessStored(ranges, generation);
    Require(marked.generation == 0 && marked.storedOver, "a mixed write batch changed its unstamped result");
    Require(!UnchangedSinceDirtySerial(base, 64, serial), "a mixed write batch hid its watched range from the dirty log");
    Require(!UnchangedSinceDirtySerial(base + Block, 64, serial), "a mixed write batch hid its unwatched range from the dirty log");
    Require(!UnchangedCommittedSince(std::span<const DirtyRange>(ranges).subspan(1, 1), generation), "a mixed write batch did not retain its legacy unstamped-store record");
    Require(UnchangedSince(base + 2 * Block, Block, generation) && UnchangedSinceDirtySerial(base + 2 * Block, Block, serial), "empty or reversed ranges dirtied the untouched block");
}

void CheckEpochDirtyBlockGranularity() {
    struct Restore {
        bool enabled = EpochDirtySetEnabled();
        ~Restore() { SetEpochDirtySet(enabled); }
    } restore;
    SetEpochDirtySet(true);
    PartlyCommitted range;
    for (std::size_t index = 0; index < 3; ++index) range.Commit(index);
    const auto base = range.Address();
    std::memset(range.Data(), 0x11, 3 * Block);
    const std::array<DirtyRange, 3> writes{{
        {base + 16, base + 32},
        {base + 2 * 4096 + 8, base + 2 * 4096 + 24},
        {base + Block - 8, base + Block + 8},
    }};
    const std::array<DirtyRange, 4> queries{{
        {base, base + 16},
        {base + 4096, base + 4096 + 16},
        {base + Block, base + Block + 16},
        {base + 2 * Block, base + 2 * Block + 16},
    }};
    for (const auto& [begin, end] : writes) {
        const auto generation = CollectWritesUncached(base, 3 * Block);
        Require(generation != 0, "the block-granularity range is not collected");
        const auto serial = DirtySerial();
        Require(MarkWritten(begin, static_cast<std::size_t>(end - begin)) != 0, "the block-granularity driver store is not stamped");
        for (const auto& [first, last] : queries) {
            const auto bytes = static_cast<std::size_t>(last - first);
            Require(UnchangedSinceDirtySerial(first, bytes, serial) == UnchangedSince(first, bytes, generation), "a dirty proof disagreed with the block stamps after an adjacent, disjoint or crossing driver store");
        }
        Require(!UnchangedSinceDirtySerial(begin, static_cast<std::size_t>(end - begin), serial), "an overlapping driver store was accepted as clean");
    }
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr auto tail = maximum - (Block - 1);
    const auto beforeTail = DirtySerial();
    Require(MarkWritten(maximum - 7, 7) == 0, "an address outside the watched arena was stamped");
    Require(!UnchangedSinceDirtySerial(tail, 1, beforeTail), "rounding the final block wrapped its dirty range");
    Require(UnchangedSinceDirtySerial(tail - Block, 1, beforeTail), "rounding the final block invalidated its predecessor");
    SetEpochDirtySet(false);
    const auto beforeEnable = DirtySerial();
    SetEpochDirtySet(true);
    Require(!UnchangedSinceDirtySerial(base, 16, beforeEnable) && !UnchangedSinceDirtySerial(tail, 1, beforeEnable), "re-enabling the dirty log did not preserve the full invalidation sentinel");
    const auto enabled = DirtySerial();
    SetEpochDirtySet(true);
    Require(DirtySerial() == enabled && UnchangedSinceDirtySerial(tail, 1, enabled), "an already enabled dirty log invalidated its current proof");
}

void CheckEpochDirtyCollectedPages() {
    struct Restore {
        bool enabled = EpochDirtySetEnabled();
        std::size_t widen = EpochDirtySetWidenBytes();
        ~Restore() {
            SetEpochDirtySetWidenBytes(widen);
            SetEpochDirtySet(enabled);
        }
    } restore;
    SetEpochDirtySet(true);
    SetEpochDirtySetWidenBytes(0);
    PartlyCommitted range;
    range.Commit(0);
    range.Commit(1);
    const auto base = range.Address();
    auto* memory = static_cast<volatile std::uint8_t*>(range.Data());
    std::memset(range.Data(), 0x11, 2 * Block);
    const auto generation = CollectWritesUncached(base, 2 * Block);
    Require(generation != 0, "the sibling-page range is not collected");
    const auto serial = DirtySerial();
    memory[2 * 4096 + 8] = static_cast<std::uint8_t>(memory[2 * 4096 + 8] ^ 1u);
    Require(DirtySerial() == serial, "a raw CPU store was published before its collection");
    Require(CollectWritesUncached(base + 2 * 4096, 16) != 0, "a sibling CPU page could not be collected");
    Require(!UnchangedSince(base, 16, generation) && !UnchangedSinceDirtySerial(base, 16, serial), "a collected sibling page did not invalidate the entire stamped block");
    std::uint64_t through = 0;
    Require(UnchangedSince(base + Block, 16, generation) && UnchangedSinceDirtySerial(base + Block, 16, serial, &through) && through == DirtySerial(), "a collected CPU page invalidated an unrelated block or advanced the wrong serial");
    const auto secondGeneration = CollectWritesUncached(base + Block, Block);
    Require(secondGeneration != 0, "the clean block could not be collected before its new proof");
    const auto secondSerial = DirtySerial();
    memory[Block] = static_cast<std::uint8_t>(memory[Block] ^ 1u);
    Require(CollectWritesUncached(base + Block, 16) != 0, "an overlapping CPU store could not be collected");
    Require(!UnchangedSince(base + Block, 16, secondGeneration) && !UnchangedSinceDirtySerial(base + Block, 16, secondSerial), "advancing a clean proof hid a later overlapping CPU store");
}

void CheckEpochDirtyConcurrentPublication() {
    struct Restore {
        bool enabled = EpochDirtySetEnabled();
        ~Restore() { SetEpochDirtySet(enabled); }
    } restore;
    SetEpochDirtySet(true);
    PartlyCommitted range;
    range.Commit(0);
    const auto base = range.Address();
    std::memset(range.Data(), 0x11, Block);
    Require(CollectWritesUncached(base, Block) != 0, "the concurrent publication block is not collected");
    const auto serial = DirtySerial();
    std::atomic<bool> finished{false};
    std::exception_ptr failure;
    std::jthread writer([&] {
        try {
            for (std::size_t index = 0; index < 512; ++index) Require(MarkWritten(base + 4096 + 8, 16) != 0, "a concurrent driver store is not stamped");
        } catch (...) {
            failure = std::current_exception();
        }
        finished.store(true, std::memory_order_release);
    });
    bool accepted = false;
    do {
        if (DirtySerial() > serial && UnchangedSinceDirtySerial(base, 16, serial)) accepted = true;
        std::this_thread::yield();
    } while (!finished.load(std::memory_order_acquire));
    writer.join();
    if (failure != nullptr) std::rethrow_exception(failure);
    Require(!accepted && !UnchangedSinceDirtySerial(base, 16, serial), "a concurrent dirty publication was accepted before its block invalidation was visible");
}

void CheckEpochDirtySet() {
    struct Restore {
        bool enabled = EpochDirtySetEnabled();
        std::size_t widen = EpochDirtySetWidenBytes();
        ~Restore() {
            SetEpochDirtySet(enabled);
            SetEpochDirtySetWidenBytes(widen);
        }
    } restore;
    SetEpochDirtySet(true);
    SetEpochDirtySetWidenBytes(Block);
    void* memory = AllocateWatched(4 * Block);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    auto* bytes = static_cast<volatile std::uint8_t*>(memory);
    std::memset(memory, 0x11, 4 * Block);
    BumpCollectEpoch();
    Require(CollectWrites(base, 4 * Block) != 0, "the dirty set block is not collected");
    const auto clean = DirtySerial();
    Require(UnchangedSinceDirtySerial(base, 4 * Block, clean), "an untouched range reads as dirty");
    bytes[Block + 40000] = 0x22;
    BumpCollectEpoch();
    const auto generation = CollectWrites(base + Block, 64);
    Require(generation != 0 && DirtySerial() > clean, "a collect widened to the block did not find the store in another page of the block");
    Require(!UnchangedSince(base + Block + 40000, 64, generation - 1), "the widened collect did not stamp the stored block");
    std::uint64_t through = 0;
    Require(!UnchangedSinceDirtySerial(base + Block, Block, clean, &through), "a collected CPU store is not in the dirty log");
    Require(UnchangedSinceDirtySerial(base + 2 * Block, Block, clean, &through) && through == DirtySerial(), "a store in one block dirtied another");
    Require(UnchangedSinceDirtySerial(base + Block, Block, through), "a range reads as dirty after the serial it was checked through");
    const auto beforeDriver = DirtySerial();
    Require(MarkWritten(base + 2 * Block + 16, 4) != 0, "a driver store into the dirty set block is not stamped");
    Require(!UnchangedSinceDirtySerial(base + 2 * Block, 64, beforeDriver), "a driver store is not in the dirty log");
    Require(UnchangedSinceDirtySerial(base + 3 * Block, Block, beforeDriver), "a driver store dirtied another block");
    const auto beforeWrite = DirtySerial();
    const std::array<std::byte, 64> zeros{};
    Write(base + 3 * Block, zeros);
    Require(!UnchangedSinceDirtySerial(base + 3 * Block, 64, beforeWrite), "a driver write is not in the dirty log");
    const auto beforeUnwatch = DirtySerial();
    Unwatch(base, Block);
    Require(!UnchangedSinceDirtySerial(base, Block, beforeUnwatch), "an unwatch is not in the dirty log");
    const auto beforeOverflow = DirtySerial();
    for (int repeat = 0; repeat < 5000; ++repeat) MarkWritten(base + 2 * Block, 4);
    Require(!UnchangedSinceDirtySerial(base + 3 * Block, Block, beforeOverflow), "an overflowed dirty log answered unchanged");
    Require(UnchangedSinceDirtySerial(base + 3 * Block, Block, DirtySerial()), "the current serial reads as dirty");
    const auto beforeStore = DirtySerial();
    bytes[3 * Block + 100] = 0x33;
    Require(UnchangedSinceDirtySerial(base + 3 * Block, Block, beforeStore), "an uncollected store showed before its epoch");
    BumpCollectEpoch();
    Require(CollectWrites(base + 3 * Block, Block) != 0 && !UnchangedSinceDirtySerial(base + 3 * Block, Block, beforeStore), "the next epoch's collect did not log the store");
    SetEpochDirtySet(false);
    bytes[2 * Block + 40000] = 0x44;
    BumpCollectEpoch();
    const auto beforeExact = DirtySerial();
    const auto exact = CollectWrites(base + 2 * Block, 64);
    Require(exact != 0, "the exact collect failed");
    Require(UnchangedSince(base + 2 * Block + 40000, 64, exact), "a collect with the dirty set off walked beyond its range");
    BumpCollectEpoch();
    Require(CollectWrites(base + 2 * Block + 40000, 64) != 0 && !UnchangedSince(base + 2 * Block + 40000, 64, exact), "the exact collect of the stored page did not stamp the store");
    Require(MarkWritten(base + 2 * Block + 16, 4) != 0, "a driver store with the dirty set off was not stamped");
    Write(base + 3 * Block, zeros);
    Unwatch(base + 3 * Block, Block);
    Require(DirtySerial() == beforeExact, "writes with the dirty set off updated the dirty log");
    Require(!UnchangedSinceDirtySerial(base + 2 * Block, Block, beforeExact), "a disabled dirty log accepted a proof");
    SetEpochDirtySet(true);
    Require(DirtySerial() > beforeExact && !UnchangedSinceDirtySerial(base + 2 * Block, Block, beforeExact), "re-enabling the dirty log preserved a proof from before skipped writes");
    const auto enabled = DirtySerial();
    SetEpochDirtySet(true);
    Require(DirtySerial() == enabled && UnchangedSinceDirtySerial(base + 2 * Block, Block, enabled), "enabling an already enabled dirty log invalidated its current proof");
}

void CheckCommittedRanges() {
    PartlyCommitted range;
    const auto base = range.Address();
    range.Commit(0);
    std::memset(range.Data(), 0x11, Block);
    Require(CollectWrites(base, PartlyCommitted::Bytes) == 0, "a partly committed range was collected as a whole");
    Runs runs;
    const Runs first{{base, base + Block}};
    const auto synced = CollectCommitted(range, runs);
    Require(synced != 0 && runs == first, "the committed part of a partly committed range was not collected");
    Require(UnchangedCommittedSince(runs, synced), "a collected committed part reads as changed");

    range.Data()[8] = 0x22;
    const auto written = CollectCommitted(range, runs);
    Require(written > synced && runs == first && !UnchangedCommittedSince(runs, synced) && UnchangedCommittedSince(runs, written), "a CPU write to the committed part was missed");
    Require(MarkWritten(base + 16, 4) != 0 && !UnchangedCommittedSince(first, written), "a driver store into the committed part was missed");
    Require(UnchangedCommittedSince(first, CollectCommitted(range, runs)), "a collect after a driver store reads the committed part as changed");

    const auto beforeCommit = CollectCommitted(range, runs);
    range.Commit(2);
    const Runs both{{base, base + Block}, {range.Address(2), range.Address(3)}};
    const auto grown = CollectCommitted(range, runs);
    Require(grown != 0 && runs == both, "a block committed inside the range is not described");
    Require(!UnchangedCommittedSince(runs, beforeCommit), "a block committed inside the range reads as unchanged since before the commit");
    Require(UnchangedCommittedSince(runs, grown), "the committed parts read as changed after their collect");

    range.Decommit(2);
    range.Commit(2);
    const auto recommitted = CollectCommitted(range, runs);
    Require(recommitted != 0 && runs == both && !UnchangedCommittedSince(runs, grown), "a block decommitted and committed again reads as unchanged");

    range.Decommit(2);
    Require(CollectCommitted(range, runs) != 0 && runs == first, "a decommitted block is still described as committed");

#ifdef _WIN32
    HANDLE firstSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Block, nullptr);
    HANDLE secondSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Block, nullptr);
    struct Sections {
        HANDLE first;
        HANDLE second;
        ~Sections() {
            if (first != nullptr) CloseHandle(first);
            if (second != nullptr) CloseHandle(second);
        }
    } sections{firstSection, secondSection};
    Require(firstSection != nullptr && secondSection != nullptr, "cannot create the shared backings of the partly committed range");
    const auto beforeMap = CollectCommitted(range, runs);
    GuestArena::GuestArenaMap_nid_postfix(range.Data() + 3 * Block, Block, firstSection, 0, PAGE_READWRITE);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(range.Address(3), Block);
    const Runs viewed{{base, base + Block}, {range.Address(3), range.Address(4)}};
    const auto mapped = CollectCommitted(range, runs);
    Require(mapped != 0 && runs == viewed, "a view mapped inside the range is not described");
    Require(!UnchangedCommittedSince(runs, beforeMap), "a view mapped inside the range reads as unchanged since before the mapping");
    Require(UnchangedCommittedSince(runs, mapped), "a collected view reads as changed");
    GuestArena::GuestArenaReset_nid_postfix(range.Data() + 3 * Block, Block);
    GuestArena::GuestArenaMap_nid_postfix(range.Data() + 3 * Block, Block, secondSection, 0, PAGE_READWRITE);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(range.Address(3), Block);
    Require(CollectCommitted(range, runs) != 0 && runs == viewed && !UnchangedCommittedSince(runs, mapped), "a view replaced inside the range reads as unchanged");
    GuestArena::GuestArenaReset_nid_postfix(range.Data() + 3 * Block, Block);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(range.Address(3), Block);
    Require(CollectCommitted(range, runs) != 0 && runs == first, "an unmapped view is still described as committed");
#endif

    range.Decommit(0);
    Require(CollectCommitted(range, runs) != 0 && runs.empty() && UnchangedCommittedSince(runs, TrackerGeneration()), "a range with no committed page did not collect as empty");
    Require(!UnchangedCommittedSince(first, 0), "an unknown generation was accepted for the committed parts");
}

int main() {
    try {
        if (!WriteWatched()) {
            std::cout << "no write watching: skipped\n";
            return 77;
        }
#ifdef _WIN32
        if (std::wstring_view(GetCommandLineW()).ends_with(HostHoleMode)) {
            CheckHostHole();
            return 0;
        }
#endif
        CheckSharedBlock();
        CheckOwnStore();
        CheckSharedDriverStoreLog();
        CheckCollectEpochSharing();
        CheckUnwatch();
        CheckNewestStamp();
        CheckEpochDirtySet();
        CheckEpochDirtyImportFailure();
        CheckEpochDirtyUnstampedBatch();
        CheckEpochDirtyBlockGranularity();
        CheckEpochDirtyCollectedPages();
        CheckEpochDirtyConcurrentPublication();
#ifdef _WIN32
        CheckDeferredSharedImports();
        CheckPrivateMappingReuse();
        CheckUntrackedHint();
        CheckHostHoleInChild();
#endif
        CheckVersionRanges();
        CheckCollectBoundaries();
        CheckImportWindow();
        CheckImportWindowZeroPages();
        CheckUnwatchedImport();
        CheckCommittedRanges();
    } catch (const std::exception& error) {
        std::cerr << "write tracking test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "write tracking tests passed\n";
    return 0;
}

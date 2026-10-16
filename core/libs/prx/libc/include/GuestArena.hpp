#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTARENA_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTARENA_HPP

#include <cstddef>
#include <cstdint>

// Guest virtual memory is placed inside one reserved arena below the PS5 application map limit
// absolute address and breaks on host addresses outside that range.
namespace GuestArena {

struct MappingSpan {
    std::uintptr_t begin, end;
    std::uint64_t identity;
    std::uint8_t kind;
};

#ifndef _WIN32
using SharedBackingResolver = bool (*)(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset);
using SharedBackingWriter = bool (*)(std::uintptr_t address, const void* source, std::size_t bytes);
#endif

extern "C" {

bool GuestArenaAvailable_nid_postfix();
bool GuestArenaContains_nid_postfix(const void* pointer, std::size_t bytes);
void* GuestArenaAllocate_nid_postfix(std::size_t bytes, std::size_t alignment);
void* GuestArenaAllocateAtOrAbove_nid_postfix(std::uintptr_t hint, std::size_t bytes, std::size_t alignment);
void* GuestArenaAllocateFromHint_nid_postfix(std::uintptr_t hint, std::size_t bytes, std::size_t alignment);
std::uintptr_t GuestArenaHighestFree_nid_no_patch(std::size_t bytes, std::size_t alignment);
void GuestArenaMarkUsed_nid_postfix(const void* pointer, std::size_t bytes);
void GuestArenaRelease_nid_postfix(const void* pointer, std::size_t bytes);
// The reserved range, and whether it was reserved with page write watching (Windows MEM_WRITE_WATCH).
void GuestArenaRange_nid_postfix(std::uintptr_t* base, std::size_t* bytes);
bool GuestArenaWriteWatched_nid_postfix();
void GuestArenaSetProtection_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint32_t protection);
void GuestArenaCommit_nid_postfix(void* pointer, std::size_t bytes, std::uint32_t protection, std::size_t granule);
void GuestArenaReset_nid_postfix(void* pointer, std::size_t bytes);
bool GuestArenaBeginMappingExclusion_nid_postfix(std::uint8_t kind);
void GuestArenaEndMappingExclusion_nid_postfix() noexcept;
bool GuestArenaMappingExclusionHeld_nid_postfix();
bool GuestArenaMappingProtected_nid_postfix(std::uintptr_t address, std::size_t bytes);
bool GuestArenaObservePrivateMapping_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint64_t generation, void (*observe)(void*), void* context);
bool GuestArenaObserveSharedMapping_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uint64_t identity, void (*observe)(void*), void* context);
bool GuestArenaCaptureMapping_nid_postfix(std::uintptr_t address, std::size_t bytes, MappingSpan* spans, std::size_t capacity, std::size_t* count);
bool GuestArenaValidateMapping_nid_postfix(const MappingSpan* spans, std::size_t count);
std::uint64_t GuestArenaTryProtectMapping_nid_postfix(const MappingSpan* spans, std::size_t count, std::uintptr_t address, std::size_t bytes, bool (*resolve)(std::uint64_t, void*), void* context, void (*releaseContext)(void*));
#ifdef _WIN32
std::uint64_t GuestArenaTryProtectAccess_nid_postfix(std::uintptr_t address, std::size_t bytes, bool (*resolve)(std::uint64_t token, void* context), void* context, void (*releaseContext)(void* context));
bool GuestArenaResolveAccess_nid_postfix(std::uintptr_t address, std::size_t bytes);
void GuestArenaReleaseAccess_nid_postfix(std::uint64_t token);
bool GuestArenaHandleWrite_nid_postfix(std::uintptr_t address);
bool GuestArenaHandleRead_nid_postfix(std::uintptr_t address);
void GuestArenaPinWritable_nid_postfix(const void* pointer, std::size_t bytes);
void GuestArenaUnpinWritable_nid_postfix(const void* pointer, std::size_t bytes);
bool GuestArenaProtection_nid_postfix(std::uintptr_t address, std::uint32_t* protection);
bool GuestArenaCollectWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear);
bool GuestArenaCollectDriverWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::uint8_t* sources, std::size_t* count, bool clear);
bool GuestArenaCollectArmedWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear);
bool GuestArenaCollectFreshWrites_nid_postfix(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear);
bool GuestArenaHostRegionOverlaps_nid_postfix(std::uintptr_t address, std::size_t bytes);
std::uint64_t GuestArenaCommitGeneration_nid_postfix();
void GuestArenaSetPrivateMappingObserver_nid_postfix(void (*callback)(std::uintptr_t address, std::size_t bytes, std::uint64_t generation));
void GuestArenaSetSharedMappingObserver_nid_postfix(void (*callback)(std::uintptr_t address, std::size_t bytes, std::uint64_t generation, std::uint64_t identity));
void GuestArenaMap_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection);
void GuestArenaMapDirect_nid_postfix(void* pointer, std::size_t bytes, void* section, std::uint64_t offset, std::uint32_t protection);
void* GuestArenaMapAlias_nid_postfix(std::uintptr_t address, std::size_t bytes);
void GuestArenaUnmapAlias_nid_postfix(void* alias);
bool GuestArenaResidentSpan_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uintptr_t* start, std::uintptr_t* end);
void GuestArenaForgetSection_nid_postfix(void* section);
void GuestArenaSetResolveHostWrites_nid_postfix(bool resolve);
bool GuestArenaResolveHostWritesEnabled_nid_postfix();
bool GuestArenaOpenAccessForRead_nid_postfix(std::uint64_t token);
void GuestArenaCopyDriverMemory_nid_postfix(void* destination, const void* source, std::size_t bytes);
void GuestArenaCopyChangedDriverMemory_nid_postfix(void* destination, const void* current, const void* original, std::size_t bytes, std::size_t* firstChanged, std::size_t* lastChanged);
bool GuestArenaCompareDriverMemory_nid_postfix(const void* first, const void* second, std::size_t bytes);
void GuestArenaStoreDriverAtomic64_nid_postfix(std::uintptr_t address, std::uint64_t value);
std::uint64_t GuestArenaLoadDriverAtomic_nid_postfix(std::uintptr_t address, std::size_t bytes);
void GuestArenaSetResolverBypass_nid_postfix(bool (*bypass)());
#else
void GuestArenaSetSharedBacking_nid_postfix(SharedBackingResolver resolver);
void GuestArenaSetSharedBackingWriter_nid_no_patch(SharedBackingWriter writer);
bool GuestArenaWriteSharedBacking_nid_no_patch(std::uintptr_t address, const void* source, std::size_t bytes);
bool GuestArenaSharedBacking_nid_postfix(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset);
#endif
bool GuestArenaTryBackingRuns_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uintptr_t* begins, std::uintptr_t* ends, std::uint64_t* canonicals, std::size_t capacity, std::size_t* count);
std::size_t GuestArenaBackingRuns_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uintptr_t* begins, std::uintptr_t* ends, std::uint64_t* canonicals, std::size_t capacity);
std::size_t GuestArenaAliasRanges_nid_postfix(std::uintptr_t address, std::size_t bytes, std::uintptr_t* begins, std::uintptr_t* ends, std::size_t capacity);
bool GuestArenaBeginHostWrite_nid_postfix(void* pointer, std::size_t bytes);
void GuestArenaEndHostWrite_nid_postfix(void* pointer, std::size_t bytes);

}

class HostWrite {
public:
    HostWrite(void* pointer, std::size_t bytes) : pointer(pointer), bytes(bytes), open(GuestArenaBeginHostWrite_nid_postfix(pointer, bytes)) {}
    ~HostWrite() {
        if (open) GuestArenaEndHostWrite_nid_postfix(pointer, bytes);
    }
    HostWrite(const HostWrite&) = delete;
    HostWrite& operator=(const HostWrite&) = delete;
    bool Open() const { return open; }

private:
    void* pointer;
    std::size_t bytes;
    bool open;
};

}

#endif

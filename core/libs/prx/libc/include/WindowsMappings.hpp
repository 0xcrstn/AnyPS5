#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSMAPPINGS_HPP

#ifdef _WIN32
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <emmintrin.h>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <map>
#include <limits>
#include <memory>
#include <optional>
#include <vector>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace GuestArena {

class WindowsMappings {
public:
    enum class Collection { All, Armed, Fresh };

    bool BeginMappingExclusion(std::uint8_t kind) {
        if (kind > 2 || ExclusionKind() != 0) throw std::runtime_error("nested native mapping exclusion is not implemented");
        exclusionWaiters.fetch_add(1, std::memory_order_relaxed);
        try { mappingExclusion.lock(); }
        catch (...) { exclusionWaiters.fetch_sub(1, std::memory_order_relaxed); throw; }
        exclusionWaiters.fetch_sub(1, std::memory_order_relaxed);
        exclusionKind.store(static_cast<std::uint8_t>(kind + 1), std::memory_order_relaxed);
        exclusionOwner.store(GetCurrentThreadId(), std::memory_order_release);
        return true;
    }

    void EndMappingExclusion() noexcept {
        auto callbacks = std::move(afterExclusion);
        afterExclusion.clear();
        exclusionOwner.store(0, std::memory_order_release);
        mappingExclusion.unlock();
        try { for (auto& callback : callbacks) callback(); }
        catch (const std::exception& error) { std::fprintf(stderr, "native post-mapping notification failed: %s\n", error.what()); std::terminate(); }
        catch (...) { std::fprintf(stderr, "native post-mapping notification failed\n"); std::terminate(); }
    }

    void AfterMappingExclusion(std::function<void()> callback) {
        if (ExclusionKind() != 0) afterExclusion.push_back(std::move(callback));
        else callback();
    }

    std::uint8_t ExclusionKind() const { return exclusionOwner.load(std::memory_order_acquire) == GetCurrentThreadId() ? exclusionKind.load(std::memory_order_relaxed) : 0; }
    std::uint32_t ExclusionWaiters() const { return exclusionWaiters.load(std::memory_order_relaxed); }

    bool MappingProtected(std::uintptr_t address, std::size_t bytes) {
        if (bytes == 0) return false;
        if (address > std::numeric_limits<std::uintptr_t>::max() - pageBytes || bytes > std::numeric_limits<std::uintptr_t>::max() - address - pageBytes) throw std::runtime_error("native mapping exclusion range overflow");
        const auto begin = address & ~(pageBytes - 1);
        const auto end = (address + bytes + pageBytes - 1) & ~(pageBytes - 1);
        std::lock_guard lock(mutex);
        return accessProtectedLocked(begin, end - begin);
    }

    bool ObservePrivateMapping(std::uintptr_t address, std::size_t bytes, std::uint64_t generation, void (*observe)(void*), void* context) {
        if (observe == nullptr || bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address) return false;
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        auto found = privateMappings.upper_bound(address);
        if (found != privateMappings.begin()) --found;
        for (auto cursor = address; cursor < end; ++found) {
            if (found == privateMappings.end() || found->first > cursor || found->second.first <= cursor || found->second.second != generation) return false;
            cursor = std::min(end, found->second.first);
        }
        observe(context);
        return true;
    }

    bool ObserveSharedMapping(std::uintptr_t address, std::size_t bytes, std::uint64_t identity, void (*observe)(void*), void* context) {
        if (observe == nullptr || bytes == 0 || identity == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address) return false;
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        for (auto cursor = address; cursor < end;) {
            const auto found = views.find(cursor & ~(pageBytes - 1));
            if (found == views.end() || found->second.identity != identity || found->second.privateCopy || (found->second.resident && !watchResident)) return false;
            cursor = std::min(end, found->first + pageBytes);
        }
        observe(context);
        return true;
    }

    class MappingExclusion {
    public:
        explicit MappingExclusion(WindowsMappings& mappings, bool guard = false) : mappings(mappings) {
            if (mappings.ExclusionKind() == 2) throw std::runtime_error("a driver store callback cannot change guest mappings or install native guards");
            if (guard && mappings.ExclusionKind() != 0 && mappings.ExclusionKind() != 3) throw std::runtime_error("native guard installation during a mapping mutation is not implemented");
            if (mappings.ExclusionKind() == 0) Lock();
        }
        MappingExclusion(MappingExclusion&& other) noexcept : mappings(other.mappings), owns(std::exchange(other.owns, false)) {}
        ~MappingExclusion() { if (owns) mappings.EndMappingExclusion(); }
        bool Owns() const { return owns; }
        void Lock() { mappings.BeginMappingExclusion(2); owns = true; }
        void Unlock() { mappings.EndMappingExclusion(); owns = false; }

    private:
        WindowsMappings& mappings;
        bool owns = false;
    };

    struct MappingLock {
        MappingExclusion exclusion;
        std::unique_lock<std::mutex> lock;
        explicit MappingLock(WindowsMappings& mappings) : exclusion(mappings), lock(mappings.mutex, std::defer_lock) {}
    };

    std::uint64_t TryProtectAccess(std::uintptr_t address, std::size_t bytes, bool (*resolve)(std::uint64_t, void*), void* context, void (*releaseContext)(void*)) {
        if (resolve == nullptr || bytes == 0 || address % subpageBytes != 0 || bytes % subpageBytes != 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address || (context != nullptr && releaseContext == nullptr)) return 0;
        const MappingExclusion exclusion(*this, true);
        std::lock_guard lock(mutex);
        return installAccessLocked(address, bytes, resolve, context, releaseContext);
    }

    bool CaptureMapping(std::uintptr_t address, std::size_t bytes, MappingSpan* spans, std::size_t capacity, std::size_t* count) {
        if (count == nullptr) return false;
        *count = 0;
        if (bytes == 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address || (capacity != 0 && spans == nullptr)) return false;
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        const auto walk = [&](MappingSpan* output) {
            std::size_t needed = 0;
            MappingSpan previous{};
            for (auto cursor = address; cursor < end;) {
                const auto current = mappingAtLocked(cursor, end);
                if (!current) return std::size_t{0};
                if (needed != 0 && previous.end == current->begin && previous.kind == current->kind && previous.identity == current->identity) {
                    previous.end = current->end;
                    if (output != nullptr) output[needed - 1].end = current->end;
                } else {
                    previous = *current;
                    if (output != nullptr) output[needed] = previous;
                    ++needed;
                }
                cursor = current->end;
            }
            return needed;
        };
        const auto needed = walk(nullptr);
        if (needed == 0) return false;
        if (needed <= capacity) walk(spans);
        *count = needed;
        return true;
    }

    bool ValidateMapping(const MappingSpan* spans, std::size_t count) {
        std::lock_guard lock(mutex);
        return validateMappingLocked(spans, count);
    }

    std::uint64_t TryProtectMapping(const MappingSpan* spans, std::size_t count, std::uintptr_t address, std::size_t bytes, bool (*resolve)(std::uint64_t, void*), void* context, void (*releaseContext)(void*)) {
        const MappingExclusion exclusion(*this, true);
        std::lock_guard lock(mutex);
        if (!validateMappingLocked(spans, count)) throw std::runtime_error("guest output mapping incarnation changed before native protection");
        if (resolve == nullptr || bytes == 0 || address % subpageBytes != 0 || bytes % subpageBytes != 0 || bytes > std::numeric_limits<std::uintptr_t>::max() - address || (context != nullptr && releaseContext == nullptr)) return 0;
        return installAccessLocked(address, bytes, resolve, context, releaseContext);
    }

private:
    std::uint64_t installAccessLocked(std::uintptr_t address, std::size_t bytes, bool (*resolve)(std::uint64_t, void*), void* context, void (*releaseContext)(void*)) {
        auto guard = std::make_shared<AccessGuard>();
        guard->resolve = resolve;
        guard->context = context;
        if (compare == nullptr) return 0;
        for (auto cursor = address; cursor < address + bytes; cursor += subpageBytes) {
            const auto base = cursor & ~(pageBytes - 1);
            const auto found = views.find(base);
            if (found == views.end() || found->second.resident || found->second.privateCopy || found->second.page->pins != 0 || !writable(found->second.protection)) return 0;
            const auto page = found->second.page;
            if (!ensureDriverAlias(found->second)) return 0;
            for (const auto alias : page->aliases) {
                const auto& view = views.at(alias);
                const auto protection = view.protection & 0xffu;
                if (view.resident || view.privateCopy || view.offset != found->second.offset || view.section != found->second.section || view.hostWrites != 0 || (view.protection & PAGE_GUARD) != 0 || (protection != PAGE_READONLY && protection != PAGE_READWRITE)) return 0;
            }
            const auto mask = subpagesOf(base, cursor, cursor + subpageBytes);
            const auto existing = std::find_if(guard->pages.begin(), guard->pages.end(), [&](const ProtectedPage& held) { return held.page == page; });
            if (existing == guard->pages.end()) guard->pages.push_back({page, mask});
            else existing->mask |= mask;
        }
        if (nextAccessToken == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("guest access protection tokens exhausted");
        const auto token = ++nextAccessToken;
        accessGuards.emplace(token, guard);
        for (const auto& held : guard->pages) {
            for (std::size_t sub = 0; sub < subpages; ++sub) {
                if ((held.mask & (1u << sub)) != 0) ++held.page->accessGuards[sub];
            }
        }
        try {
            for (const auto& held : guard->pages) {
                for (const auto alias : held.page->aliases) protectAccess(alias, views.at(alias), held.mask);
            }
        } catch (...) {
            releaseAccessLocked(token, *guard);
            throw;
        }
        guard->releaseContext = releaseContext;
        accessGuardCount.store(accessGuards.size(), std::memory_order_release);
        return token;
    }

public:
    bool OpenAccessForRead(std::uint64_t token) {
        std::lock_guard lock(mutex);
        const auto found = accessGuards.find(token);
        if (found == accessGuards.end()) return false;
        auto& guard = *found->second;
        if (guard.opened) return true;
        guard.opened = true;
        for (const auto& held : guard.pages) {
            for (std::size_t sub = 0; sub < subpages; ++sub) {
                if ((held.mask & (1u << sub)) != 0) ++held.page->readableGuards[sub];
            }
        }
        for (const auto& held : guard.pages) {
            for (const auto alias : held.page->aliases) protectAccess(alias, views.at(alias), held.mask);
        }
        return true;
    }

    void CopyDriverMemory(void* destination, const void* source, std::size_t bytes) {
        std::lock_guard lock(mutex);
        auto target = reinterpret_cast<std::uintptr_t>(destination);
        auto input = reinterpret_cast<std::uintptr_t>(source);
        while (bytes != 0) {
            const auto length = std::min({bytes, pageBytes - target % pageBytes, pageBytes - input % pageBytes});
            auto* out = driverPointer(target, true);
            const auto* in = driverPointer(input, false);
            std::memcpy(out, in, length);
            noteDriverWrite(target, length);
            target += length;
            input += length;
            bytes -= length;
        }
    }

    void CopyChangedDriverMemory(void* destination, const void* current, const void* original, std::size_t bytes, std::size_t* firstChanged, std::size_t* lastChanged) {
        std::lock_guard lock(mutex);
        *firstChanged = bytes;
        *lastChanged = 0;
        auto target = reinterpret_cast<std::uintptr_t>(destination);
        auto input = reinterpret_cast<std::uintptr_t>(current);
        auto baseline = reinterpret_cast<std::uintptr_t>(original);
        std::size_t offset = 0;
        while (offset < bytes) {
            const auto length = std::min({bytes - offset, pageBytes - target % pageBytes, pageBytes - input % pageBytes, pageBytes - baseline % pageBytes});
            const auto* now = driverPointer(input, false);
            const auto* before = driverPointer(baseline, false);
            auto* out = driverPointer(target, true);
            std::size_t at = 0;
            bool changed = false;
            for (; at + 16 <= length; at += 16) {
                const auto values = _mm_loadu_si128(reinterpret_cast<const __m128i*>(now + at));
                const auto same = _mm_cmpeq_epi8(values, _mm_loadu_si128(reinterpret_cast<const __m128i*>(before + at)));
                const auto equal = static_cast<std::uint32_t>(_mm_movemask_epi8(same));
                if (equal == 0xffffu) continue;
                if (equal == 0) _mm_storeu_si128(reinterpret_cast<__m128i*>(out + at), values);
                else _mm_maskmoveu_si128(values, _mm_xor_si128(same, _mm_set1_epi8(-1)), reinterpret_cast<char*>(out + at));
                const auto differing = ~equal & 0xffffu;
                *firstChanged = std::min(*firstChanged, offset + at + static_cast<std::size_t>(std::countr_zero(differing)));
                *lastChanged = offset + at + 32 - static_cast<std::size_t>(std::countl_zero(differing));
                changed = true;
            }
            _mm_sfence();
            for (; at < length; ++at) {
                if (now[at] == before[at]) continue;
                out[at] = now[at];
                *firstChanged = std::min(*firstChanged, offset + at);
                *lastChanged = offset + at + 1;
                changed = true;
            }
            if (changed) noteDriverWrite(target, length);
            offset += length;
            target += length;
            input += length;
            baseline += length;
        }
    }

    bool CompareDriverMemory(const void* first, const void* second, std::size_t bytes) {
        std::lock_guard lock(mutex);
        auto left = reinterpret_cast<std::uintptr_t>(first);
        auto right = reinterpret_cast<std::uintptr_t>(second);
        while (bytes != 0) {
            const auto length = std::min({bytes, pageBytes - left % pageBytes, pageBytes - right % pageBytes});
            if (std::memcmp(driverPointer(left, false), driverPointer(right, false), length) != 0) return false;
            left += length;
            right += length;
            bytes -= length;
        }
        return true;
    }

    void StoreDriverAtomic64(std::uintptr_t address, std::uint64_t value) {
        if (address % std::atomic_ref<std::uint64_t>::required_alignment != 0) throw std::runtime_error("unaligned driver atomic store");
        std::lock_guard lock(mutex);
        auto* target = reinterpret_cast<std::uint64_t*>(driverPointer(address, true));
        std::atomic_ref(*target).store(value, std::memory_order_release);
        noteDriverWrite(address, sizeof(value));
    }

    std::uint64_t LoadDriverAtomic(std::uintptr_t address, std::size_t bytes) {
        if ((bytes != 4 && bytes != 8) || address % bytes != 0) throw std::runtime_error("invalid driver atomic load");
        std::lock_guard lock(mutex);
        auto* source = driverPointer(address, false);
        if (bytes == 4) return std::atomic_ref(*reinterpret_cast<std::uint32_t*>(source)).load(std::memory_order_acquire);
        return std::atomic_ref(*reinterpret_cast<std::uint64_t*>(source)).load(std::memory_order_acquire);
    }

    void ReleaseAccess(std::uint64_t token) {
        std::shared_ptr<AccessGuard> released;
        {
            std::lock_guard lock(mutex);
            const auto found = accessGuards.find(token);
            if (found == accessGuards.end()) return;
            released = found->second;
            releaseAccessLocked(token, *released);
        }
    }

    bool ResolveAccess(std::uintptr_t address, std::size_t bytes) {
        if (ExclusionKind() == 2) throw std::runtime_error("a driver store callback cannot resolve guarded guest memory while holding the write tracker");
        if (ExclusionKind() != 0) throw std::runtime_error("native access resolution requires releasing the mapping exclusion and allocation registry");
        if (!activeAccessResolvers().empty()) {
            const auto bypass = resolverBypass.load(std::memory_order_acquire);
            if (bypass == nullptr || !bypass()) throw std::runtime_error("nested guest access resolution is not implemented");
        }
        if (bytes == 0) return true;
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - address) return false;
        if (accessGuardCount.load(std::memory_order_acquire) == 0) return true;
        std::vector<ViewIdentity> identities;
        {
            std::lock_guard lock(mutex);
            for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) identities.push_back({it->first, it->second.page, it->second.identity});
        }
        for (;;) {
            std::vector<std::pair<std::uint64_t, std::shared_ptr<AccessGuard>>> pending;
            {
                std::lock_guard lock(mutex);
                auto current = views.lower_bound(address & ~(pageBytes - 1));
                for (const auto& [base, page, identity] : identities) {
                    if (current == views.end() || current->first != base || current->second.page != page || current->second.identity != identity) return false;
                    ++current;
                    const auto mask = subpagesOf(base, address, address + bytes);
                    for (const auto& [token, guard] : accessGuards) {
                        if (std::any_of(guard->pages.begin(), guard->pages.end(), [&](const ProtectedPage& held) { return held.page == page && (held.mask & mask) != 0; }) && std::none_of(pending.begin(), pending.end(), [&](const auto& held) { return held.first == token; })) pending.emplace_back(token, guard);
                    }
                }
                if (current != views.end() && current->first < address + bytes) return false;
            }
            if (pending.empty()) return true;
            for (const auto& [token, guard] : pending) {
                std::unique_lock resolving(guard->resolverMutex, std::defer_lock);
                if (!resolving.try_lock()) {
                    const auto bypass = resolverBypass.load(std::memory_order_acquire);
                    if (bypass == nullptr || !bypass()) resolving.lock();
                }
                {
                    std::lock_guard lock(mutex);
                    if (!accessGuards.contains(token)) continue;
                }
                if (std::find(activeAccessResolvers().begin(), activeAccessResolvers().end(), token) != activeAccessResolvers().end()) throw std::runtime_error("a guest access resolution reached its own guarded pages for writing");
                struct Resolving {
                    explicit Resolving(std::uint64_t token) { activeAccessResolvers().push_back(token); }
                    ~Resolving() { activeAccessResolvers().pop_back(); }
                } resolvingScope(token);
                if (!guard->resolve(token, guard->context)) throw std::runtime_error("guest access resolver refused publication");
                std::lock_guard lock(mutex);
                if (accessGuards.contains(token)) throw std::runtime_error("guest access resolver retained its protection");
            }
        }
    }

    static WindowsMappings& Get() {
        static auto* mappings = new WindowsMappings;
        return *mappings;
    }

    void* Reserve(void* address, std::size_t bytes) {
        return allocate(GetCurrentProcess(), address, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    }

    std::vector<std::pair<std::uintptr_t, std::size_t>> Commit(void* address, std::size_t bytes, DWORD protection, std::size_t granule, bool watched, std::uint64_t generation = 0) {
        std::vector<std::pair<std::uintptr_t, std::size_t>> created;
        const auto lock = lockResolved(reinterpret_cast<std::uintptr_t>(address), bytes);
        requireAccessResolved(reinterpret_cast<std::uintptr_t>(address), bytes, true);
        collectableRanges.clear();
        const auto end = reinterpret_cast<std::uintptr_t>(address) + bytes;
        for (auto cursor = reinterpret_cast<std::uintptr_t>(address); cursor < end;) {
            const auto memory = query(cursor);
            const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
            if (memory.State == MEM_RESERVE) {
                const auto limit = std::min(end, cursor + granule);
                auto placeholderEnd = stop;
                while (placeholderEnd < limit) {
                    const auto next = query(placeholderEnd);
                    if (next.State != MEM_RESERVE) break;
                    placeholderEnd = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                }
                const auto size = std::min(limit, placeholderEnd) - cursor;
                reset(cursor, size);
                const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (watched ? MEM_WRITE_WATCH : 0);
                if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(cursor), size, flags, protection, nullptr, 0)) fail("replace guest placeholder with private memory");
                privateMappings.emplace(cursor, std::pair{cursor + size, generation});
                if (!created.empty() && created.back().first + created.back().second == cursor) created.back().second += size;
                else created.emplace_back(cursor, size);
                cursor += size;
            } else {
                if (memory.State != MEM_COMMIT) throw std::runtime_error("guest memory is not committed");
                const auto mapped = views.find(cursor & ~(pageBytes - 1));
                if (mapped != views.end()) {
                    mapped->second.protection = protection;
                    mapped->second.privateCopy = mapped->second.privateCopy || copyProtection(protection);
                    mapped->second.armed = 0;
                    invalidate(*mapped->second.page);
                }
                DWORD previous;
                if (!VirtualProtect(reinterpret_cast<void*>(cursor), stop - cursor, protection, &previous)) fail("protect guest memory");
                cursor = stop;
            }
        }
        return created;
    }

    void Reset(void* address, std::size_t bytes) {
        const auto lock = lockResolved(reinterpret_cast<std::uintptr_t>(address), bytes);
        reset(reinterpret_cast<std::uintptr_t>(address), bytes);
    }

    std::pair<std::uint64_t, std::uint64_t> Map(void* address, std::size_t bytes, HANDLE section, std::uint64_t offset, DWORD protection, bool watched = false, bool resident = false, std::atomic<std::uint64_t>* generation = nullptr) {
        if (offset % pageBytes != 0) throw std::runtime_error("shared guest mapping offset is not 16 KiB aligned");
        if (compare == nullptr) throw std::runtime_error("shared guest mappings require Windows section identity support");
        const auto lock = lockResolved(reinterpret_cast<std::uintptr_t>(address), bytes);
        const auto owned = sectionFor(section);
        if (!accessGuards.empty()) {
            for (const auto& [token, guard] : accessGuards) {
                static_cast<void>(token);
                for (const auto& held : guard->pages) {
                    const auto& view = views.at(held.page->aliases.front());
                    if (view.offset >= offset + bytes || offset >= view.offset + pageBytes) continue;
                    const auto existing = owned->physical.find(view.offset);
                    const bool sharesSlot = existing != owned->physical.end() && existing->second->page.lock() == held.page;
                    if (sameSection(view.section->handle, section) != sharesSlot) throw std::runtime_error("mapping protected backing with a different section identity is not implemented");
                    if (sharesSlot && protection != PAGE_READONLY && protection != PAGE_READWRITE) throw std::runtime_error("mapping protected backing with unsupported CPU protection is not implemented");
                }
            }
        }
        collectableRanges.clear();
        auto cursor = reinterpret_cast<std::uintptr_t>(address);
        if (viewIdentity == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("guest mapping identities exhausted");
        const auto identity = ++viewIdentity;
        const auto observedGeneration = generation != nullptr ? generation->fetch_add(1, std::memory_order_acq_rel) + 1 : 0;
        reset(cursor, bytes);
        if (resident) watchResident = watched;
        if (compare != nullptr && offset % pageBytes == 0) {
            for (std::size_t done = 0; done < bytes; done += pageBytes) {
                if (!owned->pageIds.contains(offset + done)) owned->pageIds.emplace(offset + done, nextSharedPageSerial());
            }
        }
        const auto slotOf = [&](std::size_t done) -> const std::shared_ptr<Physical>& {
            auto& slot = owned->physical[offset + done];
            if (!slot) slot = std::make_shared<Physical>();
            return slot;
        };
        const auto pageOf = [](Physical& slot) {
            auto shared = slot.page.lock();
            if (!shared) {
                shared = std::make_shared<SharedPage>();
                slot.page = shared;
            }
            return shared;
        };
        const auto single = [&](std::size_t done) {
            const auto& slot = *slotOf(done);
            const auto shared = slot.page.lock();
            return resident && !slot.stored && (!shared || shared->aliases.empty());
        };
        for (std::size_t done = 0; done < bytes;) {
            const auto base = cursor + done;
            if (single(done)) {
                auto runBytes = pageBytes;
                while (done + runBytes < bytes && (base + runBytes) % chunkBytes != 0 && single(done + runBytes)) runBytes += pageBytes;
                split(base, runBytes);
                commitResident(base, runBytes);
                residentRuns.emplace(base, Run{base + runBytes, {}, static_cast<std::uint32_t>(runBytes >> 12)});
                for (std::size_t at = 0; at < runBytes; at += pageBytes) {
                    const auto slot = slotOf(done + at);
                    const auto shared = pageOf(*slot);
                    if (slot->saved) {
                        std::memcpy(reinterpret_cast<void*>(base + at), slot->saved->bytes + slot->savedOffset, pageBytes);
                        slot->saved.reset();
                    }
                    shared->aliases.push_back(base + at);
                    if (shared->aliases.size() == 2) multiAliasPages.fetch_add(1, std::memory_order_acq_rel);
                    const auto added = views.emplace(base + at, View{shared, protection, {}, 0, owned, offset + done + at, 0, 0, true, slot}).first;
                    added->second.identity = identity;
                    added->second.privateCopy = copyProtection(protection);
                    if (const auto id = owned->pageIds.find(offset + done + at); id != owned->pageIds.end()) added->second.backingSerial = id->second;
                    invalidate(*shared);
                }
                remember(freshRanges, base, base + runBytes);
                DWORD previous;
                if (protection != PAGE_READWRITE && !VirtualProtect(reinterpret_cast<void*>(base), runBytes, protection, &previous)) fail("protect resident guest pages");
                done += runBytes;
                continue;
            }
            const auto slot = slotOf(done);
            const auto shared = pageOf(*slot);
            const auto aliases = shared->aliases;
            for (const auto alias : aliases) {
                if (views.at(alias).resident) demote(alias);
            }
            split(base, pageBytes);
            const auto guarded = protectedAccess(*shared);
            mapShared(base, section, offset + done, guarded != 0 ? PAGE_NOACCESS : protection);
            slot->stored = true;
            shared->aliases.push_back(base);
            if (shared->aliases.size() == 2) multiAliasPages.fetch_add(1, std::memory_order_acq_rel);
            const auto added = views.emplace(base, View{shared, protection, {}, 0, owned, offset + done, 0, 0, false, slot}).first;
            added->second.identity = identity;
            added->second.privateCopy = copyProtection(protection);
            if (const auto id = owned->pageIds.find(offset + done); id != owned->pageIds.end()) added->second.backingSerial = id->second;
            if (guarded != 0) protectAccess(base, added->second, allSubpages);
            remember(freshRanges, base, base + pageBytes);
            invalidate(*shared);
            done += pageBytes;
        }
        return {identity, observedGeneration};
    }

    void SetProtection(std::uintptr_t address, std::size_t bytes, DWORD protection) {
        const auto lock = lockResolved(address, bytes);
        requireAccessResolved(address, bytes, true);
        collectableRanges.clear();
        for (auto it = views.lower_bound(address); it != views.end() && it->first < address + bytes; ++it) {
            it->second.protection = protection;
            it->second.privateCopy = it->second.privateCopy || copyProtection(protection);
            it->second.armed = 0;
            invalidate(*it->second.page);
        }
    }

    void Pin(std::uintptr_t address, std::size_t bytes) {
        const auto lock = lockResolved(address, bytes);
        requireAccessResolved(address, bytes, true);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            ++page.pins;
            for (const auto alias : page.aliases) {
                auto& view = views.at(alias);
                if (view.armed == 0) continue;
                protectSubpages(alias, view.armed, view.protection, "pin shared guest page writable");
                view.armed = 0;
            }
            invalidate(page);
        }
    }

    void Unpin(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            auto& page = *it->second.page;
            if (page.pins != 0) --page.pins;
            invalidate(page);
        }
    }

    bool HandleWrite(std::uintptr_t address) {
        std::shared_ptr<SharedPage> identity;
        std::uint64_t mappingIdentity = 0;
        for (;;) {
            std::unique_lock lock(mutex);
            const auto base = address & ~(pageBytes - 1);
            const auto found = views.find(base);
            if (found == views.end() || (identity != nullptr && (found->second.page != identity || found->second.identity != mappingIdentity)) || !writable(found->second.protection)) return false;
            auto& view = found->second;
            if ((protectedAccess(*view.page) & subpagesOf(base, address, address + 1)) != 0) {
                if (ExclusionKind() != 0) {
                    std::fprintf(stderr, "guest native write reached guarded memory during a mapping or driver-store exclusion\n");
                    return false;
                }
                identity = view.page;
                mappingIdentity = view.identity;
                lock.unlock();
                if (!ResolveAccess(address, 1)) return false;
                continue;
            }
            if (view.resident) {
                const auto memory = query(address);
                return memory.State == MEM_COMMIT && writable(memory.Protect & 0xffu);
            }
            const auto written = subpagesOf(base, address, address + 1);
            invalidate(*view.page, written, WriteProvenance::Cpu);
            protectSubpages(base, written, view.protection, "resume shared memory write");
            view.armed &= static_cast<std::uint8_t>(~written);
            return true;
        }
    }

    bool HandleRead(std::uintptr_t address) {
        std::shared_ptr<SharedPage> identity;
        std::uint64_t mappingIdentity = 0;
        for (;;) {
            std::unique_lock lock(mutex);
            const auto base = address & ~(pageBytes - 1);
            const auto found = views.find(base);
            if (found == views.end() || (identity != nullptr && (found->second.page != identity || found->second.identity != mappingIdentity))) return false;
            if ((protectedAccess(*found->second.page) & subpagesOf(base, address, address + 1)) != 0) {
                if (ExclusionKind() != 0) {
                    std::fprintf(stderr, "guest native read reached guarded memory during a mapping or driver-store exclusion\n");
                    return false;
                }
                identity = found->second.page;
                mappingIdentity = found->second.identity;
                lock.unlock();
                if (!ResolveAccess(address, 1)) return false;
                continue;
            }
            const auto memory = query(address);
            return memory.State == MEM_COMMIT && (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
        }
    }

    std::size_t BackingRuns(std::uintptr_t address, std::size_t bytes, std::uintptr_t* begins, std::uintptr_t* ends, std::uint64_t* canonicals, std::size_t capacity) {
        constexpr std::uint64_t canonicalSpace = std::uint64_t{1} << 63;
        if (bytes == 0) return 0;
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - address) throw std::runtime_error("guest backing range wraps its address");
        std::lock_guard lock(mutex);
        std::size_t count = 0;
        std::uintptr_t lastBegin = 0, lastEnd = 0;
        std::uint64_t lastCanonical = 0;
        const auto emit = [&](std::uintptr_t begin, std::uintptr_t end, std::uint64_t canonical) {
            if (count != 0 && lastEnd == begin && lastCanonical + (lastEnd - lastBegin) == canonical) {
                lastEnd = end;
                if (count <= capacity) ends[count - 1] = end;
                return;
            }
            lastBegin = begin;
            lastEnd = end;
            lastCanonical = canonical;
            if (count < capacity) {
                begins[count] = begin;
                ends[count] = end;
                canonicals[count] = canonical;
            }
            ++count;
        };
        const auto stop = address + bytes;
        for (auto cursor = address; cursor < stop;) {
            const auto base = cursor & ~(pageBytes - 1);
            const auto end = std::min<std::uintptr_t>(stop, base + pageBytes);
            const auto found = views.find(base);
            if (found == views.end() || found->second.page == nullptr) emit(cursor, end, cursor);
            else emit(cursor, end, canonicalSpace | (found->second.page->serial * pageBytes + (cursor - base)));
            cursor = end;
        }
        return count;
    }

    bool TryBackingRuns(std::uintptr_t address, std::size_t bytes, std::uintptr_t* begins, std::uintptr_t* ends, std::uint64_t* canonicals, std::size_t capacity, std::size_t* count) {
        if (count == nullptr) return false;
        *count = 0;
        if (bytes == 0) return true;
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - address || (capacity != 0 && (begins == nullptr || ends == nullptr || canonicals == nullptr))) return false;
        std::lock_guard lock(mutex);
        if (compare == nullptr) return false;
        constexpr std::uint64_t canonicalSpace = std::uint64_t{1} << 63;
        std::uintptr_t lastBegin = 0, lastEnd = 0;
        std::uint64_t lastCanonical = 0;
        const auto stop = address + bytes;
        auto found = views.lower_bound(address & ~(pageBytes - 1));
        for (auto cursor = address; cursor < stop;) {
            const auto base = cursor & ~(pageBytes - 1);
            const auto end = std::min<std::uintptr_t>(stop, base + pageBytes);
            if (found == views.end() || found->first != base || found->second.privateCopy || found->second.backingSerial == 0) {
                *count = 0;
                return false;
            }
            const auto canonical = canonicalSpace | (found->second.backingSerial * pageBytes + (cursor - base));
            if (*count != 0 && lastEnd == cursor && lastCanonical + (lastEnd - lastBegin) == canonical) {
                lastEnd = end;
                if (*count <= capacity) ends[*count - 1] = end;
            } else {
                if (*count < capacity) {
                    begins[*count] = cursor;
                    ends[*count] = end;
                    canonicals[*count] = canonical;
                }
                ++*count;
                lastBegin = cursor;
                lastEnd = end;
                lastCanonical = canonical;
            }
            ++found;
            cursor = end;
        }
        return true;
    }

    bool ResidentSpan(std::uintptr_t address, std::size_t bytes, std::uintptr_t* start, std::uintptr_t* end) {
        std::lock_guard lock(mutex);
        auto run = residentRuns.upper_bound(address);
        if (run != residentRuns.begin() && std::prev(run)->second.end > address) --run;
        if (run == residentRuns.end() || run->first >= address + bytes) return false;
        *start = run->first;
        for (; run != residentRuns.end() && run->first < address + bytes; ++run) *end = run->second.end;
        return true;
    }

    void ForgetSection(HANDLE section) {
        std::lock_guard lock(mutex);
        const auto key = reinterpret_cast<std::uintptr_t>(section);
        sections.erase(key);
    }

    void SetResolverBypass(bool (*bypass)()) {
        resolverBypass.store(bypass, std::memory_order_release);
    }

    void SetResolveHostWrites(bool resolve) {
        resolveHostWrites.store(resolve, std::memory_order_release);
    }

    bool ResolveHostWritesEnabled() const {
        return resolveHostWrites.load(std::memory_order_acquire);
    }

    std::size_t AliasRanges(std::uintptr_t address, std::size_t bytes, std::uintptr_t* begins, std::uintptr_t* ends, std::size_t capacity) {
        if (bytes == 0 || multiAliasPages.load(std::memory_order_acquire) == 0) return 0;
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - address) throw std::runtime_error("guest alias range wraps its address");
        std::lock_guard lock(mutex);
        std::size_t count = 0;
        const auto emit = [&](std::uintptr_t begin, std::uintptr_t end) {
            if (count != 0 && count <= capacity && ends[count - 1] == begin) {
                ends[count - 1] = end;
                return;
            }
            if (count < capacity) {
                begins[count] = begin;
                ends[count] = end;
            }
            ++count;
        };
        const auto stop = address + bytes;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < stop; ++it) {
            const auto& aliases = it->second.page->aliases;
            if (aliases.size() < 2) continue;
            const auto from = std::max<std::uintptr_t>(address, it->first) - it->first;
            const auto to = std::min<std::uintptr_t>(stop, it->first + pageBytes) - it->first;
            for (const auto alias : aliases) {
                if (alias != it->first) emit(alias + from, alias + to);
            }
        }
        return count;
    }

    bool BeginHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::unique_lock lock(mutex, std::defer_lock);
        if (resolveHostWrites.load(std::memory_order_acquire)) {
            for (;;) {
                if (!ResolveAccess(address, bytes)) return false;
                lock.lock();
                if (!accessProtectedLocked(address, bytes)) break;
                lock.unlock();
            }
        } else {
            lock.lock();
        }
        requireAccessResolved(address, bytes);
        const auto first = views.lower_bound(address & ~(pageBytes - 1));
        const auto end = address + bytes;
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            if (!writable(it->second.protection)) return false;
        }
        for (auto it = first; it != views.end() && it->first < end; ++it) {
            auto& view = it->second;
            ++view.hostWrites;
            const auto written = subpagesOf(it->first, address, end);
            invalidate(*view.page, written);
            const auto opened = static_cast<std::uint8_t>(view.armed & written);
            if (opened == 0) continue;
            protectSubpages(it->first, opened, view.protection, "open shared memory to a host write");
            view.armed &= static_cast<std::uint8_t>(~opened);
        }
        return true;
    }

    void EndHostWrite(std::uintptr_t address, std::size_t bytes) {
        std::lock_guard lock(mutex);
        const auto end = address + bytes;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
            --it->second.hostWrites;
            invalidate(*it->second.page, subpagesOf(it->first, address, end));
        }
    }

    void* MapAlias(std::uintptr_t address, std::size_t bytes) {
        const auto lock = lockResolved(address, bytes);
        const auto refuse = [&](const char* reason) {
            char text[192];
            std::snprintf(text, sizeof(text), "read-write alias of shared guest memory 0x%llx+0x%llx: %s", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), reason);
            return std::runtime_error(text);
        };
        if (address % pageBytes != 0 || bytes % pageBytes != 0 || bytes == 0) throw refuse("the range is not made of whole shared pages");
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            const auto found = views.find(address + done);
            if (found == views.end() || !found->second.resident) continue;
            const auto run = std::prev(residentRuns.upper_bound(address + done));
            if (run->first < address || run->second.end > address + bytes) throw refuse("a resident run of direct memory extends outside the range");
        }
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            const auto found = views.find(address + done);
            if (found != views.end() && found->second.resident) demote(address + done);
        }
        auto view = views.find(address);
        if (view == views.end()) throw refuse("the range does not start at a shared view");
        const auto section = view->second.section;
        const auto offset = view->second.offset;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        bool contiguous = true;
        for (std::size_t done = 0; done < bytes; done += pageBytes) {
            const auto page = views.find(address + done);
            if (page == views.end()) throw refuse("the range is not a run of shared views");
            if (page->second.offset != offset + done || (page->second.section != section && !sameSection(page->second.section->handle, section->handle))) contiguous = false;
        }
        if (!contiguous) return mapAliasRuns(address, bytes);
        const auto lead = offset % system.dwAllocationGranularity;
        void* alias = map(section->handle, GetCurrentProcess(), nullptr, offset - lead, lead + bytes, 0, PAGE_READWRITE, nullptr, 0);
        if (alias == nullptr) {
            char text[160];
            std::snprintf(text, sizeof(text), "MapViewOfFile3 of a read-write alias of shared guest memory 0x%llx+0x%llx", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), text);
        }
        return static_cast<char*>(alias) + lead;
    }

    void UnmapAlias(void* alias) {
        if (alias == nullptr) return;
        {
            std::lock_guard lock(mutex);
            if (const auto found = splitAliases.find(reinterpret_cast<std::uintptr_t>(alias)); found != splitAliases.end()) {
                const auto error = releaseAlias(found->first, found->second);
                splitAliases.erase(found);
                if (error != ERROR_SUCCESS) throw std::system_error(static_cast<int>(error), std::system_category(), "unmap a run of a shared guest alias");
                return;
            }
        }
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto base = reinterpret_cast<std::uintptr_t>(alias) & ~(static_cast<std::uintptr_t>(system.dwAllocationGranularity) - 1);
        if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(base), 0)) fail("unmap shared guest alias");
    }

    bool Protection(std::uintptr_t address, std::uint32_t* protection) {
        std::lock_guard lock(mutex);
        const auto found = views.find(address & ~(pageBytes - 1));
        if (found == views.end()) return false;
        *protection = found->second.protection;
        return true;
    }

    bool Collect(std::uintptr_t address, std::size_t bytes, void** pages, std::size_t* count, bool clear, Collection collection = Collection::All, std::uint8_t* sources = nullptr) {
        std::lock_guard lock(mutex);
        const auto capacity = *count;
        *count = 0;
        const auto end = address + bytes;
        if (collection == Collection::Fresh) {
            const auto fresh = freshRanges.upper_bound(address);
            if ((fresh == freshRanges.begin() || std::prev(fresh)->second <= address) && (fresh == freshRanges.end() || fresh->first >= end)) return true;
            for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < end; ++it) {
                if (it->second.resident) {
                    if (!watchResident) return false;
                    continue;
                }
                if (it->second.protection != PAGE_NOACCESS) continue;
                for (auto at = std::max(address, it->first); at < std::min(end, it->first + pageBytes); at += 4096) {
                    if ((it->second.collected & (1u << ((at - it->first) / 4096))) == 0) return false;
                }
            }
        } else if (clear && !collectable(address, end)) return false;
        auto position = views.lower_bound(address & ~(pageBytes - 1));
        std::uintptr_t cleanStart = 0;
        std::uintptr_t cleanEnd = 0;
        const auto flushClean = [&] {
            if (cleanEnd > cleanStart) rememberClean(cleanStart, cleanEnd);
            cleanStart = cleanEnd = 0;
        };
        for (auto cursor = address; cursor < end;) {
            const auto nextClean = cleanRanges.upper_bound(cursor);
            if (nextClean != cleanRanges.begin()) {
                const auto clean = std::prev(nextClean);
                if (cursor < clean->second) {
                    cursor = std::min(end, clean->second);
                    continue;
                }
            }
            const auto base = cursor & ~(pageBytes - 1);
            const auto found = viewAt(base, position);
            if (found != views.end() && found->second.resident) {
                flushClean();
                if (!watchResident) return false;
                const auto available = capacity - *count;
                if (available == 0) return true;
                const auto run = std::prev(residentRuns.upper_bound(cursor));
                const auto stop = std::min({end, run->second.end, (cursor & ~std::uintptr_t{4095}) + available * 4096});
                const auto before = *count;
                collectResident(run->first, run->second, cursor, stop, pages, count, clear, collection);
                if (sources != nullptr) std::fill(sources + before, sources + *count, std::uint8_t{2});
                if (*count == capacity) return true;
                cursor = stop;
            } else if (found != views.end()) {
                auto& view = found->second;
                auto stop = std::min(std::min(end, base + pageBytes), nextClean == cleanRanges.end() ? end : nextClean->first);
                if (collection != Collection::All) {
                    const auto selected = [&](std::uintptr_t at) {
                        const bool collected = (view.collected & (1u << ((at - base) / 4096))) != 0;
                        return collection == Collection::Armed ? collected : !collected;
                    };
                    while (cursor < stop && !selected(cursor)) cursor = std::min(stop, cursor + 4096);
                    if (cursor == stop) continue;
                    for (auto at = cursor + 4096; at < stop; at += 4096) {
                        if (!selected(at)) {
                            stop = at;
                            break;
                        }
                    }
                }
                if (view.protection == PAGE_NOACCESS) {
                    flushClean();
                    return false;
                }
                const bool pinned = view.page->pins != 0;
                std::uint8_t settled = 0;
                auto settledEnd = cursor;
                bool full = false;
                for (std::size_t sub = 0; sub < subpages; ++sub) {
                    const auto from = std::max(cursor, base + sub * subpageBytes);
                    const auto to = std::min(stop, base + (sub + 1) * subpageBytes);
                    if (from >= to) continue;
                    if (collection == Collection::Fresh || pinned || view.seen[sub] != view.page->generation[sub]) {
                        if (*count == capacity) {
                            full = true;
                            break;
                        }
                        if (sources != nullptr) sources[*count] = collectedProvenance(view, sub);
                        pages[(*count)++] = reinterpret_cast<void*>(from);
                    }
                    settled |= static_cast<std::uint8_t>(1u << sub);
                    settledEnd = to;
                }
                if (clear) {
                    for (std::size_t sub = 0; sub < subpages; ++sub) {
                        const auto bit = static_cast<std::uint8_t>(1u << sub);
                        if ((settled & bit) == 0 || (view.collected & bit) != 0) continue;
                        view.collected |= bit;
                        forget(freshRanges, base + sub * subpageBytes, base + (sub + 1) * subpageBytes);
                    }
                }
                if (clear && !pinned && settled != 0) {
                    std::uint8_t untracked = 0;
                    for (const auto alias : view.page->aliases) {
                        auto& other = views.at(alias);
                        if (other.resident) continue;
                        const auto arming = static_cast<std::uint8_t>(settled & ~other.armed);
                        if (!writable(other.protection) || arming == 0) continue;
                        if (other.hostWrites != 0) {
                            untracked |= arming;
                            continue;
                        }
                        const DWORD protection = other.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
                        protectSubpages(alias, static_cast<std::uint8_t>(arming & ~protectedAccess(*other.page)), protection, "arm shared memory write tracking");
                        other.armed |= arming;
                    }
                    const auto tracked = static_cast<std::uint8_t>(settled & ~untracked);
                    for (std::size_t sub = 0; sub < subpages; ++sub) {
                        if ((tracked & (1u << sub)) == 0) continue;
                        const auto from = std::max(cursor, base + sub * subpageBytes);
                        const auto to = std::min(settledEnd, base + (sub + 1) * subpageBytes);
                        if (from == base + sub * subpageBytes && to == base + (sub + 1) * subpageBytes) {
                            view.seen[sub] = view.page->generation[sub];
                            view.nativeCollected[sub] = view.page->nativeGeneration[sub];
                            view.nativeBaseline |= static_cast<std::uint8_t>(1u << sub);
                        }
                        if (from != cleanEnd) {
                            flushClean();
                            cleanStart = from;
                        }
                        cleanEnd = to;
                    }
                }
                if (full) {
                    flushClean();
                    return true;
                }
                cursor = stop;
            } else {
                flushClean();
                if (collection == Collection::Fresh) {
                    const auto next = views.lower_bound(cursor);
                    cursor = next == views.end() ? end : std::min(end, next->first);
                    continue;
                }
                const auto memory = query(cursor);
                if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
                const auto stop = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (capacity == *count) return true;
                const auto touched = (((stop + 4095) & ~static_cast<std::uintptr_t>(4095)) - (cursor & ~static_cast<std::uintptr_t>(4095))) / 4096;
                ULONG_PTR available = std::min<ULONG_PTR>(capacity - *count, touched);
                DWORD granularity = 0;
                if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(cursor), stop - cursor, pages + *count, &available, &granularity) != 0) fail("collect private guest writes");
                if (sources != nullptr) std::fill(sources + *count, sources + *count + available, std::uint8_t{2});
                *count += available;
                if (*count == capacity) return true;
                cursor = stop;
            }
        }
        flushClean();
        return true;
    }

private:
    std::optional<MappingSpan> mappingAtLocked(std::uintptr_t address, std::uintptr_t end) {
        const auto base = address & ~(pageBytes - 1);
        if (const auto found = views.find(base); found != views.end()) {
            const auto& view = found->second;
            if (view.identity == 0 || view.privateCopy) return std::nullopt;
            return MappingSpan{address, std::min(end, base + pageBytes), view.identity, 0};
        }
        auto found = privateMappings.upper_bound(address);
        if (found == privateMappings.begin()) return std::nullopt;
        --found;
        if (found->second.first <= address || found->second.second == 0) return std::nullopt;
        const auto memory = query(address);
        if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE || copyProtection(memory.Protect)) return std::nullopt;
        const auto stop = std::min({end, found->second.first, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize});
        if (stop <= address) return std::nullopt;
        return MappingSpan{address, stop, found->second.second, 1};
    }

    bool validateMappingLocked(const MappingSpan* spans, std::size_t count) {
        if (spans == nullptr || count == 0) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& expected = spans[i];
            if (expected.begin >= expected.end || expected.identity == 0 || expected.kind > 1 || (i != 0 && spans[i - 1].end > expected.begin)) return false;
            for (auto cursor = expected.begin; cursor < expected.end;) {
                const auto current = mappingAtLocked(cursor, expected.end);
                if (!current || current->identity != expected.identity || current->kind != expected.kind) return false;
                cursor = current->end;
            }
        }
        return true;
    }

    bool collectable(std::uintptr_t address, std::uintptr_t end) {
        const auto proven = collectableRanges.upper_bound(address);
        if (proven != collectableRanges.begin() && std::prev(proven)->second >= end) return true;
        auto position = views.lower_bound(address & ~(pageBytes - 1));
        for (auto cursor = address; cursor < end;) {
            const auto base = cursor & ~(pageBytes - 1);
            if (const auto found = viewAt(base, position); found != views.end()) {
                if (found->second.protection == PAGE_NOACCESS || (found->second.resident && !watchResident)) return false;
                cursor = std::min(end, base + pageBytes);
                continue;
            }
            const auto memory = query(cursor);
            if (memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE) return false;
            cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
        }
        remember(collectableRanges, address, end);
        return true;
    }

    static constexpr std::size_t pageBytes = 0x4000;
    static constexpr std::size_t subpageBytes = 0x1000;
    static_assert(subpageBytes == 4096, "Collect reports one page per subpage");
    static constexpr std::size_t subpages = pageBytes / subpageBytes;
    static constexpr std::uint8_t allSubpages = (1u << subpages) - 1;
    static constexpr std::size_t chunkBytes = 0x200000;
    static std::uint64_t nextSharedPageSerial() {
        static std::atomic<std::uint64_t> serials{0};
        auto previous = serials.load(std::memory_order_relaxed);
        do {
            if (previous >= ((std::uint64_t{1} << 63) / pageBytes) - 2) throw std::runtime_error("canonical guest backing identities are exhausted");
        } while (!serials.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed));
        return previous + 1;
    }
    struct DriverAlias {
        DriverAlias(void* view, unsigned char* bytes) : view(view), bytes(bytes) {}
        DriverAlias(const DriverAlias&) = delete;
        DriverAlias& operator=(const DriverAlias&) = delete;
        void* view;
        unsigned char* bytes;
        ~DriverAlias() { if (view != nullptr) UnmapViewOfFile(view); }
    };
    struct SharedPage {
        std::uint64_t serial = nextSharedPageSerial();
        std::array<std::uint64_t, subpages> generation = [] {
            std::array<std::uint64_t, subpages> initial{};
            initial.fill(1);
            return initial;
        }();
        std::vector<std::uintptr_t> aliases;
        std::uint32_t pins = 0;
        std::array<std::uint32_t, subpages> accessGuards{};
        std::array<std::uint32_t, subpages> readableGuards{};
        std::unique_ptr<DriverAlias> driverAlias;
        std::array<std::uint64_t, subpages> nativeGeneration{};
        std::array<std::uint64_t, subpages> nativeMixedThrough{};
        std::array<const void*, subpages> nativeWriter{};
    };
    struct ProtectedPage {
        std::shared_ptr<SharedPage> page;
        std::uint8_t mask;
    };
    struct ViewIdentity {
        std::uintptr_t base;
        std::shared_ptr<SharedPage> page;
        std::uint64_t identity;
    };
    struct AccessGuard {
        std::vector<ProtectedPage> pages;
        bool (*resolve)(std::uint64_t, void*) = nullptr;
        void* context = nullptr;
        void (*releaseContext)(void*) = nullptr;
        bool opened = false;
        std::mutex resolverMutex;
        ~AccessGuard() {
            if (releaseContext != nullptr) releaseContext(context);
        }
    };
    struct View;

    static std::uint8_t protectedAccess(const SharedPage& page) {
        std::uint8_t mask = 0;
        for (std::size_t sub = 0; sub < subpages; ++sub) {
            if (page.accessGuards[sub] != 0) mask |= static_cast<std::uint8_t>(1u << sub);
        }
        return mask;
    }

    static std::uint8_t readableAccess(const SharedPage& page) {
        std::uint8_t mask = 0;
        for (std::size_t sub = 0; sub < subpages; ++sub) {
            if (page.accessGuards[sub] != 0 && page.readableGuards[sub] == page.accessGuards[sub]) mask |= static_cast<std::uint8_t>(1u << sub);
        }
        return mask;
    }

    static void protectAccess(std::uintptr_t base, const View& view, std::uint8_t mask) {
        const auto guarded = static_cast<std::uint8_t>(mask & protectedAccess(*view.page));
        const auto readable = static_cast<std::uint8_t>(guarded & readableAccess(*view.page));
        const DWORD protection = view.protection == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
        protectSubpages(base, static_cast<std::uint8_t>(guarded & ~readable), PAGE_NOACCESS, "protect pending guest access");
        protectSubpages(base, readable, protection, "open pending guest access for reading");
        const auto watched = static_cast<std::uint8_t>(mask & ~guarded & view.armed);
        protectSubpages(base, watched, protection, "restore guest write tracking");
        protectSubpages(base, static_cast<std::uint8_t>(mask & ~guarded & ~watched), view.protection, "restore resolved guest access");
    }

    MappingLock lockResolved(std::uintptr_t address, std::size_t bytes) {
        if (address > std::numeric_limits<std::uintptr_t>::max() - pageBytes || bytes > std::numeric_limits<std::uintptr_t>::max() - address - pageBytes) throw std::runtime_error("native mapping mutation range overflow");
        MappingLock held(*this);
        auto& lock = held.lock;
        if (!resolveHostWrites.load(std::memory_order_acquire) || bytes == 0) {
            lock.lock();
            return held;
        }
        const auto begin = address & ~(pageBytes - 1);
        const auto end = (address + bytes + pageBytes - 1) & ~(pageBytes - 1);
        for (;;) {
            lock.lock();
            if (!accessProtectedLocked(begin, end - begin)) return held;
            lock.unlock();
            if (!held.exclusion.Owns()) throw std::runtime_error("protected guest mapping mutation requires native preflight outside the allocation registry");
            held.exclusion.Unlock();
            if (!ResolveAccess(begin, end - begin)) throw std::runtime_error("guest mapping changed during native access resolution");
            held.exclusion.Lock();
        }
    }

    bool accessProtectedLocked(std::uintptr_t address, std::size_t bytes) const {
        if (accessGuards.empty()) return false;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            if ((protectedAccess(*it->second.page) & subpagesOf(it->first, address, address + bytes)) != 0) return true;
        }
        return false;
    }

    void requireAccessResolved(std::uintptr_t address, std::size_t bytes, bool wholePages = false) const {
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - address) throw std::runtime_error("guest access range wraps its address");
        if (accessGuards.empty()) return;
        for (auto it = views.lower_bound(address & ~(pageBytes - 1)); it != views.end() && it->first < address + bytes; ++it) {
            if ((protectedAccess(*it->second.page) & (wholePages ? allSubpages : subpagesOf(it->first, address, address + bytes))) != 0) throw std::runtime_error("guest mapping operation requires prior access resolution");
        }
    }

    void releaseAccessLocked(std::uint64_t token, const AccessGuard& guard) {
        for (const auto& held : guard.pages) {
            for (std::size_t sub = 0; sub < subpages; ++sub) {
                if ((held.mask & (1u << sub)) == 0) continue;
                --held.page->accessGuards[sub];
                if (guard.opened) --held.page->readableGuards[sub];
            }
        }
        accessGuards.erase(token);
        accessGuardCount.store(accessGuards.size(), std::memory_order_release);
        for (const auto& held : guard.pages) {
            for (const auto alias : held.page->aliases) protectAccess(alias, views.at(alias), held.mask);
        }
    }
    struct Physical;
    struct Section {
        HANDLE handle;
        std::map<std::uint64_t, std::uint64_t> pageIds;
        std::map<std::uint64_t, std::shared_ptr<Physical>> physical;
        explicit Section(HANDLE handle) : handle(handle) {}
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;
        ~Section() { CloseHandle(handle); }
    };
    std::shared_ptr<Section> sectionFor(HANDLE section) {
        const auto key = reinterpret_cast<std::uintptr_t>(section);
        if (const auto found = sections.find(key); found != sections.end()) {
            if (sameSection(found->second->handle, section)) return found->second;
            sections.erase(found);
        }
        for (auto it = knownSections.begin(); it != knownSections.end();) {
            const auto known = it->lock();
            if (known == nullptr) {
                it = knownSections.erase(it);
                continue;
            }
            if (sameSection(known->handle, section)) {
                sections.emplace(key, known);
                return known;
            }
            ++it;
        }
        auto owned = std::make_shared<Section>(nullptr);
        if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &owned->handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) fail("keep shared guest section");
        knownSections.push_back(owned);
        sections.emplace(key, owned);
        return owned;
    }

    struct Saved {
        unsigned char* bytes;
        explicit Saved(unsigned char* bytes) : bytes(bytes) {}
        Saved(const Saved&) = delete;
        Saved& operator=(const Saved&) = delete;
        ~Saved() { VirtualFree(bytes, 0, MEM_RELEASE); }
    };
    struct Physical {
        std::weak_ptr<SharedPage> page;
        bool stored = false;
        std::shared_ptr<Saved> saved;
        std::size_t savedOffset = 0;
    };
    struct View {
        std::shared_ptr<SharedPage> page;
        DWORD protection;
        std::array<std::uint64_t, subpages> seen;
        std::uint8_t armed;
        std::shared_ptr<Section> section;
        std::uint64_t offset;
        std::uint32_t hostWrites;
        std::uint8_t collected = 0;
        bool resident = false;
        std::shared_ptr<Physical> slot;
        std::uint64_t identity = 0;
        bool privateCopy = false;
        std::uint64_t backingSerial = 0;
        std::array<std::uint64_t, subpages> nativeCollected{};
        std::uint8_t nativeBaseline = 0;
    };

    std::uint8_t sourceProvenance(const View& view, std::size_t sub, std::uint64_t since) const {
        const auto bit = static_cast<std::uint8_t>(1u << sub);
        if (view.resident || view.privateCopy || view.backingSerial == 0 || view.page->pins != 0 || view.hostWrites != 0 || (view.nativeBaseline & bit) == 0) return 2;
        for (const auto alias : view.page->aliases) {
            const auto& other = views.at(alias);
            const auto protection = other.protection & 0xffu;
            if (other.resident || other.privateCopy || other.hostWrites != 0 || other.offset != view.offset || other.section != view.section || (other.protection & PAGE_GUARD) != 0 || (protection != PAGE_READONLY && protection != PAGE_READWRITE)) return 2;
        }
        if (view.page->nativeGeneration[sub] > since) {
            const auto writer = view.page->nativeWriter[sub];
            if (writer == nullptr || writer == nativeThreadToken() || view.page->nativeMixedThrough[sub] > since) return 2;
            return 1;
        }
        for (const auto alias : view.page->aliases) {
            const auto& other = views.at(alias);
            if (writable(other.protection) && (other.armed & bit) == 0) return 2;
        }
        return 0;
    }

    std::uint8_t collectedProvenance(const View& view, std::size_t sub) const {
        return sourceProvenance(view, sub, view.nativeCollected[sub]);
    }

    bool ensureDriverAlias(View& view) {
        if (view.page->driverAlias != nullptr) return true;
        if (view.resident) return false;
        auto alias = std::make_unique<DriverAlias>(nullptr, nullptr);
        const auto lead = view.offset % granularity;
        void* mapped = map(view.section->handle, GetCurrentProcess(), nullptr, view.offset - lead, lead + pageBytes, 0, PAGE_READWRITE, nullptr, 0);
        if (mapped == nullptr) return false;
        alias->view = mapped;
        alias->bytes = static_cast<unsigned char*>(mapped) + lead;
        view.page->driverAlias = std::move(alias);
        return true;
    }

    unsigned char* driverPointer(std::uintptr_t address, bool write) {
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end()) return reinterpret_cast<unsigned char*>(address);
        auto& view = found->second;
        const auto protection = view.protection & 0xffu;
        if (protection == PAGE_NOACCESS || protection == PAGE_EXECUTE || (view.protection & PAGE_GUARD) != 0 || (write && !writable(view.protection) && !copyProtection(view.protection))) throw std::runtime_error("driver alias access violates guest mapping protection");
        if (view.resident || view.privateCopy) {
            if (write && view.armed != 0) {
                protectSubpages(base, view.armed, view.protection, "open driver write to private guest memory");
                view.armed = 0;
            }
            return reinterpret_cast<unsigned char*>(address);
        }
        if (write || view.page->driverAlias != nullptr || protectedAccess(*view.page) != 0) {
            if (!ensureDriverAlias(view)) throw std::runtime_error("driver alias mapping is unavailable");
            return view.page->driverAlias->bytes + address - base;
        }
        return reinterpret_cast<unsigned char*>(address);
    }

    void noteDriverWrite(std::uintptr_t address, std::size_t bytes) {
        const auto base = address & ~(pageBytes - 1);
        const auto found = views.find(base);
        if (found == views.end() || found->second.resident) return;
        invalidate(*found->second.page, subpagesOf(base, address, address + bytes), WriteProvenance::Driver);
    }
    struct Run {
        std::uintptr_t end;
        std::array<std::uint64_t, 8> collected;
        std::uint32_t fresh;
    };
    struct Window {
        void* view = nullptr;
        unsigned char* bytes = nullptr;
    };
    using Ranges = std::map<std::uintptr_t, std::uintptr_t>;

    static bool runCollected(const Run& run, std::size_t bit) {
        return (run.collected[bit >> 6] >> (bit & 63) & 1) != 0;
    }

    void collectResident(std::uintptr_t start, Run& run, std::uintptr_t from, std::uintptr_t stop, void** pages, std::size_t* count, bool clear, Collection collection) {
        const auto first = from & ~std::uintptr_t{4095};
        std::vector<std::uintptr_t> written;
        if (collection != Collection::Fresh) {
            std::vector<void*> found((stop - first + 4095) / 4096);
            ULONG_PTR reported = found.size();
            DWORD granularity = 0;
            if (GetWriteWatch(clear ? WRITE_WATCH_FLAG_RESET : 0, reinterpret_cast<void*>(first), stop - first, found.data(), &reported, &granularity) != 0) fail("collect resident guest writes");
            for (ULONG_PTR index = 0; index < reported; ++index) written.push_back(reinterpret_cast<std::uintptr_t>(found[index]));
        }
        auto next = written.begin();
        std::uintptr_t resetFrom = 0;
        const auto flushReset = [&](std::uintptr_t to) {
            if (resetFrom != 0 && ResetWriteWatch(reinterpret_cast<void*>(resetFrom), to - resetFrom) != 0) fail("reset resident guest writes");
            resetFrom = 0;
        };
        for (auto at = first; at < stop; at += 4096) {
            const auto bit = (at - start) >> 12;
            const bool fresh = !runCollected(run, bit);
            bool wrote = false;
            while (next != written.end() && *next < at) ++next;
            if (next != written.end() && *next == at) wrote = true;
            const bool report = collection == Collection::Fresh ? fresh : collection == Collection::Armed ? !fresh && wrote : fresh || wrote;
            if (report) pages[(*count)++] = reinterpret_cast<void*>(std::max(at, from));
            const bool settle = clear && fresh && collection != Collection::Armed;
            if (settle) {
                run.collected[bit >> 6] |= 1ull << (bit & 63);
                --run.fresh;
                forget(freshRanges, at, at + 4096);
                if (collection == Collection::Fresh && resetFrom == 0) resetFrom = at;
            } else if (collection == Collection::Fresh) {
                flushReset(at);
            }
        }
        if (collection == Collection::Fresh) flushReset(stop);
    }

    Window open(HANDLE section, std::uint64_t offset, std::size_t bytes) {
        const auto lead = offset % granularity;
        Window window;
        window.view = map(section, GetCurrentProcess(), nullptr, offset - lead, lead + bytes, 0, PAGE_READWRITE, nullptr, 0);
        if (window.view == nullptr) fail("open a window on direct memory");
        window.bytes = static_cast<unsigned char*>(window.view) + lead;
        return window;
    }

    void close(Window& window) {
        if (window.view != nullptr && !unmap(GetCurrentProcess(), window.view, 0)) fail("close a window on direct memory");
        window = {};
    }

    void commitResident(std::uintptr_t base, std::size_t bytes) {
        const DWORD flags = MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | (watchResident ? MEM_WRITE_WATCH : 0);
        if (!allocate(GetCurrentProcess(), reinterpret_cast<void*>(base), bytes, flags, PAGE_READWRITE, nullptr, 0)) fail("replace guest placeholder with resident direct memory");
    }

    void mapShared(std::uintptr_t base, HANDLE section, std::uint64_t offset, DWORD protection) {
        void* page = reinterpret_cast<void*>(base);
        if (!map(section, GetCurrentProcess(), page, offset, pageBytes, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, nullptr, 0)) fail("map shared guest page");
        if (query(base).State == MEM_RESERVE && !VirtualAlloc(page, pageBytes, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) fail("commit shared guest page");
        DWORD previous;
        if (!VirtualProtect(page, pageBytes, protection, &previous)) fail("protect shared guest page");
    }

    static bool blank(std::uintptr_t page) {
        const auto* words = reinterpret_cast<const std::uint64_t*>(page);
        for (std::size_t i = 0; i < pageBytes / sizeof(std::uint64_t); ++i) {
            if (words[i] != 0) return false;
        }
        return true;
    }

    void demote(std::uintptr_t address) {
        const auto run = std::prev(residentRuns.upper_bound(address));
        const auto start = run->first;
        const auto stop = run->second.end;
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(start), stop - start, PAGE_READONLY, &previous)) fail("fence resident guest pages");
        const auto section = views.at(start).section;
        auto window = open(section->handle, views.at(start).offset, stop - start);
        if (query(reinterpret_cast<std::uintptr_t>(window.bytes)).State == MEM_RESERVE && !VirtualAlloc(window.bytes, stop - start, MEM_COMMIT, PAGE_READWRITE)) fail("commit direct memory pages");
        std::memcpy(window.bytes, reinterpret_cast<const void*>(start), stop - start);
        close(window);
        if (!VirtualFree(reinterpret_cast<void*>(start), stop - start, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release resident guest pages");
        for (auto page = start; page < stop; page += pageBytes) {
            auto& view = views.at(page);
            split(page, pageBytes);
            mapShared(page, view.section->handle, view.offset, view.protection);
            view.slot->stored = true;
            view.resident = false;
            view.seen.fill(0);
            view.nativeBaseline = 0;
            view.armed = 0;
            view.collected = 0;
            for (std::size_t sub = 0; sub < subpages; ++sub) {
                if (runCollected(run->second, ((page - start) >> 12) + sub)) view.collected |= static_cast<std::uint8_t>(1u << sub);
            }
            invalidate(*view.page);
        }
        residentRuns.erase(run);
    }

    void breakRun(std::uintptr_t at) {
        const auto next = residentRuns.upper_bound(at);
        if (next == residentRuns.begin()) return;
        const auto run = std::prev(next);
        if (at > run->first && at < run->second.end) demote(at);
    }

    void evict(std::map<std::uintptr_t, Run>::iterator run) {
        const auto start = run->first;
        const auto stop = run->second.end;
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(start), stop - start, PAGE_READONLY, &previous)) fail("open resident guest pages");
        std::shared_ptr<Saved> saved;
        for (auto page = start; page < stop; page += pageBytes) {
            const auto found = views.find(page);
            auto& view = found->second;
            if (!blank(page)) {
                if (!saved) {
                    auto* bytes = static_cast<unsigned char*>(VirtualAlloc(nullptr, stop - start, MEM_RESERVE, PAGE_READWRITE));
                    if (bytes == nullptr) fail("reserve unmapped direct memory");
                    saved = std::make_shared<Saved>(bytes);
                }
                if (!VirtualAlloc(saved->bytes + (page - start), pageBytes, MEM_COMMIT, PAGE_READWRITE)) fail("keep unmapped direct memory");
                std::memcpy(saved->bytes + (page - start), reinterpret_cast<const void*>(page), pageBytes);
                view.slot->saved = saved;
                view.slot->savedOffset = page - start;
            }
            if (view.page->aliases.size() == 2 && std::erase(view.page->aliases, page) != 0) multiAliasPages.fetch_sub(1, std::memory_order_acq_rel);
            else std::erase(view.page->aliases, page);
            views.erase(found);
        }
        forget(freshRanges, start, stop);
        residentRuns.erase(run);
    }

    static void forget(Ranges& ranges, std::uintptr_t start, std::uintptr_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second > start) --it;
        while (it != ranges.end() && it->first < end) {
            const auto first = it->first;
            const auto last = it->second;
            it = ranges.erase(it);
            if (first < start) ranges.emplace(first, start);
            if (last > end) it = ranges.emplace(end, last).first;
        }
    }

    static void remember(Ranges& ranges, std::uintptr_t start, std::uintptr_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second >= start) --it;
        while (it != ranges.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = ranges.erase(it);
        }
        ranges.emplace(start, end);
    }

    void forgetClean(std::uintptr_t start, std::uintptr_t end) {
        forget(cleanRanges, start, end);
    }

    void rememberClean(std::uintptr_t start, std::uintptr_t end) {
        remember(cleanRanges, start, end);
    }

    enum class WriteProvenance : std::uint8_t { Driver, Cpu, Unknown };

    static const void* nativeThreadToken() {
        static thread_local unsigned char token = 0;
        return &token;
    }

    std::uint64_t nextNativeWriteGeneration() {
        auto previous = nativeWriteGeneration.load(std::memory_order_relaxed);
        do {
            if (previous == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("native write generations are exhausted");
        } while (!nativeWriteGeneration.compare_exchange_weak(previous, previous + 1, std::memory_order_acq_rel));
        return previous + 1;
    }

    void invalidate(SharedPage& page, std::uint8_t mask = allSubpages, WriteProvenance source = WriteProvenance::Unknown) {
        const auto native = source != WriteProvenance::Driver ? nextNativeWriteGeneration() : 0;
        for (std::size_t sub = 0; sub < subpages; ++sub) {
            if ((mask & (1u << sub)) == 0) continue;
            ++page.generation[sub];
            if (native != 0) {
                const auto writer = source == WriteProvenance::Cpu ? nativeThreadToken() : nullptr;
                if (page.nativeWriter[sub] != writer) page.nativeMixedThrough[sub] = page.nativeGeneration[sub];
                page.nativeGeneration[sub] = native;
                page.nativeWriter[sub] = writer;
            }
            for (const auto alias : page.aliases) forgetClean(alias + sub * subpageBytes, alias + (sub + 1) * subpageBytes);
        }
    }

    static std::uint8_t subpagesOf(std::uintptr_t base, std::uintptr_t address, std::uintptr_t end) {
        const auto from = std::max(address, base);
        const auto to = std::min(end, base + pageBytes);
        if (to <= from) return 0;
        const auto first = (from - base) / subpageBytes;
        const auto last = (to - 1 - base) / subpageBytes;
        return static_cast<std::uint8_t>(((1u << (last + 1)) - 1) & ~((1u << first) - 1));
    }

    static void protectSubpages(std::uintptr_t base, std::uint8_t mask, DWORD protection, const char* operation) {
        for (std::size_t sub = 0; sub < subpages;) {
            if ((mask & (1u << sub)) == 0) {
                ++sub;
                continue;
            }
            auto stop = sub;
            while (stop < subpages && (mask & (1u << stop)) != 0) ++stop;
            DWORD previous;
            if (!VirtualProtect(reinterpret_cast<void*>(base + sub * subpageBytes), (stop - sub) * subpageBytes, protection, &previous)) fail(operation);
            sub = stop;
        }
    }

    bool sameSection(HANDLE first, HANDLE second) const {
        return compare != nullptr && compare(first, second);
    }

    static bool copyProtection(DWORD protection) {
        const auto access = protection & 0xffu;
        return access == PAGE_WRITECOPY || access == PAGE_EXECUTE_WRITECOPY;
    }

    static bool writable(DWORD protection) {
        return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE;
    }
    using AllocateFunction = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapFunction = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using UnmapFunction = BOOL (WINAPI*)(HANDLE, PVOID, ULONG);
    using CompareFunction = BOOL (WINAPI*)(HANDLE, HANDLE);

    WindowsMappings() {
        const auto module = GetModuleHandleW(L"KernelBase.dll");
        if (!module) fail("load Windows memory API");
        allocate = reinterpret_cast<AllocateFunction>(GetProcAddress(module, "VirtualAlloc2"));
        map = reinterpret_cast<MapFunction>(GetProcAddress(module, "MapViewOfFile3"));
        unmap = reinterpret_cast<UnmapFunction>(GetProcAddress(module, "UnmapViewOfFile2"));
        if (!allocate || !map || !unmap) throw std::runtime_error("Windows placeholder memory APIs are required");
        compare = reinterpret_cast<CompareFunction>(GetProcAddress(module, "CompareObjectHandles"));
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        granularity = system.dwAllocationGranularity;
    }

    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }

    static MEMORY_BASIC_INFORMATION query(std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory)) fail("query guest memory");
        return memory;
    }

    static void split(std::uintptr_t address, std::size_t bytes) {
        auto memory = query(address);
        memory = query(reinterpret_cast<std::uintptr_t>(memory.AllocationBase));
        if (memory.State != MEM_RESERVE) throw std::runtime_error("guest mapping requires a placeholder");
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (address != base) {
            if (!VirtualFree(reinterpret_cast<void*>(base), address - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder prefix");
            memory = query(address);
        }
        if (memory.RegionSize < bytes) throw std::runtime_error("guest placeholder is too small");
        if (memory.RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("split guest placeholder suffix");
    }

    void reset(std::uintptr_t address, std::size_t bytes) {
        requireAccessResolved(address, bytes, true);
        const auto end = address + bytes;
        auto found = privateMappings.upper_bound(address);
        if (found != privateMappings.begin()) --found;
        std::vector<std::pair<std::uintptr_t, std::pair<std::uintptr_t, std::uint64_t>>> kept;
        while (found != privateMappings.end() && found->first < end) {
            if (found->second.first <= address) { ++found; continue; }
            if (found->first < address) kept.emplace_back(found->first, std::pair{address, found->second.second});
            if (found->second.first > end) kept.emplace_back(end, found->second);
            found = privateMappings.erase(found);
        }
        for (const auto& entry : kept) privateMappings.insert(entry);
        collectableRanges.clear();
        forgetClean(address, end);
        breakRun(address);
        breakRun(end);
        for (auto cursor = address; cursor < end;) {
            const auto memory = query(cursor);
            if (memory.State == MEM_RESERVE) {
                cursor = std::min(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                continue;
            }
            if (reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != cursor) throw std::runtime_error("cannot release part of a host allocation");
            auto allocationEnd = cursor;
            do {
                const auto part = query(allocationEnd);
                if (part.AllocationBase != memory.AllocationBase) break;
                allocationEnd = reinterpret_cast<std::uintptr_t>(part.BaseAddress) + part.RegionSize;
            } while (allocationEnd < end);
            if (allocationEnd > end || query(allocationEnd).AllocationBase == memory.AllocationBase) throw std::runtime_error("guest release truncates a host allocation");
            if (memory.Type == MEM_MAPPED) {
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(cursor), MEM_PRESERVE_PLACEHOLDER)) fail("unmap shared guest page");
                const auto found = views.find(cursor);
                if (found != views.end()) {
                    if (found->second.page->aliases.size() == 2 && std::erase(found->second.page->aliases, cursor) != 0) multiAliasPages.fetch_sub(1, std::memory_order_acq_rel);
                    else std::erase(found->second.page->aliases, cursor);
                    views.erase(found);
                    forget(freshRanges, cursor, cursor + pageBytes);
                }
            } else if (memory.Type == MEM_PRIVATE) {
                if (const auto run = residentRuns.find(cursor); run != residentRuns.end()) evict(run);
                if (!VirtualFree(reinterpret_cast<void*>(cursor), allocationEnd - cursor, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) fail("release private guest memory");
            } else {
                throw std::runtime_error("unsupported guest mapping type");
            }
            cursor = allocationEnd;
        }
        const auto last = query(reinterpret_cast<std::uintptr_t>(query(end - 1).AllocationBase));
        const auto lastBase = reinterpret_cast<std::uintptr_t>(last.BaseAddress);
        if (lastBase + last.RegionSize > end) split(lastBase, end - lastBase);
        const auto first = query(address);
        split(address, std::min(bytes, reinterpret_cast<std::uintptr_t>(first.BaseAddress) + first.RegionSize - address));
        if (query(address).RegionSize != bytes && !VirtualFree(reinterpret_cast<void*>(address), bytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) fail("coalesce guest placeholders");
    }

    void* mapAliasRuns(std::uintptr_t address, std::size_t bytes) {
        auto* reserved = allocate(GetCurrentProcess(), nullptr, bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (reserved == nullptr) fail("reserve a read-write alias of shared guest memory");
        const auto base = reinterpret_cast<std::uintptr_t>(reserved);
        try {
            for (std::size_t done = 0; done < bytes;) {
                const auto& first = views.at(address + done);
                auto run = pageBytes;
                while (done + run < bytes) {
                    const auto& next = views.at(address + done + run);
                    if (next.offset != first.offset + run || (next.section != first.section && !sameSection(next.section->handle, first.section->handle))) break;
                    run += pageBytes;
                }
                split(base + done, run);
                if (!map(first.section->handle, GetCurrentProcess(), reinterpret_cast<void*>(base + done), first.offset, run, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) fail("map a run of a read-write alias of shared guest memory");
                done += run;
            }
        } catch (...) {
            releaseAlias(base, bytes);
            throw;
        }
        splitAliases.emplace(base, bytes);
        return reserved;
    }

    DWORD releaseAlias(std::uintptr_t base, std::size_t bytes) {
        DWORD error = ERROR_SUCCESS;
        for (auto cursor = base; cursor < base + bytes;) {
            const auto memory = query(cursor);
            const auto released = memory.State == MEM_RESERVE ? VirtualFree(memory.BaseAddress, 0, MEM_RELEASE) : unmap(GetCurrentProcess(), memory.BaseAddress, 0);
            if (!released && error == ERROR_SUCCESS) error = GetLastError();
            cursor = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        }
        return error;
    }

    Ranges cleanRanges;
    Ranges collectableRanges;
    Ranges freshRanges;
    std::map<std::uintptr_t, View> views;
    std::map<std::uintptr_t, std::shared_ptr<Section>> sections;
    std::vector<std::weak_ptr<Section>> knownSections;
    std::mutex mappingExclusion;
    std::vector<std::function<void()>> afterExclusion;
    std::atomic<DWORD> exclusionOwner{0};
    std::atomic<std::uint8_t> exclusionKind{0};
    std::atomic<std::uint32_t> exclusionWaiters{0};
    std::map<std::uintptr_t, std::pair<std::uintptr_t, std::uint64_t>> privateMappings;
    std::map<std::uint64_t, std::shared_ptr<AccessGuard>> accessGuards;
    std::atomic<std::size_t> accessGuardCount{0};
    std::atomic<bool> resolveHostWrites{false};
    std::atomic<std::size_t> multiAliasPages{0};
    std::atomic<bool (*)()> resolverBypass{nullptr};
    std::uint64_t nextAccessToken = 0;
    std::uint64_t viewIdentity = 0;
    struct ActiveAccessResolversTag {};
    static std::vector<std::uint64_t>& activeAccessResolvers() { return HostThreadLocal<std::vector<std::uint64_t>, ActiveAccessResolversTag>(); }

    using Views = std::map<std::uintptr_t, View>;

    Views::iterator viewAt(std::uintptr_t base, Views::iterator& position) {
        if (position != views.end() && position->first < base) {
            ++position;
            if (position != views.end() && position->first < base) position = views.lower_bound(base);
        }
        return position != views.end() && position->first == base ? position : views.end();
    }
    std::map<std::uintptr_t, Run> residentRuns;
    std::map<std::uintptr_t, std::size_t> splitAliases;
    std::size_t granularity = 0x10000;
    bool watchResident = false;
    std::atomic<std::uint64_t> nativeWriteGeneration{0};
    std::mutex mutex;
    AllocateFunction allocate = nullptr;
    MapFunction map = nullptr;
    UnmapFunction unmap = nullptr;
    CompareFunction compare = nullptr;
};

}
#endif

#endif

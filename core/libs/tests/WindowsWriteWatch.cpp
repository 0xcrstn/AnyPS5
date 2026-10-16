#define NOMINMAX
#include "prx/libc/include/WindowsMappings.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
using Mappings = GuestArena::WindowsMappings;
using Collection = Mappings::Collection;
constexpr std::size_t Small = 4096;
constexpr std::size_t Bytes = 65536;
std::atomic<unsigned> faults{0};

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

LONG CALLBACK HandleWrite(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    if (!Mappings::Get().HandleWrite(record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_SEARCH;
    ++faults;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct Region {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Bytes, nullptr);
    unsigned char* first = static_cast<unsigned char*>(Mappings::Get().Reserve(nullptr, Bytes));
    unsigned char* alias = static_cast<unsigned char*>(Mappings::Get().Reserve(nullptr, Bytes));

    Region() {
        Require(section && first && alias, "cannot allocate shared test memory");
        Mappings::Get().Map(first, Bytes, section, 0, PAGE_READWRITE);
        Mappings::Get().Map(alias, Bytes, section, 0, PAGE_READWRITE);
    }
    ~Region() {
        Mappings::Get().Reset(first, Bytes);
        Mappings::Get().Reset(alias, Bytes);
        VirtualFree(first, 0, MEM_RELEASE);
        VirtualFree(alias, 0, MEM_RELEASE);
        CloseHandle(section);
    }
    void Write(std::size_t page, unsigned char value) {
        static_cast<volatile unsigned char*>(first)[page * Small] = value;
    }
    std::set<std::size_t> Collect(Collection mode, std::size_t begin = 0, std::size_t count = 16, bool clear = true, bool useAlias = false, std::size_t capacity = 32) {
        auto* base = useAlias ? alias : first;
        const auto address = reinterpret_cast<std::uintptr_t>(base);
        auto cursor = address + begin * Small;
        const auto end = cursor + count * Small;
        std::set<std::size_t> result;
        while (cursor < end) {
            std::array<void*, 32> pages{};
            auto available = capacity;
            Require(Mappings::Get().Collect(cursor, end - cursor, pages.data(), &available, clear, mode), "shared collection failed");
            for (std::size_t i = 0; i < available; ++i) result.insert((reinterpret_cast<std::uintptr_t>(pages[i]) - address) / Small);
            if (available < capacity) break;
            cursor = reinterpret_cast<std::uintptr_t>(pages[available - 1]) + Small;
        }
        return result;
    }
    DWORD Protection(std::size_t page, bool useAlias = false) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery((useAlias ? alias : first) + page * Small, &info, sizeof(info)) == sizeof(info), "protection query failed");
        return info.Protect;
    }
};

std::set<std::size_t> Pages(std::size_t begin, std::size_t end) {
    std::set<std::size_t> result;
    for (auto page = begin; page < end; ++page) result.insert(page);
    return result;
}

void CheckArmed() {
    Region region;
    Require(region.Collect(Collection::Armed).empty(), "import collected fresh memory");
    const auto before = faults.load();
    region.Write(0, 1);
    region.Write(4, 2);
    Require(faults == before, "import armed fresh memory");
    Require(region.Collect(Collection::All, 0, 1) == Pages(0, 1), "first partial collect failed");
    Require(region.Protection(0) == PAGE_READONLY && region.Protection(3) == PAGE_READWRITE, "partial collect did not protect only its page");
    region.Write(0, 3);
    region.Write(4, 4);
    Require(region.Collect(Collection::Armed) == Pages(0, 1), "armed collect lost a write or consumed fresh pages");
    Require(region.Protection(4) == PAGE_READWRITE, "armed collect protected another fresh view");
    Require(region.Collect(Collection::All) == Pages(1, 16), "ordinary collect lost fresh pages");
    Require(region.Collect(Collection::All).empty(), "cleared memory reports writes");
}

void CheckFresh() {
    Region region;
    Require(region.Collect(Collection::All, 0, 1) == Pages(0, 1), "cannot collect initial page");
    region.Write(0, 7);
    Require(region.Collect(Collection::Fresh) == Pages(1, 16), "fresh collect consumed an existing dirty page");
    Require(region.Collect(Collection::Fresh).empty(), "fresh collect repeated initial writes");
    Require(region.Collect(Collection::Armed) == Pages(0, 1), "fresh collect cleared an existing write");
    region.Write(9, 9);
    Require(region.Collect(Collection::Armed) == Pages(9, 10), "write after activation was missed");
}

void CheckAliasesAndCapacity() {
    Region region;
    Require(region.Collect(Collection::All, 0, 1) == Pages(0, 1), "initial collect failed");
    Require(region.Protection(0, true) == PAGE_READONLY, "alias was not protected");
    Require(region.Collect(Collection::Armed, 0, 16, true, true).empty(), "alias protection was mistaken for collection history");
    Require(region.Collect(Collection::Fresh, 0, 16, true, true, 1) == Pages(0, 16), "small output buffers lost fresh pages");
    Require(region.Collect(Collection::Fresh, 0, 16, true, true).empty(), "small output buffers did not finish fresh collection");
    static_cast<volatile unsigned char*>(region.alias)[0] = 8;
    Require(region.Collect(Collection::Armed) == Pages(0, 1), "alias write was missed or fresh neighbors were consumed");
    Require(region.Collect(Collection::Fresh) == Pages(1, 16), "alias collection changed another view's history");
    Mappings::Get().Reset(region.first, Bytes);
    Mappings::Get().Map(region.first, Bytes, region.section, 0, PAGE_READWRITE);
    Require(region.Collect(Collection::Armed).empty(), "remapped address kept old collection history");
    Require(region.Collect(Collection::Fresh) == Pages(0, 16), "remapped memory was not fresh");
}

void CheckPinsAndHostWrites() {
    Region region;
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    Mappings::Get().Pin(address, 16384);
    Require(region.Collect(Collection::Fresh) == Pages(0, 16), "pinned fresh collection failed");
    Require(region.Protection(0) == PAGE_READWRITE, "pinned page was protected");
    Require(region.Collect(Collection::Armed) == Pages(0, 4), "pinned writes were trusted as clean");
    Mappings::Get().Unpin(address, 16384);
    Require(region.Collect(Collection::Armed) == Pages(0, 4), "unpin was not invalidated");
    Require(region.Collect(Collection::Armed).empty(), "unpin did not restore tracking");
    Require(Mappings::Get().BeginHostWrite(address, 16384), "host write refused");
    region.Write(0, 3);
    region.Collect(Collection::Armed);
    region.Write(0, 4);
    Mappings::Get().EndHostWrite(address, 16384);
    Require(region.Collect(Collection::Armed) == Pages(0, 4), "host write completion was missed");
    Require(region.Collect(Collection::Armed).empty(), "completed host write stayed dirty");
}

void CheckFailureAndHoles() {
    Region region;
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    DWORD previous;
    Mappings::Get().SetProtection(address + 16384, 16384, PAGE_NOACCESS);
    Require(VirtualProtect(region.first + 16384, 16384, PAGE_NOACCESS, &previous), "cannot protect test page");
    std::array<void*, 32> pages{};
    auto count = pages.size();
    Require(!Mappings::Get().Collect(address, Bytes, pages.data(), &count, true, Collection::Fresh), "fresh inaccessible page accepted");
    Require(region.Protection(0) == PAGE_READWRITE, "failed collect changed an earlier page");
    Mappings::Get().SetProtection(address + 16384, 16384, PAGE_READWRITE);
    Require(VirtualProtect(region.first + 16384, 16384, PAGE_READWRITE, &previous), "cannot restore test page");
    Mappings::Get().Reset(region.first + 16384, 16384);
    Require(region.Collect(Collection::Fresh) == std::set<std::size_t>({0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15}), "fresh collection did not tolerate an unmapped block neighbor");
    Require(region.Collect(Collection::Fresh).empty(), "holes caused fresh pages to repeat");
}

void CheckPrivate() {
    auto& mappings = Mappings::Get();
    auto* memory = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    Require(memory != nullptr, "private reservation failed");
    mappings.Commit(memory, Bytes, PAGE_READWRITE, Bytes, true);
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    std::array<void*, 32> pages{};
    std::size_t count = pages.size();
    mappings.Collect(address, Bytes, pages.data(), &count, true);
    static_cast<volatile unsigned char*>(memory)[Small] = 3;
    count = pages.size();
    Require(mappings.Collect(address, Bytes, pages.data(), &count, true, Collection::Fresh) && count == 0, "fresh collect touched private memory");
    count = pages.size();
    Require(mappings.Collect(address, Bytes, pages.data(), &count, true, Collection::Armed) && count == 1 && pages[0] == memory + Small, "private write watch changed");
    mappings.Reset(memory, Bytes);
    VirtualFree(memory, 0, MEM_RELEASE);
}

void CheckForgottenSectionKeepsNoPages() {
    auto& mappings = Mappings::Get();
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Bytes, nullptr);
    auto* first = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    auto* second = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    Require(section && first && second, "cannot allocate forgotten section test memory");
    const auto resident = [&](unsigned char* address) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(address, &info, sizeof(info)) == sizeof(info), "resident query failed");
        return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE;
    };
    mappings.Map(first, Bytes, section, 0, PAGE_READWRITE, true, true);
    mappings.Map(second, Bytes, section, 0, PAGE_READWRITE, true, true);
    Require(!resident(first) && !resident(second), "a second mapping kept the first resident");
    mappings.ForgetSection(section);
    mappings.Reset(first, Bytes);
    mappings.Reset(second, Bytes);
    mappings.Map(first, Bytes, section, 0, PAGE_READWRITE, true, true);
    Require(resident(first), "a forgotten section's handle value kept its pages in the section");
    first[0] = 7;
    mappings.ForgetSection(section);
    mappings.Reset(first, Bytes);
    mappings.Map(first, Bytes, section, 0, PAGE_READWRITE, true, true);
    Require(resident(first) && first[0] == 0, "a forgotten section's handle value kept its unmapped bytes");
    mappings.Reset(first, Bytes);
    VirtualFree(first, 0, MEM_RELEASE);
    VirtualFree(second, 0, MEM_RELEASE);
    CloseHandle(section);
}

void CheckConcurrentWrites() {
    Region region;
    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t page = 0; page < 16; ++page) region.Write(page, static_cast<unsigned char>(page + 1));
    });
    start.store(true, std::memory_order_release);
    region.Collect(Collection::Fresh);
    writer.join();
    for (std::size_t page = 0; page < 16; ++page) Require(region.first[page * Small] == page + 1, "activation lost a concurrent write");
    region.Collect(Collection::Armed);
    Require(region.Collect(Collection::Armed).empty(), "concurrent writes did not settle");
}

void CheckMappingExclusion() {
    struct ResolutionMode {
        ResolutionMode() { Mappings::Get().SetResolveHostWrites(true); }
        ~ResolutionMode() { Mappings::Get().SetResolveHostWrites(false); }
    } mode;
    struct Context {
        unsigned char* destination;
        std::array<unsigned char, Small> pixels;
        unsigned calls = 0;
    } context{nullptr, {}};
    Region region;
    auto& mappings = Mappings::Get();
    context.destination = region.first;
    const auto first = reinterpret_cast<std::uintptr_t>(region.first);
    const auto alias = reinterpret_cast<std::uintptr_t>(region.alias);
    std::memset(region.first, 0x11, Bytes);
    context.pixels.fill(0x44);
    const auto resolve = +[](std::uint64_t token, void* pointer) {
        auto& context = *static_cast<Context*>(pointer);
        auto& mappings = Mappings::Get();
        Require(mappings.ExclusionKind() == 0, "native materialization retained the mapping exclusion");
        mappings.CopyDriverMemory(context.destination, context.pixels.data(), context.pixels.size());
        ++context.calls;
        mappings.ReleaseAccess(token);
        return true;
    };
    const auto token = mappings.TryProtectAccess(first, Small, resolve, &context, +[](void*) {});
    Require(token != 0 && mappings.MappingProtected(alias, Small), "an alias did not share native output protection");
    struct Exclusion {
        explicit Exclusion(std::uint8_t kind) { Mappings::Get().BeginMappingExclusion(kind); }
        ~Exclusion() { Mappings::Get().EndMappingExclusion(); }
    };
    {
        const Exclusion mutation(0);
        bool refused = false;
        try { mappings.Reset(region.alias, Bytes); }
        catch (const std::runtime_error&) { refused = true; }
        Require(refused && context.calls == 0 && mappings.MappingProtected(alias, Small), "a final mapping mutation ran its materializer while retaining exclusion");
    }
    Require(mappings.ResolveAccess(alias, Small) && context.calls == 1, "native alias preflight did not materialize outside exclusion");
    Require(std::all_of(region.first, region.first + Small, [](unsigned char value) { return value == 0x44; }) && std::all_of(region.first + Small, region.first + Bytes, [](unsigned char value) { return value == 0x11; }), "native alias materialization lost pixels or untouched bytes");
    std::atomic<std::uint64_t> racedToken{~std::uint64_t{0}};
    std::exception_ptr failure;
    std::jthread installer;
    {
        const Exclusion mutation(0);
        Require(!mappings.MappingProtected(alias, Bytes), "completed preflight retained native protection");
        const auto before = mappings.ExclusionWaiters();
        installer = std::jthread([&] {
            try { racedToken.store(mappings.TryProtectAccess(alias, Small, resolve, &context, +[](void*) {}), std::memory_order_release); }
            catch (...) { failure = std::current_exception(); }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (mappings.ExclusionWaiters() == before && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        Require(mappings.ExclusionWaiters() > before, "the native guard installer did not reach the exclusion race");
        Require(racedToken.load(std::memory_order_acquire) == ~std::uint64_t{0}, "a new native guard passed the final mutation exclusion");
        mappings.Reset(region.alias, Bytes);
    }
    installer.join();
    if (failure) std::rethrow_exception(failure);
    Require(racedToken.load(std::memory_order_acquire) == 0, "a blocked native guard attached to a retired mapping");
    {
        const Exclusion store(1);
        bool refused = false;
        try { mappings.Reset(region.first, Bytes); }
        catch (const std::runtime_error&) { refused = true; }
        Require(refused && region.first[0] == 0x44, "a driver store callback changed mappings before refusal");
        refused = false;
        try { mappings.BeginMappingExclusion(1); }
        catch (const std::runtime_error&) { refused = true; }
        Require(refused, "nested driver store exclusion did not refuse before the tracker");
    }
}

void CheckPrivateMappingIncarnations() {
    auto& mappings = Mappings::Get();
    auto* memory = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    Require(memory != nullptr, "cannot reserve private incarnation memory");
    constexpr std::size_t granule = 16384;
    mappings.Commit(memory, Bytes, PAGE_READWRITE, granule, true, 101);
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    unsigned observations = 0;
    const auto observe = +[](void* context) { ++*static_cast<unsigned*>(context); };
    Require(mappings.ObservePrivateMapping(address, Bytes, 101, observe, &observations), "a current private commit could not restore its watch");
    mappings.Reset(memory + granule, granule);
    mappings.Commit(memory + granule, granule, PAGE_READWRITE, granule, true, 102);
    Require(!mappings.ObservePrivateMapping(address, Bytes, 101, observe, &observations), "a delayed private notification restored replacement mapping coverage");
    Require(!mappings.ObservePrivateMapping(address + granule, granule, 101, observe, &observations), "a replaced private granule retained its old notification identity");
    Require(mappings.ObservePrivateMapping(address, granule, 101, observe, &observations) && mappings.ObservePrivateMapping(address + 2 * granule, 2 * granule, 101, observe, &observations), "a partial replacement invalidated the untouched private incarnations");
    Require(mappings.ObservePrivateMapping(address + granule, granule, 102, observe, &observations) && observations == 4, "the replacement private notification did not restore exactly its own mapping");
    mappings.Reset(memory, Bytes);
    Require(!mappings.ObservePrivateMapping(address, Bytes, 101, observe, &observations), "a retired private allocation accepted a delayed notification");
    VirtualFree(memory, 0, MEM_RELEASE);
}

std::vector<GuestArena::MappingSpan> CaptureMapping(std::uintptr_t address, std::size_t bytes) {
    std::size_t count = 0;
    auto& mappings = Mappings::Get();
    Require(mappings.CaptureMapping(address, bytes, nullptr, 0, &count) && count != 0, "mapping witness sizing failed");
    std::vector<GuestArena::MappingSpan> spans(count);
    Require(mappings.CaptureMapping(address, bytes, spans.data(), spans.size(), &count) && count == spans.size(), "mapping witness capture failed");
    return spans;
}

void CheckMappingWitnesses() {
    struct Context { unsigned calls = 0; } context;
    Region region;
    Region replacementRegion;
    auto& mappings = Mappings::Get();
    const auto first = reinterpret_cast<std::uintptr_t>(region.first);
    const auto alias = reinterpret_cast<std::uintptr_t>(region.alias);
    auto original = CaptureMapping(first, Bytes);
    Require(original.size() == 1 && mappings.ValidateMapping(original.data(), original.size()), "a contiguous shared mapping did not have one valid incarnation run");
    {
        Region unrelated;
        mappings.Reset(unrelated.first, Bytes);
        mappings.Map(unrelated.first, Bytes, unrelated.section, 0, PAGE_READWRITE);
        Require(mappings.ValidateMapping(original.data(), original.size()), "unrelated mapping changes invalidated an output witness");
    }
    mappings.SetProtection(first, Bytes, PAGE_READONLY);
    Require(mappings.ValidateMapping(original.data(), original.size()), "a read-only protection change replaced the mapping incarnation");
    mappings.SetProtection(first, Bytes, PAGE_READWRITE);
    HANDLE duplicate = nullptr;
    Require(DuplicateHandle(GetCurrentProcess(), region.section, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS), "cannot duplicate witness section handle");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{duplicate};
    mappings.Map(region.alias, Bytes, duplicate, 0, PAGE_READWRITE);
    auto duplicateView = CaptureMapping(alias, Bytes);
    Require(mappings.ValidateMapping(original.data(), original.size()) && mappings.ValidateMapping(duplicateView.data(), duplicateView.size()), "a duplicate-handle alias invalidated the surviving mapping");
    struct Exclusion {
        explicit Exclusion(std::uint8_t kind) { Mappings::Get().BeginMappingExclusion(kind); }
        ~Exclusion() { Mappings::Get().EndMappingExclusion(); }
    };
    {
        const Exclusion store(1);
        Require(mappings.ValidateMapping(original.data(), original.size()), "mapping validation could not run while a driver store owned exclusion");
    }
    const auto resolve = +[](std::uint64_t token, void* pointer) {
        ++static_cast<Context*>(pointer)->calls;
        Mappings::Get().ReleaseAccess(token);
        return true;
    };
    std::exception_ptr failure;
    std::atomic<bool> refused{false};
    std::jthread installer;
    {
        const Exclusion mutation(0);
        const auto before = mappings.ExclusionWaiters();
        installer = std::jthread([&] {
            try {
                const auto token = mappings.TryProtectMapping(original.data(), original.size(), first, Bytes, resolve, &context, +[](void*) {});
                mappings.ReleaseAccess(token);
            }
            catch (const std::runtime_error& error) {
                if (std::string(error.what()).find("mapping incarnation") != std::string::npos) refused.store(true, std::memory_order_release);
                else failure = std::current_exception();
            }
            catch (...) { failure = std::current_exception(); }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (mappings.ExclusionWaiters() == before && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        Require(mappings.ExclusionWaiters() > before, "the witnessed installer did not reach the mapping exclusion");
        mappings.Reset(region.first, Bytes);
        mappings.Map(region.first, Bytes, replacementRegion.section, 0, PAGE_READWRITE);
        mappings.Reset(region.first, Bytes);
        mappings.Map(region.first, Bytes, region.section, 0, PAGE_READWRITE);
    }
    installer.join();
    if (failure) std::rethrow_exception(failure);
    Require(refused.load(std::memory_order_acquire) && context.calls == 0 && !mappings.MappingProtected(first, Bytes), "a delayed witnessed guard attached old output to a replacement mapping");
    Require(!mappings.ValidateMapping(original.data(), original.size()) && mappings.ValidateMapping(duplicateView.data(), duplicateView.size()), "X to Y to X mapping reuse preserved an old witness or invalidated its untouched alias");
    auto current = CaptureMapping(first, Bytes);
    struct ReleaseGuard { std::uint64_t token; ~ReleaseGuard() { Mappings::Get().ReleaseAccess(token); } };
    {
        const auto interior = mappings.TryProtectMapping(current.data(), current.size(), first + Small, Small, resolve, &context, +[](void*) {});
        const ReleaseGuard releaseInterior{interior};
        Require(interior != 0 && region.Protection(0, true) == PAGE_READWRITE && region.Protection(1, true) == PAGE_NOACCESS, "validation-only edge bytes expanded inward-rounded native protection");
    }
    const GuestArena::MappingSpan selected{first, first + Small, current[0].identity, current[0].kind};
    const auto token = mappings.TryProtectMapping(&selected, 1, first, Bytes, resolve, &context, +[](void*) {});
    const ReleaseGuard releaseGuard{token};
    Require(token != 0 && mappings.MappingProtected(alias, Small), "current witnessed output did not protect its duplicate-handle alias");
    Require(mappings.ResolveAccess(alias, Small) && context.calls == 1, "current witnessed output did not resolve through its duplicate-handle alias");
    mappings.Reset(region.first + 16384, 16384);
    std::array<GuestArena::MappingSpan, 4> unknown{};
    unknown[0].identity = 77;
    std::size_t count = unknown.size();
    Require(!mappings.CaptureMapping(first, Bytes, unknown.data(), unknown.size(), &count) && count == 0 && unknown[0].identity == 77, "a hole produced a partial or matching empty mapping witness");
    mappings.Map(region.first + 16384, 16384, region.section, 16384, PAGE_READWRITE);
    auto split = CaptureMapping(first, Bytes);
    Require(split.size() == 3 && !mappings.ValidateMapping(current.data(), current.size()), "a partial shared remap did not split its incarnation runs");
    GuestArena::MappingSpan untouched{first, first + 16384, current[0].identity, current[0].kind};
    Require(mappings.ValidateMapping(&untouched, 1), "a partial shared remap invalidated an untouched incarnation");
    count = 0;
    Require(mappings.CaptureMapping(first, Bytes, unknown.data(), 1, &count) && count == 3 && unknown[0].identity == 77, "insufficient witness capacity published a truncated proof");
    mappings.SetProtection(first + 16384, 16384, PAGE_WRITECOPY);
    count = unknown.size();
    Require(!mappings.CaptureMapping(first, Bytes, unknown.data(), unknown.size(), &count) && count == 0 && !mappings.ValidateMapping(split.data(), split.size()), "copy-on-write mapping accepted a definite output witness");
    mappings.SetProtection(first + 16384, 16384, PAGE_READWRITE);
    Require(!mappings.ValidateMapping(split.data(), split.size()), "restoring writable protection cleared unknown copy-on-write provenance");
    Require(!mappings.ValidateMapping(nullptr, 0), "an empty witness matched a mapping");
}

void CheckPrivateMappingWitnesses() {
    auto& mappings = Mappings::Get();
    auto* memory = static_cast<unsigned char*>(mappings.Reserve(nullptr, Bytes));
    Require(memory != nullptr, "cannot reserve private witness memory");
    struct Release {
        unsigned char* memory;
        ~Release() { Mappings::Get().Reset(memory, Bytes); VirtualFree(memory, 0, MEM_RELEASE); }
    } release{memory};
    constexpr std::size_t granule = 16384;
    const auto address = reinterpret_cast<std::uintptr_t>(memory);
    mappings.Commit(memory, Bytes, PAGE_READWRITE, granule, true, 201);
    auto original = CaptureMapping(address, Bytes);
    Require(original.size() == 1 && original[0].kind == 1, "a contiguous private commit did not coalesce its witness");
    mappings.SetProtection(address, Bytes, PAGE_READONLY);
    Require(mappings.ValidateMapping(original.data(), original.size()), "private protection-only mutation invalidated its incarnation");
    mappings.SetProtection(address, Bytes, PAGE_READWRITE);
    mappings.Reset(memory + granule, granule);
    mappings.Commit(memory + granule, granule, PAGE_READWRITE, granule, true, 202);
    auto replacement = CaptureMapping(address, Bytes);
    Require(replacement.size() == 3 && !mappings.ValidateMapping(original.data(), original.size()), "private middle replacement retained the previous whole-range witness");
    bool refused = false;
    try { mappings.TryProtectMapping(original.data(), original.size(), address, Bytes, +[](std::uint64_t, void*) { return false; }, nullptr, nullptr); }
    catch (const std::runtime_error& error) { refused = std::string(error.what()).find("mapping incarnation") != std::string::npos; }
    Require(refused, "unsupported private protection bypassed stale mapping validation");
    GuestArena::MappingSpan untouched{address + 2 * granule, address + Bytes, 201, 1};
    Require(mappings.ValidateMapping(&untouched, 1) && mappings.ValidateMapping(replacement.data(), replacement.size()), "private middle replacement invalidated unaffected mapping spans");
    mappings.Reset(memory + granule, granule);
    mappings.Commit(memory + granule, granule, PAGE_READWRITE, granule, true);
    std::size_t count = 0;
    Require(!mappings.CaptureMapping(address, Bytes, nullptr, 0, &count) && count == 0, "private memory without a tracked incarnation produced an output witness");
}

void CheckResidentMappingWitness() {
    auto& mappings = Mappings::Get();
    Region region;
    mappings.Reset(region.first, Bytes);
    mappings.Reset(region.alias, Bytes);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE, 0, Bytes, nullptr);
    Require(section != nullptr, "cannot create resident witness section");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{section};
    mappings.Map(region.first, Bytes, section, 0, PAGE_READWRITE, true, true);
    const auto address = reinterpret_cast<std::uintptr_t>(region.first);
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(region.first, &memory, sizeof(memory)) == sizeof(memory) && memory.Type == MEM_PRIVATE, "resident witness setup did not create resident pages");
    auto witness = CaptureMapping(address, Bytes);
    Require(witness.size() == 1 && witness[0].kind == 0 && mappings.ValidateMapping(witness.data(), witness.size()), "a resident view could not prove its logical mapping incarnation");
    const auto resolve = +[](std::uint64_t, void*) { return false; };
    Require(mappings.TryProtectMapping(witness.data(), witness.size(), address, Bytes, resolve, nullptr, nullptr) == 0 && mappings.ValidateMapping(witness.data(), witness.size()), "unsupported resident native protection corrupted its otherwise valid mapping witness");
    mappings.Map(region.alias, Bytes, section, 0, PAGE_READWRITE);
    Require(mappings.ValidateMapping(witness.data(), witness.size()), "resident alias demotion replaced its logical mapping incarnation");
}
}

int main() {
    auto* handler = AddVectoredExceptionHandler(1, HandleWrite);
    try {
        Require(handler != nullptr, "cannot install test handler");
        CheckArmed();
        CheckFresh();
        CheckAliasesAndCapacity();
        CheckPinsAndHostWrites();
        CheckFailureAndHoles();
        CheckPrivate();
        CheckConcurrentWrites();
        CheckForgottenSectionKeepsNoPages();
        CheckMappingExclusion();
        CheckPrivateMappingIncarnations();
        CheckMappingWitnesses();
        CheckPrivateMappingWitnesses();
        CheckResidentMappingWitness();
        std::cout << "Windows deferred write-watch tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    RemoveVectoredExceptionHandler(handler);
}

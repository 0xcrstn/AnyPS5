#include "prx/libSceAgcDriver/Execution/include/DriverHeapTrace.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace AgcDriver::DriverHeapTrace {

bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_TRACE_DRIVER_HEAP");
        return text != nullptr && *text != '\0' && *text != '0';
    }();
    return enabled;
}

#ifdef _WIN32
namespace {

using HeapAllocFunction = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFunction = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using HeapFreeFunction = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);
using VirtualAllocFunction = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFreeFunction = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);

struct Block {
    std::uint64_t bytes;
    std::size_t module;
};

struct Module {
    std::uintptr_t begin;
    std::uintptr_t end;
    std::size_t index;
};

struct State {
    std::mutex mutex;
    HeapAllocFunction heapAlloc = nullptr;
    HeapReAllocFunction heapReAlloc = nullptr;
    HeapFreeFunction heapFree = nullptr;
    VirtualAllocFunction virtualAlloc = nullptr;
    VirtualFreeFunction virtualFree = nullptr;
    std::vector<Module> ranges;
    std::vector<ModuleUse> modules;
    std::unordered_set<std::uintptr_t> hooked;
    std::unordered_map<void*, Block> heapBlocks;
    std::map<std::uintptr_t, Block> virtualBlocks;
};

State& Traced() {
    static auto* state = new State();
    return *state;
}

std::size_t moduleOf(const State& state, std::uintptr_t address) {
    const auto found = std::upper_bound(state.ranges.begin(), state.ranges.end(), address, [](std::uintptr_t value, const Module& module) { return value < module.begin; });
    if (found == state.ranges.begin()) return state.modules.size();
    const auto& module = *std::prev(found);
    return address < module.end ? module.index : state.modules.size();
}

struct Callers {
    std::array<void*, 4> frames{};
    USHORT count = 0;
};

Callers callers() {
    Callers result;
    result.count = RtlCaptureStackBackTrace(0, static_cast<ULONG>(result.frames.size()), result.frames.data(), nullptr);
    return result;
}

void note(std::unordered_map<void*, Block> State::*blocks, std::uint64_t ModuleUse::*bytes, void* pointer, std::uint64_t size, const Callers& from) {
    auto& state = Traced();
    std::lock_guard lock(state.mutex);
    auto module = state.modules.size();
    for (USHORT frame = 0; frame < from.count && module >= state.modules.size(); ++frame) module = moduleOf(state, reinterpret_cast<std::uintptr_t>(from.frames[frame]));
    if (module >= state.modules.size()) return;
    const auto [entry, inserted] = (state.*blocks).insert_or_assign(pointer, Block{size, module});
    static_cast<void>(entry);
    static_cast<void>(inserted);
    state.modules[module].*bytes += size;
    if (blocks == &State::heapBlocks) ++state.modules[module].heapBlocks;
}

void forget(std::unordered_map<void*, Block> State::*blocks, std::uint64_t ModuleUse::*bytes, void* pointer) {
    auto& state = Traced();
    std::lock_guard lock(state.mutex);
    const auto found = (state.*blocks).find(pointer);
    if (found == (state.*blocks).end()) return;
    auto& module = state.modules[found->second.module];
    module.*bytes -= found->second.bytes;
    if (blocks == &State::heapBlocks) --module.heapBlocks;
    (state.*blocks).erase(found);
}

LPVOID WINAPI TracedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes) {
    const auto pointer = Traced().heapAlloc(heap, flags, bytes);
    if (pointer != nullptr) note(&State::heapBlocks, &ModuleUse::heapBytes, pointer, bytes, callers());
    return pointer;
}

LPVOID WINAPI TracedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID previous, SIZE_T bytes) {
    const auto pointer = Traced().heapReAlloc(heap, flags, previous, bytes);
    if (pointer != nullptr) {
        forget(&State::heapBlocks, &ModuleUse::heapBytes, previous);
        note(&State::heapBlocks, &ModuleUse::heapBytes, pointer, bytes, callers());
    }
    return pointer;
}

BOOL WINAPI TracedHeapFree(HANDLE heap, DWORD flags, LPVOID pointer) {
    if (pointer != nullptr) forget(&State::heapBlocks, &ModuleUse::heapBytes, pointer);
    return Traced().heapFree(heap, flags, pointer);
}

void forgetCommitted(std::uintptr_t begin, std::uintptr_t end) {
    auto& state = Traced();
    std::lock_guard lock(state.mutex);
    for (auto entry = state.virtualBlocks.lower_bound(begin); entry != state.virtualBlocks.end() && entry->first < end;) {
        state.modules[entry->second.module].virtualBytes -= entry->second.bytes;
        entry = state.virtualBlocks.erase(entry);
    }
}

LPVOID WINAPI TracedVirtualAlloc(LPVOID address, SIZE_T bytes, DWORD type, DWORD protection) {
    const auto pointer = Traced().virtualAlloc(address, bytes, type, protection);
    if (pointer == nullptr || (type & MEM_COMMIT) == 0) return pointer;
    const auto from = callers();
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    forgetCommitted(begin, begin + bytes);
    auto& state = Traced();
    std::lock_guard lock(state.mutex);
    auto module = state.modules.size();
    for (USHORT frame = 0; frame < from.count && module >= state.modules.size(); ++frame) module = moduleOf(state, reinterpret_cast<std::uintptr_t>(from.frames[frame]));
    if (module >= state.modules.size()) return pointer;
    state.virtualBlocks.insert_or_assign(begin, Block{bytes, module});
    state.modules[module].virtualBytes += bytes;
    return pointer;
}

BOOL WINAPI TracedVirtualFree(LPVOID address, SIZE_T bytes, DWORD type) {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    std::uintptr_t end = begin + bytes;
    if ((type & MEM_RELEASE) != 0) {
        end = begin;
        MEMORY_BASIC_INFORMATION memory{};
        while (VirtualQuery(reinterpret_cast<const void*>(end), &memory, sizeof(memory)) == sizeof(memory) && memory.AllocationBase == address && memory.RegionSize != 0) end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    }
    if ((type & (MEM_RELEASE | MEM_DECOMMIT)) != 0) forgetCommitted(begin, end);
    return Traced().virtualFree(address, bytes, type);
}

bool contains(std::wstring_view text, std::wstring_view part) {
    if (part.size() > text.size()) return false;
    for (std::size_t at = 0; at + part.size() <= text.size(); ++at) {
        bool same = true;
        for (std::size_t index = 0; index < part.size() && same; ++index) same = towlower(text[at + index]) == towlower(part[index]);
        if (same) return true;
    }
    return false;
}

template<typename TFunction>
void hookEntry(ULONG_PTR& slot, TFunction& original, TFunction traced) {
    if (original == nullptr) original = reinterpret_cast<TFunction>(slot);
    DWORD previous = 0;
    if (VirtualProtect(&slot, sizeof(slot), PAGE_READWRITE, &previous) == 0) return;
    slot = reinterpret_cast<ULONG_PTR>(traced);
    VirtualProtect(&slot, sizeof(slot), previous, &previous);
}

void hookImports(State& state, HMODULE module) {
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return;
    for (auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); descriptor->Name != 0; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const std::string_view name(reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name));
            if (name == "HeapAlloc") hookEntry(slots->u1.Function, state.heapAlloc, &TracedHeapAlloc);
            else if (name == "HeapReAlloc") hookEntry(slots->u1.Function, state.heapReAlloc, &TracedHeapReAlloc);
            else if (name == "HeapFree") hookEntry(slots->u1.Function, state.heapFree, &TracedHeapFree);
            else if (name == "VirtualAlloc") hookEntry(slots->u1.Function, state.virtualAlloc, &TracedVirtualAlloc);
            else if (name == "VirtualFree") hookEntry(slots->u1.Function, state.virtualFree, &TracedVirtualFree);
        }
    }
}

}

std::size_t HookModules(std::string_view pathPart) {
    if (!Enabled()) return 0;
    std::array<HMODULE, 1024> handles{};
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), handles.data(), sizeof(handles), &needed) == 0) return 0;
    const std::wstring part(pathPart.begin(), pathPart.end());
    auto& state = Traced();
    std::lock_guard lock(state.mutex);
    std::size_t added = 0;
    for (std::size_t index = 0; index < std::min<std::size_t>(needed / sizeof(HMODULE), handles.size()); ++index) {
        const auto base = reinterpret_cast<std::uintptr_t>(handles[index]);
        if (state.hooked.contains(base)) continue;
        std::array<wchar_t, 1024> path{};
        if (K32GetModuleFileNameExW(GetCurrentProcess(), handles[index], path.data(), static_cast<DWORD>(path.size())) == 0 || !contains(path.data(), part)) continue;
        MODULEINFO info{};
        if (K32GetModuleInformation(GetCurrentProcess(), handles[index], &info, sizeof(info)) == 0) continue;
        state.hooked.insert(base);
        std::wstring_view full(path.data());
        const auto slash = full.find_last_of(L"\\/");
        const auto file = slash == std::wstring_view::npos ? full : full.substr(slash + 1);
        state.modules.push_back({std::string(file.begin(), file.end())});
        state.ranges.push_back({base, base + info.SizeOfImage, state.modules.size() - 1});
        std::sort(state.ranges.begin(), state.ranges.end(), [](const Module& left, const Module& right) { return left.begin < right.begin; });
        hookImports(state, handles[index]);
        ++added;
    }
    return added;
}

std::vector<ModuleUse> Read() {
    auto& state = Traced();
    std::lock_guard lock(state.mutex);
    return state.modules;
}

std::string Owner(std::uintptr_t address) {
    auto& state = Traced();
    std::lock_guard lock(state.mutex);
    auto found = state.virtualBlocks.upper_bound(address);
    if (found == state.virtualBlocks.begin()) return {};
    --found;
    return address - found->first < found->second.bytes ? state.modules[found->second.module].name : std::string{};
}
#else
std::size_t HookModules(std::string_view) {
    return 0;
}

std::vector<ModuleUse> Read() {
    return {};
}

std::string Owner(std::uintptr_t) {
    return {};
}
#endif

}

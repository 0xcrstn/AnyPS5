#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace AgcDriver {
namespace {

struct CaptureProfile {
    std::atomic<std::uint64_t> captures{0};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> pages{0};
    // The whole CaptureResources call: the plan lookup and the materialization are one call.
    std::atomic<std::uint64_t> captureNanoseconds{0};
    // Pages read word by word because recorded GPU work writes them, and the word reads of those
    // pages that waited (a read taking longer than WordWaitNanoseconds went through a hook sync),
    // were read raw on the driver's evidence, or were read both ways (verify) and differed.
    std::atomic<std::uint64_t> pagesWordwise{0};
    std::atomic<std::uint64_t> wordWaits{0};
    std::atomic<std::uint64_t> wordWaitNanoseconds{0};
    std::atomic<std::uint64_t> wordsRaw{0};
    std::atomic<std::uint64_t> wordsVerified{0};
    std::atomic<std::uint64_t> wordMismatches{0};
    // Words served from the driver's known values (never profile-gated: the [copy] line reads them).
    std::atomic<std::uint64_t> wordsKnown{0};
    std::atomic<std::uint64_t> wordsKnownVerified{0};
    std::atomic<std::uint64_t> wordsKnownMismatches{0};
    // Hook reads not reported to the observer because no GPU wait happened across them.
    std::atomic<std::uint64_t> observationsSkipped{0};
    // Sub-phases of a capture: the source lookup (the stage inputs, the recompiler's key over the
    // code and the plan, timed by CaptureResources itself: ResourceCapture::sourceNanoseconds),
    // whole-page fetches, per-word fetches (their hook waits included) and the GPU waits the flush
    // hook made anywhere inside the capture (Recorder::ThreadWaitedMs). The walk itself is what
    // remains of the capture time after the lookup, the fetches and the specialization
    // (ResourceMaterializer).
    std::atomic<std::uint64_t> resolveNanoseconds{0};
    std::atomic<std::uint64_t> pageReadNanoseconds{0};
    std::atomic<std::uint64_t> wordReadNanoseconds{0};
    std::atomic<std::uint64_t> hookWaitNanoseconds{0};
    // The driver's per-shader source handle memo: captures served from it, and resolves it made.
    std::atomic<std::uint64_t> handleHits{0};
    std::atomic<std::uint64_t> handleMisses{0};
};

struct MemoProfile {
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> patched{0};
    std::atomic<std::uint64_t> absent{0};
    std::atomic<std::uint64_t> valueWords{0};
    std::atomic<std::uint64_t> pointerWords{0};
    std::atomic<std::uint64_t> memory{0};
    std::atomic<std::uint64_t> replayed{0};
    std::atomic<std::uint64_t> hitNanoseconds{0};
    std::atomic<std::uint64_t> missNanoseconds{0};
    std::atomic<std::uint64_t> evaluationNanoseconds{0};
    std::atomic<std::uint64_t> pageSetupNanoseconds{0};
    std::atomic<std::uint64_t> pageReadNanoseconds{0};
    std::atomic<std::uint64_t> missReads{0};
    std::atomic<std::uint64_t> missPages{0};
    std::atomic<std::uint64_t> values{0};
    std::atomic<std::uint64_t> laneEvaluators{0};
    std::atomic<std::uint64_t> phis{0};
    std::mutex reportMutex;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

MemoProfile& MemoTotals() {
    static MemoProfile profile;
    return profile;
}

std::atomic<std::uint64_t> memoizedCaptures{0};

bool CaptureMemoEnabled() {
    static const bool enabled = std::getenv("APS5_NO_CAPTURE_MEMO") == nullptr;
    return enabled;
}

constexpr std::size_t MemoWays = 4;
constexpr std::size_t MemoShaders = 1024;

struct MemoEntry {
    std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
    std::uint64_t shaderBase = 0;
    std::vector<std::uint32_t> userData;
    std::vector<ShaderMemory::RecordedAccess> accesses;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
};

struct MemoShader {
    std::array<MemoEntry, MemoWays> ways;
    std::size_t next = 0;
};

struct MemoTable {
    std::unordered_map<const ShaderRecompiler::SourceHandle*, MemoShader> shaders;
    std::uint32_t depth = 0;
};

struct MemoDepth {
    explicit MemoDepth(MemoTable& table) : table(table) { ++table.depth; }
    ~MemoDepth() { --table.depth; }
    MemoTable& table;
};

MemoTable& ThreadMemo() {
    struct MemoTableStorage {};
    return HostThreadLocal<MemoTable, MemoTableStorage>();
}

bool AllWordsRead(const ShaderRecompiler::SrtUserDataUse& use) {
    return use.asValue == ~std::uint64_t{0};
}

bool WordRead(const ShaderRecompiler::SrtUserDataUse& use, std::size_t word) {
    return AllWordsRead(use) || (word < ShaderRecompiler::SrtUserDataUse::TrackedWords && (((use.asBase | use.asValue) >> word) & 1u) != 0u);
}

enum class MemoMatch { Other, Exact, UnreadWords, PointerWords, ValueWords };

MemoMatch MatchUserData(const MemoEntry& entry, std::uint64_t shaderBase, std::span<const std::uint32_t> userData) {
    if (entry.capture == nullptr || entry.shaderBase != shaderBase || entry.userData.size() != userData.size()) return MemoMatch::Other;
    const auto& use = entry.capture->userDataUse;
    bool exact = true;
    bool pointers = false;
    for (std::size_t word = 0; word < userData.size(); ++word) {
        if (entry.userData[word] == userData[word]) continue;
        exact = false;
        if (!WordRead(use, word)) continue;
        if (AllWordsRead(use) || ((use.asValue >> word) & 1u) != 0u) return MemoMatch::ValueWords;
        pointers = true;
    }
    if (exact) return MemoMatch::Exact;
    return pointers ? MemoMatch::PointerWords : MemoMatch::UnreadWords;
}

double Per(std::uint64_t value, std::uint64_t count) {
    return count != 0 ? static_cast<double>(value) / static_cast<double>(count) : 0.0;
}

void ReportMemo() {
    auto& memo = MemoTotals();
    std::unique_lock lock(memo.reportMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - memo.lastReport < std::chrono::seconds(10)) return;
    memo.lastReport = now;
    const auto take = [](std::atomic<std::uint64_t>& counter) { return counter.exchange(0, std::memory_order_relaxed); };
    const auto total = take(memo.total);
    const auto hits = take(memo.hits);
    const auto patched = take(memo.patched);
    const auto absent = take(memo.absent);
    const auto valueWords = take(memo.valueWords);
    const auto pointerWords = take(memo.pointerWords);
    const auto memory = take(memo.memory);
    const auto replayed = take(memo.replayed);
    const auto hitNs = take(memo.hitNanoseconds);
    const auto missNs = take(memo.missNanoseconds);
    const auto evaluationNs = take(memo.evaluationNanoseconds);
    const auto setupNs = take(memo.pageSetupNanoseconds);
    const auto pageNs = take(memo.pageReadNanoseconds);
    const auto reads = take(memo.missReads);
    const auto pages = take(memo.missPages);
    const auto values = take(memo.values);
    const auto laneEvaluators = take(memo.laneEvaluators);
    const auto phis = take(memo.phis);
    const auto served = hits + patched;
    const auto misses = total > served ? total - served : 0;
    AgcDriver::ProfilePrint_nid_no_patch("[capture-memo] (10 s) %llu prepared captures: %llu served from the memo (%llu with unread user words patched; %.1f words replayed each, %.1f us each); %llu walked (%.1f us each: SRT evaluation %.1f with page setup %.1f and page reads %.1f inside; %.1f word reads, %.2f new pages, %.1f values evaluated, %.2f lane evaluators, %.1f phis each), because: no entry %llu, read user words differ %llu (only pointer words %llu), guest words differ %llu\n", static_cast<unsigned long long>(total), static_cast<unsigned long long>(served), static_cast<unsigned long long>(patched), Per(replayed, served), Per(hitNs, served) / 1e3, static_cast<unsigned long long>(misses), Per(missNs, misses) / 1e3, Per(evaluationNs, misses) / 1e3, Per(setupNs, misses) / 1e3, Per(pageNs, misses) / 1e3, Per(reads, misses), Per(pages, misses), Per(values, misses), Per(laneEvaluators, misses), Per(phis, misses), static_cast<unsigned long long>(absent), static_cast<unsigned long long>(valueWords + pointerWords), static_cast<unsigned long long>(pointerWords), static_cast<unsigned long long>(memory));
}

constexpr std::uint64_t WordWaitNanoseconds = 50000;

bool WordwisePages() {
    static const bool wordwise = std::getenv("APS5_NO_WORDWISE_CAPTURE") == nullptr;
    return wordwise;
}

bool CaptureProfiled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

std::uint64_t NanosecondsSince(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
}

CaptureProfile& CaptureTotals() {
    static CaptureProfile profile;
    return profile;
}

std::atomic<ShaderMemory::WaitedMsProvider> waitedMsProvider{nullptr};

double WaitedMs() {
    const auto provider = waitedMsProvider.load(std::memory_order_acquire);
    return provider != nullptr ? provider() : 0.0;
}

}

void ShaderMemory::SetWaitedMsProvider(WaitedMsProvider provider) {
    waitedMsProvider.store(provider, std::memory_order_release);
}

ShaderMemory::ShaderMemory(std::span<const ShaderRecompiler::MemoryRegion> regions, PendingWriteQuery pendingWrite, PendingWriteObserver observe, HookWaitCounter hookWaits) : pendingWrite(pendingWrite), observe(observe), hookWaits(hookWaits) {
    const ShaderRecompiler::RequestMemoryView validated(regions);
    for (const auto& region : regions) {
        initial.emplace(region.guestAddress, region.bytes);
    }
}

ShaderMemory::Page& ShaderMemory::page(std::uint64_t base) {
    const auto found = pages.find(base);
    if (found != pages.end()) return found->second;
    auto& page = pages[base];
    ++CaptureTotals().pages;
    const bool profile = CaptureProfiled();
    const auto setupStarted = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // A page is either mapped whole or read word by word where the guest mapped less than a page,
    // or where recorded GPU work writes into it (a whole-page read would wait for that work).
    if (GuestMemory::Accessible(reinterpret_cast<const void*>(base), PageBytes)) {
        if (WordwisePages() && pendingWrite != nullptr && pendingWrite(base, PageBytes, {}) != PendingWrite::None) {
            page.wordwise = true;
            ++CaptureTotals().pagesWordwise;
            if (profile) pageSetupNanoseconds += NanosecondsSince(setupStarted);
            return page;
        }
        const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (profile) pageSetupNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(started - setupStarted).count());
        GuestMemory::Read(base, std::as_writable_bytes(std::span(page.words)), sizeof(std::uint32_t));
        if (profile) {
            const auto nanoseconds = NanosecondsSince(started);
            CaptureTotals().pageReadNanoseconds += nanoseconds;
            pageReadNanoseconds += nanoseconds;
        }
        page.valid.set();
    } else if (profile) {
        pageSetupNanoseconds += NanosecondsSince(setupStarted);
    }
    return page;
}

bool ShaderMemory::read(void* context, std::uint64_t address, std::uint32_t* value) {
    auto& self = *static_cast<ShaderMemory*>(context);
    if (address % sizeof(*value) != 0 || address > std::numeric_limits<std::uint64_t>::max() - sizeof(*value)) {
        throw std::runtime_error("AGC driver: invalid shader memory read address");
    }
    ++CaptureTotals().reads;
    if (!self.initial.empty()) {
        const auto next = self.initial.upper_bound(address);
        if (next != self.initial.begin()) {
            const auto previous = std::prev(next);
            const auto offset = address - previous->first;
            if (offset < previous->second.size()) {
                if (previous->second.size() - offset < sizeof(*value)) throw std::runtime_error("AGC driver: shader memory read crosses a snapshot boundary");
                std::memcpy(value, previous->second.data() + offset, sizeof(*value));
                if (self.recording != nullptr) self.recording->push_back({address, 0, *value});
                return true;
            }
        }
        if (next != self.initial.end() && next->first - address < sizeof(*value)) throw std::runtime_error("AGC driver: shader memory read overlaps a snapshot boundary");
    }
    auto& page = self.page(address & ~static_cast<std::uint64_t>(PageBytes - 1));
    const auto index = static_cast<std::size_t>((address % PageBytes) / sizeof(*value));
    if (!page.valid.test(index)) {
        std::uint32_t word = 0;
        auto policy = PendingWrite::Sync;
        if (page.wordwise) {
            policy = self.pendingWrite(address, sizeof(word), std::as_writable_bytes(std::span(&word, 1)));
            if (policy != PendingWrite::None && policy != PendingWrite::Sync && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(word))) policy = PendingWrite::Sync;
        }
        // A read is evidence only when the hook waited for unfinished GPU work across it.
        const auto hookWaits = [&] { return self.hookWaits != nullptr ? self.hookWaits() : 0; };
        const auto report = [&](std::uint64_t waitsBefore, bool unchanged) {
            if (self.hookWaits == nullptr || self.hookWaits() != waitsBefore) self.observe(address, unchanged);
            else ++CaptureTotals().observationsSkipped;
        };
        const auto fetchStarted = CaptureProfiled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (policy == PendingWrite::KnownValue || policy == PendingWrite::VerifyKnownValue) {
            // The query stored the known dword in `word`.
            ++CaptureTotals().wordsKnown;
            if (policy == PendingWrite::VerifyKnownValue) {
                std::uint32_t waited = 0;
                const auto waitsBefore = hookWaits();
                GuestMemory::Read(address, std::as_writable_bytes(std::span(&waited, 1)), alignof(std::uint32_t));
                ++CaptureTotals().wordsKnownVerified;
                if (waited != word) ++CaptureTotals().wordsKnownMismatches;
                if (self.observe != nullptr) report(waitsBefore, waited == word);
                word = waited;
            }
        } else if (policy == PendingWrite::Raw || policy == PendingWrite::VerifyRaw) {
            std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
            ++CaptureTotals().wordsRaw;
            if (policy == PendingWrite::VerifyRaw) {
                std::uint32_t waited = 0;
                const auto waitsBefore = hookWaits();
                GuestMemory::Read(address, std::as_writable_bytes(std::span(&waited, 1)), alignof(std::uint32_t));
                ++CaptureTotals().wordsVerified;
                if (waited != word) ++CaptureTotals().wordMismatches;
                if (self.observe != nullptr) report(waitsBefore, waited == word);
                word = waited;
            }
        } else {
            // On a word-wise page the bytes before the hook's wait are kept for the observer.
            const bool observed = page.wordwise && policy == PendingWrite::Sync && self.observe != nullptr && GuestMemory::Accessible(reinterpret_cast<const void*>(address), sizeof(word));
            std::uint32_t before = 0;
            if (observed) std::memcpy(&before, reinterpret_cast<const void*>(address), sizeof(before));
            const auto waitsBefore = observed ? hookWaits() : 0;
            const auto started = page.wordwise ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            GuestMemory::Read(address, std::as_writable_bytes(std::span(&word, 1)), alignof(std::uint32_t));
            if (page.wordwise) {
                const auto nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
                if (nanoseconds >= WordWaitNanoseconds) {
                    ++CaptureTotals().wordWaits;
                    CaptureTotals().wordWaitNanoseconds += nanoseconds;
                }
            }
            if (observed) report(waitsBefore, before == word);
        }
        if (CaptureProfiled()) CaptureTotals().wordReadNanoseconds += NanosecondsSince(fetchStarted);
        page.words[index] = word;
        page.valid.set(index);
    }
    page.read.set(index);
    page.recent.set(index);
    *value = page.words[index];
    if (self.recording != nullptr) self.recording->push_back({address, 0, *value});
    return true;
}

bool ShaderMemory::accessible(void* context, std::uint64_t address, std::uint64_t bytes) {
    auto& self = *static_cast<ShaderMemory*>(context);
    const bool result = self.probe(address, bytes);
    if (self.recording != nullptr) self.recording->push_back({address, bytes == 0 ? ~std::uint64_t{0} : bytes, result ? 1u : 0u});
    return result;
}

bool ShaderMemory::probe(std::uint64_t address, std::uint64_t bytes) {
    auto& self = *this;
    if (bytes == 0 || address > std::numeric_limits<std::uint64_t>::max() - bytes) return false;
    const auto end = address + bytes;
    while (address < end) {
        const auto next = self.initial.upper_bound(address);
        if (next != self.initial.begin()) {
            const auto previous = std::prev(next);
            if (address - previous->first < previous->second.size()) {
                address = previous->first + previous->second.size();
                continue;
            }
        }
        const auto base = address & ~static_cast<std::uint64_t>(PageBytes - 1);
        const auto pageEnd = std::min<std::uint64_t>(base + PageBytes, end);
        const auto& page = self.page(base);
        if (!page.wordwise && !page.valid.all() && !GuestMemory::Accessible(reinterpret_cast<const void*>(address), static_cast<std::size_t>(pageEnd - address))) return false;
        address = pageEnd;
    }
    return true;
}

ShaderMemory::KnownValueCounts ShaderMemory::KnownValues() {
    auto& totals = CaptureTotals();
    return {totals.wordsKnown.load(std::memory_order_relaxed), totals.wordsKnownVerified.load(std::memory_order_relaxed), totals.wordsKnownMismatches.load(std::memory_order_relaxed)};
}

std::uint64_t ShaderMemory::MemoizedCaptures() {
    return memoizedCaptures.load(std::memory_order_relaxed);
}

bool ShaderMemory::replay(std::span<const RecordedAccess> accesses) {
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Capture);
    for (const auto& access : accesses) {
        if (access.bytes == 0) {
            std::uint32_t word = 0;
            if (!read(this, access.address, &word) || word != access.value) return false;
            continue;
        }
        if (probe(access.address, access.bytes == ~std::uint64_t{0} ? 0 : access.bytes) != (access.value != 0u)) return false;
    }
    return true;
}

void ShaderMemory::CountHandleMemo(bool hit) {
    if (!CaptureProfiled()) return;
    (hit ? CaptureTotals().handleHits : CaptureTotals().handleMisses).fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle) {
    return capture(request, handle, nullptr);
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::Capture(const ShaderRecompiler::PreparedShaderInvocation& invocation) {
    auto& table = ThreadMemo();
    if (!CaptureMemoEnabled() || invocation.Handle() == nullptr || table.depth != 0) return capture(invocation.Request(), nullptr, &invocation);
    const MemoDepth depth(table);
    const auto& request = invocation.Request();
    const auto& handle = invocation.Handle();
    const auto userData = request.context.userData;
    const auto shaderBase = request.shader.codeAddress;
    const bool profile = CaptureProfiled();
    auto& memo = MemoTotals();
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto found = table.shaders.find(handle.get());
    if (found == table.shaders.end()) {
        if (table.shaders.size() >= MemoShaders) table.shaders.clear();
        found = table.shaders.emplace(handle.get(), MemoShader{}).first;
    }
    auto& shader = found->second;
    MemoEntry* slot = nullptr;
    auto reason = MemoMatch::Other;
    for (auto& entry : shader.ways) {
        if (entry.handle != handle) continue;
        const auto match = MatchUserData(entry, shaderBase, userData);
        if (match == MemoMatch::Exact || match == MemoMatch::UnreadWords) {
            const auto readsBefore = profile ? entry.accesses.size() : 0;
            if (!replay(entry.accesses)) {
                slot = &entry;
                reason = MemoMatch::Other;
                if (profile) ++memo.memory;
                break;
            }
            if (match == MemoMatch::UnreadWords) {
                auto patched = std::make_shared<ShaderRecompiler::ResourceCapture>(*entry.capture);
                std::copy_n(userData.begin(), patched->snapshot.userData.size(), patched->snapshot.userData.begin());
                entry.capture = std::move(patched);
                entry.userData.assign(userData.begin(), userData.end());
            }
            memoizedCaptures.fetch_add(1, std::memory_order_relaxed);
            if (profile) {
                ++memo.total;
                ++(match == MemoMatch::Exact ? memo.hits : memo.patched);
                memo.replayed += readsBefore;
                memo.hitNanoseconds += NanosecondsSince(started);
                ReportMemo();
            }
            return entry.capture;
        }
        if (match == MemoMatch::PointerWords || (match == MemoMatch::ValueWords && reason != MemoMatch::PointerWords)) reason = match;
    }
    if (profile && slot == nullptr) {
        if (reason == MemoMatch::PointerWords) ++memo.pointerWords;
        else if (reason == MemoMatch::ValueWords) ++memo.valueWords;
        else ++memo.absent;
    }
    if (slot == nullptr) slot = &shader.ways[shader.next++ % MemoWays];
    slot->capture = nullptr;
    slot->accesses.clear();
    const auto pagesBefore = profile ? pages.size() : 0;
    const auto setupBefore = pageSetupNanoseconds;
    const auto pageReadBefore = pageReadNanoseconds;
    const auto evaluationBefore = profile ? ShaderRecompiler::ResourceMaterializer::ThreadEvaluationNanoseconds() : 0;
    const auto countsBefore = profile ? ShaderRecompiler::ThreadSrtEvaluationCounts() : ShaderRecompiler::SrtEvaluationCounts{};
    recording = &slot->accesses;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> result;
    try {
        result = capture(request, nullptr, &invocation);
    } catch (...) {
        recording = nullptr;
        *slot = MemoEntry{};
        throw;
    }
    recording = nullptr;
    if (slot->handle != handle) slot->handle = handle;
    slot->shaderBase = shaderBase;
    slot->userData.assign(userData.begin(), userData.end());
    slot->capture = result;
    if (profile) {
        ++memo.total;
        memo.missReads += static_cast<std::uint64_t>(std::count_if(slot->accesses.begin(), slot->accesses.end(), [](const RecordedAccess& access) { return access.bytes == 0; }));
        memo.missPages += pages.size() - pagesBefore;
        memo.pageSetupNanoseconds += pageSetupNanoseconds - setupBefore;
        memo.pageReadNanoseconds += pageReadNanoseconds - pageReadBefore;
        memo.evaluationNanoseconds += ShaderRecompiler::ResourceMaterializer::ThreadEvaluationNanoseconds() - evaluationBefore;
        const auto counts = ShaderRecompiler::ThreadSrtEvaluationCounts();
        memo.values += counts.values - countsBefore.values;
        memo.laneEvaluators += counts.laneEvaluators - countsBefore.laneEvaluators;
        memo.phis += counts.phis - countsBefore.phis;
        memo.missNanoseconds += NanosecondsSince(started);
        ReportMemo();
    }
    return result;
}

std::shared_ptr<const ShaderRecompiler::ResourceCapture> ShaderMemory::capture(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::SourceHandle* handle, const ShaderRecompiler::PreparedShaderInvocation* invocation) {
    // The capture's word and page reads (through `read`) are attributed to it ([hooksync], [guestmem]).
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::Capture);
    const bool profile = CaptureProfiled();
    auto& totals = CaptureTotals();
    // The hook's GPU waits made anywhere inside the capture.
    const auto waitedBefore = profile ? WaitedMs() : 0.0;
    const auto started = std::chrono::steady_clock::now();
    ShaderRecompiler::SrtRuntime runtime;
    runtime.userData = request.context.userData;
    runtime.shaderBase = request.shader.codeAddress;
    runtime.userContext = this;
    runtime.readMemory = &read;
    runtime.readSpecializationMemory = &read;
    runtime.accessible = &accessible;
    auto capture = invocation != nullptr ? invocation->Capture(runtime) : handle != nullptr ? ShaderRecompiler::CaptureResources(request, runtime, *handle) : ShaderRecompiler::CaptureResources(request, runtime);
    if (profile) {
        totals.captureNanoseconds += NanosecondsSince(started);
        totals.resolveNanoseconds += capture->sourceNanoseconds;
        totals.hookWaitNanoseconds += static_cast<std::uint64_t>((WaitedMs() - waitedBefore) * 1e6);
        std::uint64_t initialBytes = 0;
        for (const auto& [address, bytes] : initial) initialBytes += bytes.size();
        if (++totals.captures % 500 == 0) {
            const auto captures = static_cast<double>(totals.captures.load());
            const auto captureNs = totals.captureNanoseconds.load();
            const auto resolveNs = totals.resolveNanoseconds.load();
            const auto pageNs = totals.pageReadNanoseconds.load();
            const auto wordNs = totals.wordReadNanoseconds.load();
            const auto specializationNs = ShaderRecompiler::ResourceMaterializer::SpecializationNanoseconds();
            const auto hookNs = totals.hookWaitNanoseconds.load();
            const auto walkNs = captureNs > resolveNs + pageNs + wordNs + specializationNs ? captureNs - resolveNs - pageNs - wordNs - specializationNs : 0;
            AgcDriver::ProfilePrint_nid_no_patch( "[capture] %llu captures: %llu word reads, %llu pages fetched (%llu read word by word over pending GPU writes: %llu words waited %.2f s, %llu read raw on evidence, %llu verified with %llu mismatches, %llu served from known values, %llu hook reads not observed: no GPU wait), capture (plan lookup + materialize) %.1f s, %llu KiB registered code this capture; sub-phases (s, us per capture): source resolve %.2f/%.1f, walk %.2f/%.1f, specialization (every materialize) %.2f/%.1f, page reads %.2f/%.1f, word reads %.2f/%.1f, hook GPU waits inside %.2f/%.1f; source handle memo %llu hits, %llu resolves\n", static_cast<unsigned long long>(totals.captures.load()), static_cast<unsigned long long>(totals.reads.load()), static_cast<unsigned long long>(totals.pages.load()), static_cast<unsigned long long>(totals.pagesWordwise.load()), static_cast<unsigned long long>(totals.wordWaits.load()), totals.wordWaitNanoseconds.load() / 1e9, static_cast<unsigned long long>(totals.wordsRaw.load()), static_cast<unsigned long long>(totals.wordsVerified.load()), static_cast<unsigned long long>(totals.wordMismatches.load()), static_cast<unsigned long long>(totals.wordsKnown.load()), static_cast<unsigned long long>(totals.observationsSkipped.load()), captureNs / 1e9, static_cast<unsigned long long>(initialBytes / 1024), resolveNs / 1e9, resolveNs / captures / 1e3, walkNs / 1e9, walkNs / captures / 1e3, specializationNs / 1e9, specializationNs / captures / 1e3, pageNs / 1e9, pageNs / captures / 1e3, wordNs / 1e9, wordNs / captures / 1e3, hookNs / 1e9, hookNs / captures / 1e3, static_cast<unsigned long long>(totals.handleHits.load()), static_cast<unsigned long long>(totals.handleMisses.load()));
        }
    }
    return capture;
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::Regions() const {
    std::vector<ShaderRecompiler::MemoryRegion> result;
    result.reserve(initial.size() + pages.size());
    auto next = initial.begin();
    // Both maps are ordered by address and never overlap, so a merge keeps the result sorted.
    for (const auto& [base, page] : pages) {
        while (next != initial.end() && next->first < base) {
            result.push_back({next->first, next->second});
            ++next;
        }
        for (std::size_t index = 0; index < PageWords;) {
            if (!page.read.test(index)) {
                ++index;
                continue;
            }
            const auto first = index;
            while (index < PageWords && page.read.test(index)) ++index;
            result.push_back({base + first * sizeof(std::uint32_t), std::as_bytes(std::span(page.words).subspan(first, index - first))});
        }
    }
    for (; next != initial.end(); ++next) result.push_back({next->first, next->second});
    return result;
}

std::vector<ShaderRecompiler::MemoryRegion> ShaderMemory::TakeRecentRegions() {
    std::vector<ShaderRecompiler::MemoryRegion> result;
    for (auto& [base, page] : pages) {
        if (page.recent.none()) continue;
        for (std::size_t index = 0; index < PageWords;) {
            if (!page.recent.test(index)) {
                ++index;
                continue;
            }
            const auto first = index;
            while (index < PageWords && page.recent.test(index)) ++index;
            result.push_back({base + first * sizeof(std::uint32_t), std::as_bytes(std::span(page.words).subspan(first, index - first))});
        }
        page.recent.reset();
    }
    return result;
}

DataWordPositionCounts DataWordPositions(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::pair<std::uint32_t, std::uint64_t>> leaves, std::span<const std::uint64_t> otherReads, std::span<const std::uint32_t> words, std::span<const std::uint32_t> flattenedSrt, std::vector<std::uint32_t>& positions, std::vector<std::uint32_t>& slots) {
    DataWordPositionCounts counts;
    positions.clear();
    slots.clear();
    std::vector<std::size_t> prefix(runs.size() + 1, 0);
    for (std::size_t i = 0; i < runs.size(); ++i) prefix[i + 1] = prefix[i] + static_cast<std::size_t>((runs[i].second - runs[i].first) / sizeof(std::uint32_t));
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    for (const auto& [slot, address] : leaves) {
        if ((address & 3u) != 0) {
            ++counts.unmapped;
            continue;
        }
        if (std::binary_search(otherReads.begin(), otherReads.end(), address)) {
            ++counts.aliased;
            continue;
        }
        auto run = std::upper_bound(runs.begin(), runs.end(), address, [](std::uint64_t value, const std::pair<std::uint64_t, std::uint64_t>& candidate) { return value < candidate.first; });
        if (run == runs.begin() || address + sizeof(std::uint32_t) > (run - 1)->second) {
            ++counts.unmapped;
            continue;
        }
        --run;
        const auto position = prefix[static_cast<std::size_t>(run - runs.begin())] + static_cast<std::size_t>((address - run->first) / sizeof(std::uint32_t));
        if (position >= words.size() || slot >= flattenedSrt.size() || words[position] != flattenedSrt[slot]) {
            ++counts.mismatched;
            continue;
        }
        found.emplace_back(static_cast<std::uint32_t>(position), slot);
    }
    std::sort(found.begin(), found.end());
    positions.reserve(found.size());
    slots.reserve(found.size());
    for (const auto& [position, slot] : found) {
        positions.push_back(position);
        slots.push_back(slot);
    }
    return counts;
}

}

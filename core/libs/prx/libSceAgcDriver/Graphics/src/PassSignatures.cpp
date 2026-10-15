#include "prx/libSceAgcDriver/Graphics/include/PassSignatures.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint64_t HashBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t HashPrime = 0x100000001b3ull;

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
    for (std::size_t byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8u)) & 0xffu;
        hash *= HashPrime;
    }
    return hash;
}

struct DrawScope {
    bool active = false;
    std::uint64_t hash = HashBasis;
    std::uint64_t passSerial = 0;
    std::uint64_t passIdentity = 0;
};

DrawScope& drawScope() {
    thread_local DrawScope scope;
    return scope;
}

struct Pass {
    std::uint64_t serial = 0;
    std::uint64_t hash = HashBasis;
    std::uint64_t draws = 0;
    bool open = false;
};

struct State {
    std::mutex mutex;
    Pass pass;
    std::unordered_map<std::uint64_t, std::uint64_t> previous;
    std::unordered_map<std::uint64_t, std::uint64_t> current;
    PassSignatures::Counts counts;
    PassSignatures::Counts reported;
};

State& state() {
    static auto* shared = new State;
    return *shared;
}

std::atomic<int>& enabledOverride() {
    static std::atomic<int> enabled{-1};
    return enabled;
}

void closePass(State& shared) {
    if (!shared.pass.open) return;
    ++shared.counts.passes;
    shared.counts.draws += shared.pass.draws;
    ++shared.current[shared.pass.hash];
    if (const auto found = shared.previous.find(shared.pass.hash); found != shared.previous.end() && found->second != 0) {
        --found->second;
        ++shared.counts.repeated;
        shared.counts.repeatedDraws += shared.pass.draws;
    }
    shared.pass = {};
}

}

bool PassSignatures::Enabled() {
    static const bool profiled = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto forced = enabledOverride().load(std::memory_order_relaxed);
    return forced >= 0 ? forced != 0 : profiled;
}

void PassSignatures::SetEnabled(bool enabled) {
    enabledOverride().store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool PassSignatures::Active() {
    return drawScope().active;
}

void PassSignatures::BeginDraw(std::uint64_t passSerial, std::uint64_t passIdentity) {
    if (!Enabled()) return;
    auto& scope = drawScope();
    scope = {true, mix(HashBasis, passIdentity), passSerial, passIdentity};
}

void PassSignatures::Note(std::uint32_t command, std::initializer_list<std::uint64_t> arguments, std::span<const std::uint64_t> tail) {
    auto& scope = drawScope();
    if (!scope.active) return;
    scope.hash = mix(scope.hash, command);
    for (const auto argument : arguments) scope.hash = mix(scope.hash, argument);
    scope.hash = mix(scope.hash, tail.size());
    for (const auto word : tail) scope.hash = mix(scope.hash, word);
}

void PassSignatures::EndDraw() {
    auto& scope = drawScope();
    if (!scope.active) return;
    scope.active = false;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    if (shared.pass.open && shared.pass.serial != scope.passSerial) closePass(shared);
    if (!shared.pass.open) shared.pass = {scope.passSerial, mix(HashBasis, scope.passIdentity), 0, true};
    shared.pass.hash = mix(shared.pass.hash, scope.hash);
    ++shared.pass.draws;
}

void PassSignatures::NoteFrame() {
    if (!Enabled()) return;
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    closePass(shared);
    shared.previous = std::move(shared.current);
    shared.current.clear();
    ++shared.counts.frames;
}

PassSignatures::Counts PassSignatures::Totals() {
    auto& shared = state();
    std::lock_guard lock(shared.mutex);
    return shared.counts;
}

void PassSignatures::Report() {
    if (!Enabled()) return;
    auto& shared = state();
    Counts delta;
    {
        std::lock_guard lock(shared.mutex);
        delta = {shared.counts.passes - shared.reported.passes, shared.counts.repeated - shared.reported.repeated, shared.counts.draws - shared.reported.draws, shared.counts.repeatedDraws - shared.reported.repeatedDraws, shared.counts.frames - shared.reported.frames};
        shared.reported = shared.counts;
    }
    AgcDriver::ProfilePrint_nid_no_patch("[pass-reuse] (10 s, %llu frames): %llu of %llu render passes (%llu of %llu draws) recorded exactly the command stream of a pass of the previous frame\n", static_cast<unsigned long long>(delta.frames), static_cast<unsigned long long>(delta.repeated), static_cast<unsigned long long>(delta.passes), static_cast<unsigned long long>(delta.repeatedDraws), static_cast<unsigned long long>(delta.draws));
}

}

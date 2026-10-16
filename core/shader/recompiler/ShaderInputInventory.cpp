#include "ShaderInputInventory.hpp"
#include "ShaderCacheDirectory.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace ShaderRecompiler::ShaderDiskCache {
std::span<const std::byte> CompilerConfigurationKey();
}

namespace ShaderRecompiler {
namespace {

constexpr std::uint64_t HeaderLimit = 128u;
constexpr std::string_view InputDialect = "LE64-FORKLDS32";
constexpr std::uint32_t MinimumRequestVersion = 32u;

bool validRequestDialect(std::string_view bytes) {
    if (bytes.size() < 8u) return false;
    std::uint32_t version = 0;
    for (unsigned i = 0; i < 4u; ++i) version |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 4u])) << (i * 8u);
    return version >= MinimumRequestVersion;
}

std::string configurationPrefix() {
    const auto configuration = ShaderDiskCache::CompilerConfigurationKey();
    if (configuration.size() > 65536u) throw std::length_error("shader compiler configuration exceeds limit");
    std::string prefix;
    for (unsigned i = 0; i < 8u; ++i) prefix.push_back(static_cast<char>(static_cast<std::uint64_t>(configuration.size()) >> (i * 8u)));
    prefix.append(reinterpret_cast<const char*>(configuration.data()), configuration.size());
    return prefix;
}

class DirectoryLock {
public:
    explicit DirectoryLock(const std::filesystem::path& directory) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return;
        const auto path = directory / ".writer-lock";
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        handle = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
        if (handle >= 0 && flock(handle, LOCK_EX | LOCK_NB) != 0) { ::close(handle); handle = -1; }
#endif
    }
    ~DirectoryLock() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (handle >= 0) ::close(handle);
#endif
    }
    DirectoryLock(const DirectoryLock&) = delete;
    DirectoryLock& operator=(const DirectoryLock&) = delete;
    bool Acquired() const {
#ifdef _WIN32
        return handle != INVALID_HANDLE_VALUE;
#else
        return handle >= 0;
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
};

class Digest {
public:
    void Append(std::string_view bytes) {
        length += bytes.size();
        while (!bytes.empty()) {
            const auto count = std::min(bytes.size(), block.size() - used);
            std::memcpy(block.data() + used, bytes.data(), count);
            used += count;
            bytes.remove_prefix(count);
            if (used == block.size()) { transform(); used = 0; }
        }
    }

    std::string Finish() {
        const auto bits = length * 8u;
        block[used++] = 0x80u;
        if (used > 56u) {
            std::fill(block.begin() + used, block.end(), 0u);
            transform();
            used = 0;
        }
        std::fill(block.begin() + used, block.begin() + 56u, 0u);
        for (unsigned i = 0; i < 8u; ++i) block[63u - i] = static_cast<unsigned char>(bits >> (i * 8u));
        transform();
        constexpr char hex[] = "0123456789abcdef";
        std::string text;
        text.reserve(64u);
        for (const auto value : hash) {
            for (int shift = 28; shift >= 0; shift -= 4) text.push_back(hex[(value >> shift) & 15u]);
        }
        return text;
    }
private:
    void transform() {
        constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
        };
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16u; ++i) {
            for (std::size_t j = 0; j < 4u; ++j) words[i] = (words[i] << 8u) | block[i * 4u + j];
        }
        for (std::size_t i = 16u; i < words.size(); ++i) {
            const auto a = words[i - 15u], b = words[i - 2u];
            words[i] = words[i - 16u] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3u)) + words[i - 7u] + (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10u));
        }
        auto working = hash;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto a = working[0], e = working[4];
            const auto first = working[7] + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & working[5]) ^ (~e & working[6])) + constants[i] + words[i];
            const auto second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & working[1]) ^ (a & working[2]) ^ (working[1] & working[2]));
            for (std::size_t j = 7u; j > 0; --j) working[j] = working[j - 1u];
            working[4] += first;
            working[0] = first + second;
        }
        for (std::size_t i = 0; i < hash.size(); ++i) hash[i] += working[i];
    }

    std::array<std::uint32_t, 8> hash{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    std::array<unsigned char, 64> block{};
    std::size_t used = 0;
    std::uint64_t length = 0;
};

bool validIdentity(std::string_view text) {
    return text.size() == 64u && std::ranges::all_of(text, [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

bool supportedHost() {
    return std::endian::native == std::endian::little && sizeof(void*) == 8u;
}

ShaderInputCaptureStatus preflight(const RecompileRequest& request, std::uint64_t limit) {
    if (!supportedHost() || request.shader.code.empty()) return ShaderInputCaptureStatus::Invalid;
    std::uint64_t bytes = 0;
    const auto add = [&](std::uint64_t count, std::uint64_t width = 1u) {
        if (count > (limit - bytes) / width) return false;
        bytes += count * width;
        return true;
    };
    const auto binary = [&](const ShaderBinary& shader) { return add(shader.code.size(), 4u) && add(shader.header.size()); };
    if (!binary(request.shader) || !add(request.context.userData.size(), 4u) || !add(request.context.linkedCallFallbacks.size(), 4u) || !add(request.target.supportedCapabilities.size(), 4u)) return ShaderInputCaptureStatus::TooLarge;
    if (request.context.memory.size() > 4096u || request.target.supportedExtensions.size() > 256u || (request.graphics && request.graphics->linkedPrograms.size() > 256u)) return ShaderInputCaptureStatus::TooLarge;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(request.context.memory.size());
    for (const auto& region : request.context.memory) {
        if (!add(region.bytes.size())) return ShaderInputCaptureStatus::TooLarge;
        if (region.bytes.empty() || region.guestAddress > std::numeric_limits<std::uint64_t>::max() - region.bytes.size()) return ShaderInputCaptureStatus::Invalid;
        ranges.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) if (ranges[i].first < ranges[i - 1u].second) return ShaderInputCaptureStatus::Invalid;
    for (const auto extension : request.target.supportedExtensions) if (!add(extension.size())) return ShaderInputCaptureStatus::TooLarge;
    if (request.graphics) {
        for (const auto& program : request.graphics->linkedPrograms) {
            if (!binary(program.binary) || !add(program.userData.size(), 4u)) return ShaderInputCaptureStatus::TooLarge;
        }
    }
    return ShaderInputCaptureStatus::Stored;
}

std::string requestIdentity(const RecompileRequest& request, std::uint64_t limit, std::uint64_t& size) {
    Digest digest;
    const auto prefix = configurationPrefix();
    if (prefix.size() > limit) throw std::length_error("shader input exceeds entry limit");
    size = prefix.size();
    digest.Append(prefix);
    RequestSerializer{}.VisitSerialized(request, [&](std::string_view bytes) {
        if (bytes.size() > limit - size) throw std::length_error("shader input exceeds entry limit");
        size += bytes.size();
        digest.Append(bytes);
    });
    return digest.Finish();
}

std::string readBounded(const std::filesystem::path& path, std::uint64_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("shader input cannot be opened");
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > limit) throw std::runtime_error("shader input exceeds entry limit");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!file.seekg(0) || !file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("shader input cannot be read");
    return bytes;
}

std::string_view payloadOf(std::string_view file, std::string_view identity) {
    const auto end = file.find('\n');
    if (end == std::string_view::npos || end > HeaderLimit) throw std::runtime_error("invalid shader input header");
    std::istringstream header(std::string(file.substr(0, end)));
    std::string magic, host, checksum, extra;
    unsigned schema = 0;
    std::uint64_t size = 0;
    if (!(header >> magic >> schema >> host >> checksum >> size) || header >> extra || magic != "APS5INPUT" || schema != 1u || host != InputDialect || !supportedHost()) throw std::runtime_error("incompatible shader input schema, request dialect or host");
    const auto payload = file.substr(end + 1u);
    if (size != payload.size() || !validIdentity(checksum) || checksum != identity || ShaderInputInventory::Identity(payload) != checksum) throw std::runtime_error("shader input checksum mismatch");
    return payload;
}

}

struct ShaderInputInventory::State {
    std::filesystem::path directory;
    ShaderInputInventoryOptions options;
    std::mutex mutex;
};

ShaderInputInventory::ShaderInputInventory(std::filesystem::path directory, ShaderInputInventoryOptions options) : state(std::make_unique<State>()) {
    if (options.maxEntryBytes == 0u || options.maxEntries == 0u || options.maxTotalBytes < options.maxEntryBytes || options.maxEntryBytes > 256u * 1024u * 1024u) throw std::invalid_argument("invalid shader input inventory limits");
    state->directory = std::move(directory);
    state->options = options;
}

ShaderInputInventory::~ShaderInputInventory() = default;

std::string ShaderInputInventory::Identity(std::string_view bytes) {
    Digest digest;
    digest.Append(bytes);
    return digest.Finish();
}

std::vector<ShaderInputRecord> ShaderInputInventory::List() const {
    std::vector<ShaderInputRecord> records;
    if (state->directory.empty()) return records;
    std::error_code error;
    if (!std::filesystem::exists(state->directory, error)) {
        if (error) throw std::runtime_error("shader input inventory cannot be inspected");
        return records;
    }
    std::uint64_t total = 0;
    for (std::filesystem::directory_iterator it(state->directory, error), end; !error && it != end; it.increment(error)) {
        const auto& entry = *it;
        if (entry.path().extension() != ".input") continue;
        const auto identity = entry.path().stem().string();
        if (!validIdentity(identity) || !entry.is_regular_file(error)) throw std::runtime_error("invalid shader input inventory entry");
        const auto bytes = entry.file_size(error);
        if (error || bytes > state->options.maxEntryBytes + HeaderLimit || bytes > state->options.maxTotalBytes - total || records.size() >= state->options.maxEntries) throw std::runtime_error("shader input inventory exceeds limits");
        total += bytes;
        records.push_back({identity, entry.path(), bytes});
    }
    if (error) throw std::runtime_error("shader input inventory cannot be listed");
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) { return left.identity < right.identity; });
    return records;
}

DeserializedRequest ShaderInputInventory::Load(const std::filesystem::path& path) const {
    const auto identity = path.stem().string();
    if (path.extension() != ".input" || !validIdentity(identity)) throw std::runtime_error("invalid shader input filename");
    const auto file = readBounded(path, state->options.maxEntryBytes + HeaderLimit);
    const auto payload = payloadOf(file, identity);
    if (payload.size() > state->options.maxEntryBytes) throw std::runtime_error("shader input exceeds entry limit");
    if (payload.size() < 8u) throw std::runtime_error("invalid shader input configuration");
    std::uint64_t configurationBytes = 0;
    for (unsigned i = 0; i < 8u; ++i) configurationBytes |= static_cast<std::uint64_t>(static_cast<unsigned char>(payload[i])) << (i * 8u);
    if (configurationBytes > 65536u || configurationBytes > payload.size() - 8u) throw std::runtime_error("invalid shader input configuration");
    const auto prefix = configurationPrefix();
    if (payload.substr(0, 8u + configurationBytes) != prefix) throw std::runtime_error("shader input compiler configuration differs from current configuration");
    const auto serialized = payload.substr(8u + configurationBytes);
    if (!validRequestDialect(serialized)) throw std::runtime_error("incompatible shader input request dialect");
    auto request = RequestSerializer{}.DeserializeBinary(serialized);
    if (preflight(request.request, state->options.maxEntryBytes) != ShaderInputCaptureStatus::Stored) throw std::runtime_error("invalid shader input snapshot");
    return request;
}

ShaderInputCaptureStatus ShaderInputInventory::Store(const RecompileRequest& request) {
    if (state->directory.empty()) return ShaderInputCaptureStatus::Disabled;
    const auto status = preflight(request, state->options.maxEntryBytes);
    if (status != ShaderInputCaptureStatus::Stored) return status;
    try {
        std::uint64_t size = 0;
        static_cast<void>(requestIdentity(request, state->options.maxEntryBytes, size));
        return StoreBinary(RequestSerializer{}.SerializeBinary(request));
    } catch (const std::length_error&) { return ShaderInputCaptureStatus::TooLarge; }
}

ShaderInputCaptureStatus ShaderInputInventory::StoreBinary(std::string_view bytes) {
    if (state->directory.empty()) return ShaderInputCaptureStatus::Disabled;
    if (bytes.size() > state->options.maxEntryBytes) return ShaderInputCaptureStatus::TooLarge;
    if (!validRequestDialect(bytes)) return ShaderInputCaptureStatus::Invalid;
    try {
        const auto request = RequestSerializer{}.DeserializeBinary(bytes);
        const auto status = preflight(request.request, state->options.maxEntryBytes);
        if (status != ShaderInputCaptureStatus::Stored) return status;
    } catch (const std::exception&) { return ShaderInputCaptureStatus::Invalid; }
    std::string payload;
    try {
        payload = configurationPrefix();
        if (payload.size() > state->options.maxEntryBytes - bytes.size()) return ShaderInputCaptureStatus::TooLarge;
        payload.append(bytes);
    } catch (const std::length_error&) { return ShaderInputCaptureStatus::TooLarge; }
    const auto identity = Identity(payload);
    const auto path = state->directory / (identity + ".input");
    std::lock_guard lock(state->mutex);
    try {
        const DirectoryLock directoryLock(state->directory);
        if (!directoryLock.Acquired()) return ShaderInputCaptureStatus::IoError;
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            const auto existing = readBounded(path, state->options.maxEntryBytes + HeaderLimit);
            if (payloadOf(existing, identity) != payload) return ShaderInputCaptureStatus::Invalid;
            return ShaderInputCaptureStatus::Duplicate;
        }
        if (error) return ShaderInputCaptureStatus::IoError;
        const auto records = List();
        std::uint64_t total = 0;
        for (const auto& record : records) total += record.bytes;
        const auto header = "APS5INPUT 1 " + std::string(InputDialect) + " " + identity + " " + std::to_string(payload.size()) + "\n";
        if (records.size() >= state->options.maxEntries || header.size() + payload.size() > state->options.maxTotalBytes - total) return ShaderInputCaptureStatus::Full;
        std::string file = header;
        file.append(payload);
        if (!WriteFileAtomically(path, std::as_bytes(std::span(file.data(), file.size())))) return ShaderInputCaptureStatus::IoError;
        return ShaderInputCaptureStatus::Stored;
    } catch (const std::exception&) { return ShaderInputCaptureStatus::IoError; }
}

std::filesystem::path ShaderInputInventory::DefaultDirectory() {
    const auto disabled = [](const char* name) { const auto* value = std::getenv(name); return value != nullptr && *value != '\0' && std::string_view(value) != "0"; };
    if (disabled("ANYPS5_NO_SHADER_INPUTS") || disabled("APS5_NO_SHADER_INPUTS")) return {};
#ifdef _WIN32
    if (const auto* directory = _wgetenv(L"ANYPS5_SHADER_INPUT_DIR"); directory != nullptr && *directory != L'\0') return directory;
    std::wstring executable(260u, L'\0');
    for (;;) {
        const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (size == 0) return {};
        if (size < executable.size()) { executable.resize(size); break; }
        if (executable.size() >= 32768u) return {};
        executable.resize(executable.size() * 2u);
    }
    return std::filesystem::path(executable).parent_path() / "shader_inputs";
#else
    if (const auto* directory = std::getenv("ANYPS5_SHADER_INPUT_DIR"); directory != nullptr && *directory != '\0') return directory;
    std::error_code error;
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    const auto executable = std::filesystem::canonical(buffer.data(), error);
#else
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
#endif
    if (error) return {};
    return executable.parent_path() / "shader_inputs";
#endif
}

namespace {

std::uint64_t runtimeMegabytes(const char* name, std::uint64_t fallback, std::uint64_t maximum) {
    const auto* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback * 1024u * 1024u;
    std::uint64_t value = 0;
    const auto* end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0u || value > maximum) {
        std::fprintf(stderr, "[shader-inputs] invalid %s; using %llu MiB default\n", name, static_cast<unsigned long long>(fallback));
        value = fallback;
    }
    return value * 1024u * 1024u;
}

ShaderInputInventoryOptions runtimeOptions() {
    ShaderInputInventoryOptions options;
    options.maxEntryBytes = runtimeMegabytes("ANYPS5_SHADER_INPUT_MAX_ENTRY_MB", 16u, 64u);
    return options;
}

class CaptureStore {
public:
    CaptureStore() : options(runtimeOptions()), inventory(ShaderInputInventory::DefaultDirectory(), options), enabled(!ShaderInputInventory::DefaultDirectory().empty()), queueLimit(runtimeMegabytes("ANYPS5_SHADER_INPUT_QUEUE_MB", 32u, 256u)) {
        misconfigured = enabled && options.maxEntryBytes > queueLimit;
        if (misconfigured) {
            std::fprintf(stderr, "[shader-inputs] capture disabled: entry limit exceeds queue limit\n");
            enabled = false;
        }
        if (enabled) std::thread([this] { run(); }).detach();
    }

    bool ShouldCapture(const std::shared_ptr<const RecompileResult>& result) {
        if (!enabled && !misconfigured) return false;
        if (result != nullptr && result->inputProvenance != nullptr && result->inputProvenance->Load() != nullptr) return false;
        std::lock_guard lock(mutex);
        if (cooling(result)) { ++totals.cooldown; return false; }
        if (misconfigured || DebugProbeActive()) { refuse(result, ShaderInputCaptureStatus::Invalid); return false; }
        if (pendingEntries >= 8u || pendingBytes >= queueLimit || seen.size() >= options.maxEntries) { refuse(result, ShaderInputCaptureStatus::Full); return false; }
        return true;
    }

    ShaderInputCaptureStatus Capture(const RecompileRequest& request, const std::shared_ptr<const RecompileResult>& result, bool completeSnapshot) {
        if (misconfigured) { std::lock_guard lock(mutex); refuse(result, ShaderInputCaptureStatus::Invalid); return ShaderInputCaptureStatus::Invalid; }
        if (!enabled) return ShaderInputCaptureStatus::Disabled;
        if (!completeSnapshot || DebugProbeActive()) { std::lock_guard lock(mutex); refuse(result, ShaderInputCaptureStatus::Invalid); return ShaderInputCaptureStatus::Invalid; }
        if (result != nullptr && result->inputProvenance != nullptr && result->inputProvenance->Load() != nullptr) {
            std::lock_guard lock(mutex);
            count(ShaderInputCaptureStatus::Duplicate);
            return ShaderInputCaptureStatus::Duplicate;
        }
        {
            std::lock_guard lock(mutex);
            if (const auto reason = cooling(result)) { ++totals.cooldown; return *reason; }
        }
        const auto checked = preflight(request, options.maxEntryBytes);
        if (checked != ShaderInputCaptureStatus::Stored) { std::lock_guard lock(mutex); refuse(result, checked); return checked; }
        {
            std::lock_guard lock(mutex);
            if (pendingEntries >= 8u || pendingBytes >= queueLimit) { refuse(result, ShaderInputCaptureStatus::Full); return ShaderInputCaptureStatus::Full; }
        }
        std::uint64_t size = 0;
        std::string identity;
        try { identity = requestIdentity(request, options.maxEntryBytes, size); }
        catch (const std::length_error&) { std::lock_guard lock(mutex); refuse(result, ShaderInputCaptureStatus::TooLarge); return ShaderInputCaptureStatus::TooLarge; }
        {
            std::lock_guard lock(mutex);
            if (seen.contains(identity)) {
                associate(result, identity);
                count(ShaderInputCaptureStatus::Duplicate);
                return ShaderInputCaptureStatus::Duplicate;
            }
            if (seen.size() >= options.maxEntries || pendingEntries >= 8u || size > queueLimit - pendingBytes) { refuse(result, ShaderInputCaptureStatus::Full); return ShaderInputCaptureStatus::Full; }
            pendingAssociations.emplace(identity, PendingAssociation{std::make_shared<const std::string>(identity), {}});
            try { seen.insert(identity); }
            catch (...) { pendingAssociations.erase(identity); throw; }
            ++pendingEntries;
            pendingBytes += size;
        }
        try {
            auto bytes = RequestSerializer{}.SerializeBinary(request);
            const auto prefix = configurationPrefix();
            Digest digest;
            digest.Append(prefix);
            digest.Append(bytes);
            if (prefix.size() + bytes.size() != size || digest.Finish() != identity) throw std::runtime_error("shader input snapshot changed during capture");
            std::lock_guard lock(mutex);
            associate(result, identity);
            jobs.push_back({identity, std::move(bytes), size});
            ++totals.queued;
            wake.notify_one();
        } catch (...) {
            std::lock_guard lock(mutex);
            finishPending(identity, false, ShaderInputCaptureStatus::Invalid);
            seen.erase(identity);
            --pendingEntries;
            pendingBytes -= size;
            count(ShaderInputCaptureStatus::Invalid);
            idle.notify_all();
            return ShaderInputCaptureStatus::Invalid;
        }
        return ShaderInputCaptureStatus::Queued;
    }

    ShaderInputCaptureCounters Totals() {
        std::lock_guard lock(mutex);
        auto result = totals;
        result.pendingBytes = pendingBytes;
        result.pendingEntries = pendingEntries;
        return result;
    }

    void Flush() {
        std::unique_lock lock(mutex);
        idle.wait(lock, [&] { return pendingEntries == 0u; });
    }
private:
    std::optional<ShaderInputCaptureStatus> cooling(const std::shared_ptr<const RecompileResult>& result) {
        if (result == nullptr || result->inputProvenance == nullptr) return std::nullopt;
        const auto found = retries.find(result->inputProvenance.get());
        if (found == retries.end()) return std::nullopt;
        if (found->second.slot.lock() != result->inputProvenance || found->second.after <= std::chrono::steady_clock::now()) {
            retries.erase(found);
            return std::nullopt;
        }
        return found->second.reason;
    }

    void remember(const std::shared_ptr<ShaderInputProvenance>& slot, ShaderInputCaptureStatus reason) noexcept {
        if (slot == nullptr) return;
        try {
            const auto now = std::chrono::steady_clock::now();
            if (retries.size() >= 1024u && !retries.contains(slot.get())) {
                std::erase_if(retries, [&](const auto& entry) { return entry.second.slot.expired() || entry.second.after <= now; });
                if (retries.size() >= 1024u) retries.erase(retries.begin());
            }
            retries[slot.get()] = {slot, now + std::chrono::seconds(5), reason};
        } catch (...) {}
    }

    void refuse(const std::shared_ptr<const RecompileResult>& result, ShaderInputCaptureStatus reason) {
        count(reason);
        if (result != nullptr) remember(result->inputProvenance, reason);
    }

    void associate(const std::shared_ptr<const RecompileResult>& result, const std::string& identity) {
        if (result == nullptr || result->inputProvenance == nullptr || result->inputProvenance->Load() != nullptr) return;
        const auto found = pendingAssociations.find(identity);
        if (found == pendingAssociations.end()) {
            static_cast<void>(result->inputProvenance->Assign(std::make_shared<const std::string>(identity)));
            return;
        }
        if (pendingAssociationCount >= options.maxEntries) { count(ShaderInputCaptureStatus::Full); return; }
        found->second.slots.push_back(result->inputProvenance);
        ++pendingAssociationCount;
        static_cast<void>(result->inputProvenance->Assign(found->second.identity));
    }

    void finishPending(const std::string& identity, bool success, ShaderInputCaptureStatus reason = ShaderInputCaptureStatus::IoError) {
        const auto found = pendingAssociations.find(identity);
        if (found == pendingAssociations.end()) return;
        if (!success) {
            for (const auto& slot : found->second.slots) {
                if (const auto live = slot.lock()) {
                    if (live->Clear(found->second.identity)) remember(live, reason);
                }
            }
        }
        pendingAssociationCount -= found->second.slots.size();
        pendingAssociations.erase(found);
    }

    void count(ShaderInputCaptureStatus status) {
        switch (status) {
        case ShaderInputCaptureStatus::Stored: ++totals.stored; break;
        case ShaderInputCaptureStatus::Duplicate: ++totals.duplicate; break;
        case ShaderInputCaptureStatus::TooLarge: ++totals.tooLarge; break;
        case ShaderInputCaptureStatus::Full: ++totals.full; break;
        case ShaderInputCaptureStatus::Invalid: ++totals.invalid; break;
        case ShaderInputCaptureStatus::IoError: ++totals.ioErrors; break;
        default: break;
        }
        if (status != ShaderInputCaptureStatus::Stored && status != ShaderInputCaptureStatus::Duplicate && status != ShaderInputCaptureStatus::Disabled) {
            const auto failures = totals.tooLarge + totals.full + totals.invalid + totals.ioErrors;
            if (failures == 1u || (failures & (failures - 1u)) == 0u) std::fprintf(stderr, "[shader-inputs] incomplete observed coverage: %llu oversized, %llu admission/quota refusals, %llu invalid, %llu I/O failures\n", static_cast<unsigned long long>(totals.tooLarge), static_cast<unsigned long long>(totals.full), static_cast<unsigned long long>(totals.invalid), static_cast<unsigned long long>(totals.ioErrors));
        }
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return !jobs.empty(); });
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            ShaderInputCaptureStatus status = ShaderInputCaptureStatus::IoError;
            try { status = inventory.StoreBinary(job.bytes); } catch (...) {}
            {
                std::lock_guard lock(mutex);
                count(status);
                const bool success = status == ShaderInputCaptureStatus::Stored || status == ShaderInputCaptureStatus::Duplicate;
                finishPending(job.identity, success, status);
                if (!success) seen.erase(job.identity);
                --pendingEntries;
                pendingBytes -= job.reservedBytes;
                idle.notify_all();
            }
        }
    }

    struct Job { std::string identity; std::string bytes; std::uint64_t reservedBytes = 0; };
    struct PendingAssociation { std::shared_ptr<const std::string> identity; std::vector<std::weak_ptr<ShaderInputProvenance>> slots; };
    struct Retry { std::weak_ptr<ShaderInputProvenance> slot; std::chrono::steady_clock::time_point after; ShaderInputCaptureStatus reason; };
    ShaderInputInventoryOptions options;
    ShaderInputInventory inventory;
    bool enabled;
    std::uint64_t queueLimit;
    bool misconfigured = false;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Job> jobs;
    std::set<std::string> seen;
    std::unordered_map<std::string, PendingAssociation> pendingAssociations;
    std::unordered_map<const ShaderInputProvenance*, Retry> retries;
    std::uint64_t pendingAssociationCount = 0;
    std::uint64_t pendingBytes = 0;
    std::uint64_t pendingEntries = 0;
    ShaderInputCaptureCounters totals;
};

CaptureStore& captureStore() {
    static auto* store = new CaptureStore;
    return *store;
}

}

bool ShouldCaptureShaderInput(const std::shared_ptr<const RecompileResult>& result) noexcept {
    try { return captureStore().ShouldCapture(result); }
    catch (...) { return false; }
}

ShaderInputCaptureStatus CaptureShaderInput(const RecompileRequest& request, const std::shared_ptr<const RecompileResult>& result, bool completeSnapshot) noexcept {
    try { return captureStore().Capture(request, result, completeSnapshot); }
    catch (const std::length_error&) { return ShaderInputCaptureStatus::TooLarge; }
    catch (...) { return ShaderInputCaptureStatus::Invalid; }
}

std::string FindShaderInputIdentity(const std::shared_ptr<const RecompileResult>& result) {
    return result != nullptr ? FindShaderInputIdentity(*result) : std::string{};
}

std::string FindShaderInputIdentity(const RecompileResult& result) {
    const auto identity = result.inputProvenance != nullptr ? result.inputProvenance->Load() : nullptr;
    return identity != nullptr ? *identity : std::string{};
}
ShaderInputCaptureCounters ShaderInputCaptureTotals() { return captureStore().Totals(); }
void FlushShaderInputs() { captureStore().Flush(); }

}

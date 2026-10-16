#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeStore.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace AgcDriver::Graphics {

namespace {

bool validIdentity(const std::string& identity) {
    return identity.size() == 32 && std::ranges::all_of(identity, [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path) {
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        OVERLAPPED overlapped{};
        if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped)) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
#else
        descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor >= 0 && ::flock(descriptor, LOCK_EX) != 0) { ::close(descriptor); descriptor = -1; }
#endif
    }

    ~FileLock() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) { OVERLAPPED overlapped{}; UnlockFileEx(handle, 0, 1, 0, &overlapped); CloseHandle(handle); }
#else
        if (descriptor >= 0) { ::flock(descriptor, LOCK_UN); ::close(descriptor); }
#endif
    }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    [[nodiscard]] bool Held() const {
#ifdef _WIN32
        return handle != INVALID_HANDLE_VALUE;
#else
        return descriptor >= 0;
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int descriptor = -1;
#endif
};

struct Entry {
    std::string identity;
    std::uint64_t bytes;
    std::filesystem::file_time_type used;
};

bool entries(const std::filesystem::path& directory, std::vector<Entry>& result, std::size_t maximum, bool newest, std::uint64_t& count, std::uint64_t& total, const std::string& exclude = {}, bool strict = false) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) return false;
    for (const auto end = std::filesystem::directory_iterator(); iterator != end; iterator.increment(error)) {
        if (error) return false;
        const auto& file = *iterator;
        if (file.path().extension() != ".recipe" || !validIdentity(file.path().stem().string())) continue;
        if (file.path().stem().string() == exclude) continue;
        if (!std::filesystem::is_regular_file(file.symlink_status(error)) || error) { if (strict) return false; error.clear(); continue; }
        const auto bytes = file.file_size(error);
        if (error) { if (strict) return false; error.clear(); continue; }
        const auto used = file.last_write_time(error);
        if (error) { if (strict) return false; error.clear(); continue; }
        if (bytes > std::numeric_limits<std::uint64_t>::max() - total) return false;
        total += bytes;
        ++count;
        if (maximum == 0) continue;
        const auto preferred = [newest](const auto& left, const auto& right) { return left.used != right.used ? (newest ? left.used > right.used : left.used < right.used) : left.identity < right.identity; };
        Entry entry{file.path().stem().string(), bytes, used};
        const auto position = std::lower_bound(result.begin(), result.end(), entry, preferred);
        if (result.size() < maximum || position != result.end()) {
            result.insert(position, std::move(entry));
            if (result.size() > maximum) result.pop_back();
        }
    }
    if (error) return false;
    return true;
}

void cleanPending(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    for (const auto end = std::filesystem::directory_iterator(); !error && iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        if (entry.path().extension() != ".tmp" || !entry.path().filename().string().starts_with("pending-")) continue;
        if (std::filesystem::is_regular_file(entry.symlink_status(error)) && !error) std::filesystem::remove(entry.path(), error);
        error.clear();
    }
}

bool read(const std::filesystem::path& path, std::size_t maximum, std::vector<std::byte>& bytes) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) return false;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > maximum) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file && file.peek() == std::char_traits<char>::eof();
}

void touch(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
}

bool write(const std::filesystem::path& directory, const std::filesystem::path& destination, std::span<const std::byte> bytes) {
    static std::atomic<std::uint64_t> sequence{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = ::getpid();
#endif
    const auto temporary = directory / ("pending-" + std::to_string(process) + "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
    std::error_code error;
    if (std::filesystem::exists(temporary, error) || error) return false;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) { file.close(); std::filesystem::remove(temporary, error); return false; }
        file.close();
        if (!file) { std::filesystem::remove(temporary, error); return false; }
    }
#ifdef _WIN32
    const bool renamed = MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!renamed) { std::filesystem::remove(temporary, error); return false; }
#else
    std::filesystem::rename(temporary, destination, error);
    if (error) { std::filesystem::remove(temporary, error); return false; }
#endif
    return true;
}

PipelineRecipeStoreStatus load(const std::filesystem::path& directory, const PipelineRecipeStoreLimits& limits, const std::string& identity, PipelineRecipe& recipe, bool inputsOnly) {
    if (!validIdentity(identity)) return PipelineRecipeStoreStatus::Rejected;
    const auto path = directory / (identity + ".recipe");
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) return PipelineRecipeStoreStatus::Failed;
    if (!exists) return PipelineRecipeStoreStatus::Absent;
    std::vector<std::byte> bytes;
    if (!read(path, limits.entryBytes, bytes) || PipelineRecipeIdentity(bytes) != identity) return PipelineRecipeStoreStatus::Rejected;
    if (!(inputsOnly ? DecodePipelineRecipeInputs(bytes, recipe) : DecodePipelineRecipe(bytes, recipe))) return PipelineRecipeStoreStatus::Rejected;
    touch(path);
    return PipelineRecipeStoreStatus::Loaded;
}

}

PipelineRecipeStore::PipelineRecipeStore(std::filesystem::path directory, PipelineRecipeStoreLimits limits) : directory(std::move(directory)), limits(limits) {
    if (this->directory.empty() || limits.entries > 65536u || limits.entryBytes == 0 || limits.entryBytes > PipelineRecipeMaxBytes) throw std::invalid_argument("invalid pipeline recipe store limits");
}

PipelineRecipeStoreStatus PipelineRecipeStore::Store(const PipelineRecipe& recipe) {
    try {
        const auto encoded = EncodePipelineRecipe(recipe);
        if (limits.entries == 0 || encoded.size() > limits.entryBytes || encoded.size() > limits.bytes) return PipelineRecipeStoreStatus::Full;
        const auto identity = PipelineRecipeIdentity(encoded);
        std::lock_guard lock(mutex);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return PipelineRecipeStoreStatus::Failed;
        FileLock fileLock(directory / "store.lock");
        if (!fileLock.Held()) return PipelineRecipeStoreStatus::Failed;
        cleanPending(directory);
        const auto destination = directory / (identity + ".recipe");
        std::vector<std::byte> previous;
        if (read(destination, limits.entryBytes, previous)) {
            if (previous == encoded) { touch(destination); return PipelineRecipeStoreStatus::Present; }
            if (PipelineRecipeIdentity(previous) == identity) return PipelineRecipeStoreStatus::Rejected;
        }
        for (;;) {
            std::vector<Entry> stored;
            std::uint64_t count = 0, total = 0;
            if (!entries(directory, stored, 64u, false, count, total, identity)) return PipelineRecipeStoreStatus::Failed;
            if (count < limits.entries && total <= limits.bytes - encoded.size()) break;
            if (!limits.evict) return PipelineRecipeStoreStatus::Full;
            if (stored.empty()) return PipelineRecipeStoreStatus::Failed;
            for (const auto& entry : stored) {
                if (count < limits.entries && total <= limits.bytes - encoded.size()) break;
                std::filesystem::remove(directory / (entry.identity + ".recipe"), error);
                if (error) return PipelineRecipeStoreStatus::Failed;
                total -= entry.bytes;
                --count;
            }
        }
        return write(directory, destination, encoded) ? PipelineRecipeStoreStatus::Stored : PipelineRecipeStoreStatus::Failed;
    } catch (const std::exception&) {
        return PipelineRecipeStoreStatus::Failed;
    }
}

PipelineRecipeStoreStatus PipelineRecipeStore::Load(const std::string& identity, PipelineRecipe& recipe) const {
    std::lock_guard lock(mutex);
    return load(directory, limits, identity, recipe, false);
}

PipelineRecipeStoreStatus PipelineRecipeStore::LoadInputs(const std::string& identity, PipelineRecipe& recipe) const {
    std::lock_guard lock(mutex);
    return load(directory, limits, identity, recipe, true);
}

std::vector<std::string> PipelineRecipeStore::List(std::size_t maximum) const {
    std::lock_guard lock(mutex);
    std::vector<Entry> stored;
    std::uint64_t totalCount = 0, totalBytes = 0;
    const auto bound = std::min(maximum, limits.entries);
    if (!entries(directory, stored, bound, true, totalCount, totalBytes)) return {};
    const auto count = stored.size();
    std::vector<std::string> result;
    result.reserve(count);
    std::uint64_t bytes = 0;
    for (const auto& entry : stored) {
        if (result.size() == count) break;
        if (entry.bytes > limits.entryBytes || entry.bytes > limits.bytes - bytes) continue;
        bytes += entry.bytes;
        result.push_back(entry.identity);
    }
    return result;
}

std::vector<std::string> PipelineRecipeStore::ListForPreparation(std::size_t maximum) const {
    if (maximum > 65536u) throw std::invalid_argument("pipeline preparation count exceeds limit");
    std::lock_guard lock(mutex);
    std::vector<Entry> stored;
    std::uint64_t count = 0, total = 0;
    if (!entries(directory, stored, maximum, true, count, total, {}, true)) throw std::runtime_error("cannot enumerate pipeline recipe inputs");
    if (count > maximum || count > limits.entries || total > limits.bytes) throw std::runtime_error("pipeline recipe inputs exceed preparation bounds");
    std::vector<std::string> result;
    result.reserve(stored.size());
    for (const auto& entry : stored) {
        if (entry.bytes == 0 || entry.bytes > limits.entryBytes) throw std::runtime_error("pipeline recipe input exceeds entry bound");
        result.push_back(entry.identity);
    }
    return result;
}

}

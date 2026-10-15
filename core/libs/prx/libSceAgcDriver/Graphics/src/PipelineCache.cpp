#include "prx/libSceAgcDriver/Graphics/include/PipelineCache.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint32_t FileMagic = 0x50565041u;
constexpr std::uint32_t FileFormat = 1;

bool profiling() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

std::mutex& pathMutex(const std::filesystem::path& path) {
    struct Registry {
        std::mutex mutex;
        std::map<std::filesystem::path, std::mutex> paths;
    };
    static auto& registry = *new Registry;
    std::lock_guard lock(registry.mutex);
    return registry.paths[path];
}

}

PipelineCache::PipelineCache(const Context& context, const VkPhysicalDeviceProperties& properties) : context(context), properties(properties) {
    static_assert(sizeof(FileHeader) == 24);
    const auto directory = ShaderRecompiler::ShaderCacheDirectory();
    if (!directory.empty()) {
        std::string uuid;
        for (const auto byte : properties.pipelineCacheUUID) {
            char digits[3];
            std::snprintf(digits, sizeof(digits), "%02x", static_cast<unsigned>(byte));
            uuid += digits;
        }
        char name[160];
        std::snprintf(name, sizeof(name), "vk-pipelines-%04x-%04x-%08x-%s.bin", properties.vendorID, properties.deviceID, properties.driverVersion, uuid.c_str());
        path = directory / name;
        adoptEarlierDriverFile(directory, uuid);
    }
    std::vector<std::byte> initialData;
    if (!path.empty()) {
        std::lock_guard pathLock(pathMutex(path));
        load(initialData);
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = initialData.size();
    info.pInitialData = initialData.empty() ? nullptr : initialData.data();
    const auto create = context.Function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache");
    VkResult result = create(context.device, &info, nullptr, &cache);
    if (result != VK_SUCCESS && !initialData.empty()) {
        std::fprintf(stderr, "[pipeline-cache] the driver refused %s (Vulkan result %d); starting empty\n", path.string().c_str(), static_cast<int>(result));
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        loadedBytes = 0;
        settledBytes = 0;
        settledFile.reset();
        result = create(context.device, &info, nullptr, &cache);
    }
    Check(result, "vkCreatePipelineCache");
    if (!path.empty()) saver = std::thread([this] { run(); });
}

void PipelineCache::adoptEarlierDriverFile(const std::filesystem::path& directory, const std::string& uuid) {
    std::error_code error;
    if (!ShaderRecompiler::ShaderDiskCache::LayeredCache() || std::filesystem::exists(path, error)) return;
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "vk-pipelines-%04x-%04x-", properties.vendorID, properties.deviceID);
    const auto suffix = "-" + uuid + ".bin";
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.starts_with(prefix) || !name.ends_with(suffix) || !it->is_regular_file(error)) continue;
        std::error_code timeError;
        const auto written = std::filesystem::last_write_time(it->path(), timeError);
        if (timeError || (!newest.empty() && written <= newestTime)) continue;
        newest = it->path();
        newestTime = written;
    }
    if (newest.empty()) return;
    std::lock_guard pathLock(pathMutex(path));
    if (!std::filesystem::copy_file(newest, path, std::filesystem::copy_options::skip_existing, error) || error) return;
    std::fprintf(stderr, "[pipeline-cache] starting from %s, written by an earlier driver with the same pipeline cache UUID; the driver validates it again\n", newest.filename().string().c_str());
}

PipelineCache::~PipelineCache() {
    if (saver.joinable()) {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        saver.join();
        save(true);
    }
    context.Function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache")(context.device, cache, nullptr);
}

std::span<const std::byte> PipelineCache::File::Payload() const {
    return std::span(contents).subspan(sizeof(FileHeader));
}

PipelineCache::File PipelineCache::readFile() const {
    File file;
    if (!ShaderRecompiler::ReadWholeFile(path, file.contents)) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        file.state = exists || error ? FileState::Unreadable : FileState::Missing;
        return file;
    }
    file.state = FileState::Invalid;
    if (file.contents.size() < sizeof(FileHeader)) {
        file.invalidReason = "truncated";
        return file;
    }
    std::memcpy(&file.header, file.contents.data(), sizeof(FileHeader));
    if (file.header.magic != FileMagic || file.header.format != FileFormat) {
        file.invalidReason = "another format";
        return file;
    }
    if (file.header.dataBytes != file.contents.size() - sizeof(FileHeader)) {
        file.invalidReason = "truncated";
        return file;
    }
    const auto data = file.Payload();
    if (ShaderRecompiler::HashBytes(data) != file.header.dataHash) {
        file.invalidReason = "checksum mismatch";
        return file;
    }
    struct VulkanHeader {
        std::uint32_t headerSize;
        std::uint32_t headerVersion;
        std::uint32_t vendorID;
        std::uint32_t deviceID;
        std::uint8_t uuid[VK_UUID_SIZE];
    } vulkan{};
    static_assert(sizeof(VulkanHeader) == 32);
    if (data.size() < sizeof(vulkan)) {
        file.invalidReason = "no Vulkan header";
        return file;
    }
    std::memcpy(&vulkan, data.data(), sizeof(vulkan));
    if (vulkan.headerSize < sizeof(vulkan) || vulkan.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vulkan.vendorID != properties.vendorID || vulkan.deviceID != properties.deviceID || std::memcmp(vulkan.uuid, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        file.invalidReason = "made for another device or driver";
        return file;
    }
    file.state = FileState::Valid;
    return file;
}

std::optional<PipelineCache::FileHeader> PipelineCache::readHeader() const {
    std::ifstream file(path, std::ios::binary);
    FileHeader header{};
    if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) return std::nullopt;
    return header;
}

void PipelineCache::load(std::vector<std::byte>& initialData) {
    const auto file = readFile();
    if (file.state == FileState::Missing) return;
    if (file.state == FileState::Unreadable) {
        std::fprintf(stderr, "[pipeline-cache] cannot read %s; starting empty\n", path.string().c_str());
        return;
    }
    if (file.state == FileState::Invalid) {
        std::fprintf(stderr, "[pipeline-cache] ignoring %s: %s\n", path.string().c_str(), file.invalidReason);
        return;
    }
    const auto data = file.Payload();
    initialData.assign(data.begin(), data.end());
    loadedBytes = initialData.size();
    settledBytes = initialData.size();
    settledFile = file.header;
    std::fprintf(stderr, "[pipeline-cache] loaded %.1f KiB from %s\n", static_cast<double>(initialData.size()) / 1024.0, path.string().c_str());
}

bool PipelineCache::snapshot(bool final, std::vector<std::byte>* data, std::size_t& liveBytes) {
    const auto getData = context.Function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
    auto synchronizationLock = final ? synchronization.LockForSnapshot() : synchronization.TryLockForSnapshot();
    if (!synchronizationLock.owns_lock()) return false;
    liveBytes = 0;
    if (getData(context.device, cache, &liveBytes, nullptr) != VK_SUCCESS) liveBytes = 0;
    if (data == nullptr || liveBytes == 0) return true;
    data->resize(liveBytes);
    const auto result = getData(context.device, cache, &liveBytes, data->data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) liveBytes = 0;
    data->resize(liveBytes);
    return true;
}

bool PipelineCache::save(bool final) {
    std::lock_guard pathLock(pathMutex(path));
    std::size_t liveBytes = 0;
    if (!snapshot(final, nullptr, liveBytes)) {
        ++deferredSaves;
        return false;
    }
    if (liveBytes == 0) return true;
    if (liveBytes == settledBytes && readHeader() == settledFile) return true;
    std::vector<std::byte> live;
    if (!snapshot(final, &live, liveBytes)) {
        ++deferredSaves;
        return false;
    }
    if (live.empty()) return true;
    const auto deferred = deferredSaves;
    deferredSaves = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto settle = [&](const std::optional<FileHeader>& file) {
        settledBytes = live.size();
        settledFile = file;
    };
    const auto disk = readFile();
    if (disk.state == FileState::Unreadable) {
        std::fprintf(stderr, "[pipeline-cache] cannot read %s; not saving over it\n", path.string().c_str());
        settle(readHeader());
        return true;
    }
    if (disk.state == FileState::Invalid) std::fprintf(stderr, "[pipeline-cache] replacing %s: %s\n", path.string().c_str(), disk.invalidReason);
    std::size_t floor = 0;
    VkPipelineCache merged = VK_NULL_HANDLE;
    VkPipelineCache copied = VK_NULL_HANDLE;
    struct DestroyCaches {
        const Context& context;
        const VkPipelineCache& merged;
        const VkPipelineCache& copied;
        ~DestroyCaches() {
            const auto destroy = context.Function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache");
            if (merged != VK_NULL_HANDLE) destroy(context.device, merged, nullptr);
            if (copied != VK_NULL_HANDLE) destroy(context.device, copied, nullptr);
        }
    } destroyCaches{context, merged, copied};
    const auto createCache = context.Function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache");
    if (disk.state == FileState::Valid) {
        const auto data = disk.Payload();
        VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        info.initialDataSize = data.size();
        info.pInitialData = data.data();
        const auto result = createCache(context.device, &info, nullptr, &merged);
        if (result == VK_SUCCESS) {
            floor = std::max(loadedBytes, data.size());
        } else {
            merged = VK_NULL_HANDLE;
            std::fprintf(stderr, "[pipeline-cache] replacing %s: the driver refused it (Vulkan result %d)\n", path.string().c_str(), static_cast<int>(result));
        }
    }
    std::vector<std::byte> file;
    std::size_t size = 0;
    if (merged != VK_NULL_HANDLE) {
        VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        info.initialDataSize = live.size();
        info.pInitialData = live.data();
        auto result = createCache(context.device, &info, nullptr, &copied);
        if (result != VK_SUCCESS) {
            copied = VK_NULL_HANDLE;
            std::fprintf(stderr, "[pipeline-cache] cannot copy the live cache to merge it into %s (Vulkan result %d); not saving over it\n", path.string().c_str(), static_cast<int>(result));
            return true;
        }
        result = context.Function<PFN_vkMergePipelineCaches>("vkMergePipelineCaches")(context.device, merged, 1, &copied);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[pipeline-cache] cannot merge into %s (Vulkan result %d); not saving over it\n", path.string().c_str(), static_cast<int>(result));
            return true;
        }
        const auto getData = context.Function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
        if (getData(context.device, merged, &size, nullptr) != VK_SUCCESS || size == 0) return true;
        if (size == disk.header.dataBytes) {
            settle(disk.header);
            return true;
        }
        file.resize(sizeof(FileHeader) + size);
        result = getData(context.device, merged, &size, file.data() + sizeof(FileHeader));
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) return true;
        file.resize(sizeof(FileHeader) + size);
    } else {
        size = live.size();
        file.resize(sizeof(FileHeader) + size);
        std::memcpy(file.data() + sizeof(FileHeader), live.data(), size);
    }
    if (size < floor) {
        std::fprintf(stderr, "[pipeline-cache] keeping %s: the merged cache has %.1f KiB, less than the %.1f KiB on disk or loaded at start\n", path.string().c_str(), static_cast<double>(size) / 1024.0, static_cast<double>(floor) / 1024.0);
        settle(disk.header);
        return true;
    }
    const FileHeader header{FileMagic, FileFormat, size, ShaderRecompiler::HashBytes(std::span(file).subspan(sizeof(FileHeader)))};
    std::memcpy(file.data(), &header, sizeof(header));
    if (!ShaderRecompiler::WriteFileAtomically(path, file)) {
        std::fprintf(stderr, "[pipeline-cache] cannot write %s\n", path.string().c_str());
        return true;
    }
    settle(header);
    if (final || profiling()) std::fprintf(stderr, "[pipeline-cache] saved %.1f KiB to %s in %.1f ms%s%s; the snapshot waited for %llu attempts while pipelines were being created\n", static_cast<double>(size) / 1024.0, path.string().c_str(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), merged != VK_NULL_HANDLE ? " (merged with the file)" : "", final ? " (teardown)" : "", static_cast<unsigned long long>(deferred));
    return true;
}

void PipelineCache::run() {
    std::unique_lock lock(mutex);
    auto interval = std::chrono::milliseconds(std::chrono::seconds(SaveIntervalSeconds));
    while (!wake.wait_for(lock, interval, [&] { return stopping; })) {
        lock.unlock();
        const bool saved = save(false);
        lock.lock();
        interval = saved ? std::chrono::milliseconds(std::chrono::seconds(SaveIntervalSeconds)) : std::chrono::milliseconds(BusyRetryMilliseconds);
    }
}

}

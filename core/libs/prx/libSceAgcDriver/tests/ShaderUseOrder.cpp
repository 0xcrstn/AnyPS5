#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/PreparationQueue.hpp"
#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AgcDriver::DriverDetail;
using namespace ShaderRecompiler;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<int> drain(PreparationQueue<int>& queue) {
    std::vector<int> order;
    while (!queue.Empty()) order.push_back(queue.Pop());
    return order;
}

void queueOrder() {
    PreparationQueue<int> queue;
    const int owners[8] = {};
    using Queue = PreparationQueue<int>;
    queue.Push(1, &owners[1], Queue::Unranked, false, false);
    queue.Push(2, &owners[2], 500u, false, false);
    queue.Push(3, &owners[3], 20u, false, false);
    queue.Push(4, &owners[4], Queue::Unranked, false, false);
    queue.Push(5, &owners[5], 20u, false, false);
    queue.Push(6, &owners[6], 10u, false, true);
    queue.Push(7, &owners[7], Queue::Unranked, true, false);
    require(queue.Size() == 7u && queue.RankedSize() == 3u, "the queue miscounted its ranked tasks");
    require(drain(queue) == std::vector<int>{7, 3, 5, 2, 1, 4, 6}, "tasks were not taken demanded first, then by earliest first use with ties in arrival order, then in arrival order with tasks queued behind last");
    queue.Push(1, &owners[1], Queue::Unranked, false, false);
    queue.Push(2, &owners[2], 5u, false, false);
    queue.Push(3, &owners[3], Queue::Unranked, false, false);
    require(queue.Prioritize(&owners[3]) == std::optional<std::size_t>{2u}, "a waited-for task was not found at its position");
    require(queue.Prioritize(&owners[6]) == std::nullopt, "a task that is not queued was found");
    require(drain(queue) == std::vector<int>{3, 2, 1}, "a waited-for task did not move to the front");
}

void useRecords(const std::filesystem::path& directory) {
    const auto record = ShaderDiskCache::EncodeShaderUse({0x1234u, 77u});
    require(record.size() == 16u, "a use record is not 16 bytes");
    std::vector<std::byte> file;
    for (const auto& use : std::vector<ShaderDiskCache::ShaderUse>{{0x1234u, 77u}, {0x99u, 5u}, {0x1234u, 12u}, {0x1234u, 400u}}) {
        const auto bytes = ShaderDiskCache::EncodeShaderUse(use);
        file.insert(file.end(), bytes.begin(), bytes.end());
    }
    auto torn = ShaderDiskCache::EncodeShaderUse({0x555u, 1u});
    torn[3] = static_cast<std::byte>(static_cast<unsigned>(torn[3]) ^ 0xffu);
    file.insert(file.end(), torn.begin(), torn.end());
    file.push_back(std::byte{0x7f});
    std::size_t records = 0;
    const auto decoded = ShaderDiskCache::DecodeShaderUses(file, &records);
    require(records == 4u && decoded.size() == 2u, "a damaged or partial record was accepted");
    require(decoded.at(0x1234u) == 12u && decoded.at(0x99u) == 5u, "repeated records did not keep the earliest first use");
    const auto path = ShaderDiskCache::ShaderUseFile();
    require(path.filename() == "shader-use.bin" && std::filesystem::equivalent(path.parent_path(), directory), "use records are not kept beside the versioned cache directories");
    require(WriteFileAtomically(path, file), "the test could not write its use file");
    const auto& earlier = ShaderDiskCache::EarlierShaderUses();
    require(earlier.size() == 2u && earlier.at(0x1234u) == 12u, "earlier uses were not loaded");
    std::vector<std::byte> compacted;
    require(ReadWholeFile(path, compacted) && compacted.size() == 32u, "a file with repeated and damaged records was not compacted");
    ShaderDiskCache::StoreShaderUse({0x4242u, 3u});
    std::vector<std::byte> appended;
    require(ReadWholeFile(path, appended) && appended.size() == 48u, "a new first use was not appended");
    require(ShaderDiskCache::DecodeShaderUses(appended).at(0x4242u) == 3u, "the appended use does not read back");
}

void setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}

int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("agc_shader_use_order_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        queueOrder();
        std::filesystem::create_directories(directory);
        setEnvironment("ANYPS5_NO_SHADER_CACHE", "0");
        setEnvironment("ANYPS5_SHADER_CACHE_DIR", directory.string());
        useRecords(directory);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_shader_use_order_tests: %s\n", error.what());
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::puts("agc_shader_use_order_tests passed");
    return 0;
}

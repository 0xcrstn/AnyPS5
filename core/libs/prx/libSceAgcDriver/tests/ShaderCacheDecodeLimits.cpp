#include "ShaderDiskCache.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {

bool rejectLargeAllocation = false;
std::size_t largestAllocation = 0;

}

void* operator new(std::size_t bytes) {
    if (rejectLargeAllocation) {
        largestAllocation = std::max(largestAllocation, bytes);
        if (bytes > 1024u * 1024u) throw std::bad_alloc();
    }
    if (auto* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {

using namespace ShaderRecompiler;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> amplified(std::span<const std::byte> encoded, std::size_t offset, std::uint64_t count, std::size_t minimum) {
    require(offset + sizeof(count) <= encoded.size(), "invalid malicious-count fixture offset");
    std::vector<std::byte> bytes(encoded.begin(), encoded.begin() + offset);
    const auto position = bytes.size();
    bytes.resize(position + sizeof(count));
    std::memcpy(bytes.data() + position, &count, sizeof(count));
    if (count <= 2u * 1024u * 1024u) bytes.resize(bytes.size() + static_cast<std::size_t>(count) * minimum);
    bytes.insert(bytes.end(), encoded.begin() + offset + sizeof(count), encoded.end());
    return bytes;
}

template<typename TDecode>
void bounded(TDecode decode) {
    largestAllocation = 0;
    rejectLargeAllocation = true;
    bool accepted = false;
    bool threw = false;
    try { accepted = decode(); } catch (const std::bad_alloc&) { threw = true; }
    rejectLargeAllocation = false;
    require(!threw && largestAllocation <= 1024u * 1024u, "decoder attempted an amplified allocation before rejecting count");
    require(!accepted, "decoder accepted malicious count");
}

void resultCounts() {
    RecompileResult empty;
    empty.spirv = std::vector<std::uint32_t>{0x07230203u, 0x00010000u, 0u, 1u, 0u};
    std::vector<std::byte> encoded;
    ShaderDiskCache::EncodeResult(empty, encoded);
    auto one = empty;
    DescriptorBinding binding{};
    binding.kind = DescriptorKind::StorageBuffer;
    binding.role = DescriptorRole::ShaderData;
    binding.count = 1;
    one.bindings.push_back(binding);
    std::vector<std::byte> withBinding;
    ShaderDiskCache::EncodeResult(one, withBinding);
    const auto mismatch = std::mismatch(encoded.begin(), encoded.end(), withBinding.begin(), withBinding.end());
    const auto offset = static_cast<std::size_t>(mismatch.first - encoded.begin());
    require(offset < encoded.size(), "cannot locate encoded binding count");
    const auto malicious = amplified(encoded, offset, 131072u, 8u);
    bounded([&] { RecompileResult result; return ShaderDiskCache::DecodeResult(malicious, result); });
    const auto overflow = amplified(encoded, offset, std::numeric_limits<std::uint64_t>::max(), 8u);
    bounded([&] { RecompileResult result; return ShaderDiskCache::DecodeResult(overflow, result); });
    auto legitimate = empty;
    legitimate.bindings.assign(2048, binding);
    for (std::size_t index = 0; index < legitimate.bindings.size(); ++index) {
        legitimate.bindings[index].binding = static_cast<std::uint32_t>(index);
        legitimate.bindings[index].guestDescriptor.assign(16, static_cast<std::uint32_t>(index));
        legitimate.bindings[index].bufferWritten.assign(1, true);
    }
    std::vector<std::byte> large;
    ShaderDiskCache::EncodeResult(legitimate, large);
    RecompileResult decoded;
    require(ShaderDiskCache::DecodeResult(large, decoded), "allocation budget rejected legitimate large binding metadata");
    require(decoded.bindings.size() == legitimate.bindings.size() && decoded.bindings.back().guestDescriptor == legitimate.bindings.back().guestDescriptor, "allocation budget changed decoded binding metadata");
}

void planCounts() {
    IrResourcePlan plan;
    std::vector<std::byte> encoded;
    require(ShaderDiskCache::EncodePlan(plan, encoded), "cannot encode empty plan fixture");
    constexpr std::size_t RecordCountOffset = sizeof(std::uint32_t) * 4u + sizeof(std::uint64_t) * 2u;
    const auto malicious = amplified(encoded, RecordCountOffset, 2u * 1024u * 1024u, 6u);
    bounded([&] { IrResourcePlan result; return ShaderDiskCache::DecodePlan(malicious, result); });
    const auto overflow = amplified(encoded, RecordCountOffset, std::numeric_limits<std::uint64_t>::max(), 6u);
    bounded([&] { IrResourcePlan result; return ShaderDiskCache::DecodePlan(overflow, result); });
}

}

int main() {
    try {
        resultCounts();
        planCounts();
        std::cout << "shader cache decoder allocation limit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        rejectLargeAllocation = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}

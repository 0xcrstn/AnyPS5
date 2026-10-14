#include "Optimization/ResourceProgram.hpp"
#include "BdaAbi.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace ShaderRecompiler;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool SameBinding(const DescriptorBinding& a, const DescriptorBinding& b) {
    if (a.kind != b.kind || a.role != b.role || a.descriptorSet != b.descriptorSet || a.binding != b.binding || a.count != b.count || a.readOnly != b.readOnly || a.imageShape != b.imageShape) return false;
    if (a.guestDescriptor != b.guestDescriptor || a.samplerDepthCompare != b.samplerDepthCompare || a.imageWritten != b.imageWritten || a.imageDepthCompare != b.imageDepthCompare || a.imageAtomic != b.imageAtomic || a.imageAtomic64 != b.imageAtomic64) return false;
    if (a.bufferAtomic != b.bufferAtomic || a.bufferWritten != b.bufferWritten || a.samplerUnnormalized != b.samplerUnnormalized || a.samplerIntegerBorder != b.samplerIntegerBorder || a.imageUnnormalized != b.imageUnnormalized || a.imageUnnormalizedBaseLevel != b.imageUnnormalizedBaseLevel) return false;
    if (a.imageSamplers != b.imageSamplers || a.bufferRead != b.bufferRead || a.bufferReadRanges.size() != b.bufferReadRanges.size()) return false;
    for (std::size_t index = 0; index < a.bufferReadRanges.size(); ++index) {
        const auto& left = a.bufferReadRanges[index];
        const auto& right = b.bufferReadRanges[index];
        if (left.bounded != right.bounded || left.unbound != right.unbound || left.ranges != right.ranges) return false;
    }
    return true;
}

void RequireSameResult(const RecompileResult& a, const RecompileResult& b, const std::string& message) {
    Require(a.spirv == b.spirv && a.variantId == b.variantId && a.specializationId == b.specializationId && a.specialization == b.specialization && a.pushConstants == b.pushConstants, message + ": shader or push constants differ");
    Require(a.workgroupMemoryDwords == b.workgroupMemoryDwords && a.bdaWrites == b.bdaWrites && a.shaderDataUserWords == b.shaderDataUserWords && a.bufferBaseSlots == b.bufferBaseSlots && a.poisonedSrtReads == b.poisonedSrtReads, message + ": runtime metadata differs");
    Require(a.bindings.size() == b.bindings.size() && a.vertexAttributes.size() == b.vertexAttributes.size(), message + ": resource counts differ");
    for (std::size_t index = 0; index < a.bindings.size(); ++index) Require(SameBinding(a.bindings[index], b.bindings[index]), message + ": binding differs");
    for (std::size_t index = 0; index < a.vertexAttributes.size(); ++index) {
        const auto& left = a.vertexAttributes[index];
        const auto& right = b.vertexAttributes[index];
        Require(left.location == right.location && left.components == right.components && left.resource.fields == right.resource.fields && left.fetchIndex == right.fetchIndex && left.formatComponents == right.formatComponents, message + ": vertex attribute differs");
    }
}

void VerifyStorageReuse(const PreparedShaderInvocation& prepared, const ResourceCapture& capture, bool templates) {
    const bool reuse = templates && std::getenv("APS5_NO_MATERIALIZE_STORAGE_REUSE") == nullptr;
    auto changed = capture;
    changed.snapshot.buffers[0].dwords[0] += 512u;
    changed.snapshot.images[0].dwords[0] += 512u;
    changed.snapshot.userData.back() ^= 3u;
    const auto original = prepared.Materialize(capture);
    const auto updated = prepared.Materialize(changed);
    {
        std::shared_ptr<const RecompileResult> fromExitedThread;
        std::exception_ptr producerFailure;
        std::thread producer([&] {
            try {
                fromExitedThread = prepared.MaterializeValues(capture);
            } catch (...) {
                producerFailure = std::current_exception();
            }
        });
        producer.join();
        if (producerFailure) std::rethrow_exception(producerFailure);
        auto stillOwned = prepared.MaterializeValues(changed);
        RequireSameResult(*fromExitedThread, *original, "result retained after its producer thread exited");
        RequireSameResult(*stillOwned, *updated, "concurrent result has fresh values");
        auto anotherOwner = fromExitedThread;
        std::thread releaseOne([owned = std::move(anotherOwner)]() mutable { owned.reset(); });
        releaseOne.join();
        auto independent = prepared.MaterializeValues(changed);
        Require(independent.get() != fromExitedThread.get() && independent.get() != stillOwned.get(), "a result was reused while another owner retained it");
        RequireSameResult(*fromExitedThread, *original, "retained result changed after another owner's release");
        const auto* address = fromExitedThread.get();
        const auto* bindings = fromExitedThread->bindings.data();
        const auto* push = fromExitedThread->pushConstants.data();
        std::vector<const std::uint32_t*> descriptors;
        for (const auto& binding : fromExitedThread->bindings) descriptors.push_back(binding.guestDescriptor.data());
        const auto before = GetMaterializeValuesCounts();
        std::thread releaseLast([owned = std::move(fromExitedThread)]() mutable { owned.reset(); });
        releaseLast.join();
        const auto recycled = prepared.MaterializeValues(changed);
        if (reuse) {
            Require(recycled.get() == address, "last-owner release on another thread did not recycle the result");
            Require(recycled->bindings.data() == bindings && recycled->pushConstants.data() == push, "recycled result replaced its output vectors");
            for (std::size_t index = 0; index < descriptors.size(); ++index) Require(recycled->bindings[index].guestDescriptor.data() == descriptors[index], "recycled binding replaced its descriptor storage");
            Require(GetMaterializeValuesCounts().storageReused == before.storageReused + 1u, "cross-thread storage reuse was not counted");
        }
        RequireSameResult(*recycled, *updated, "recycled result has stale dynamic values");
        RequireSameResult(*stillOwned, *updated, "another retained result changed during recycling");
    }
    {
        auto occupied = prepared.MaterializeValues(capture);
        auto oversized = prepared.MaterializeValues(capture);
        Require(oversized.use_count() == 1, "oversized retention fixture does not own its result");
        auto writable = std::const_pointer_cast<RecompileResult>(std::move(oversized));
        writable->pushConstants.reserve(512u * 1024u);
        oversized = std::move(writable);
        const auto before = GetMaterializeValuesCounts();
        oversized.reset();
        if (reuse) {
            const auto after = GetMaterializeValuesCounts();
            Require(after.storageRefused == before.storageRefused + 1u && after.storageRetainedBytes == before.storageRetainedBytes, "oversized unused vector capacity was retained");
        }
        RequireSameResult(*occupied, *original, "oversized release changed a retained result");
    }
    {
        std::array<std::exception_ptr, 2> failures{};
        std::array<std::thread, 2> workers;
        for (std::size_t worker = 0; worker < workers.size(); ++worker) {
            workers[worker] = std::thread([&, worker] {
                try {
                    auto current = capture;
                    std::shared_ptr<const RecompileResult> previous;
                    std::optional<RecompileResult> expectedPrevious;
                    for (std::uint32_t iteration = 0; iteration < 16u; ++iteration) {
                        current.snapshot.buffers[0].dwords[0] = capture.snapshot.buffers[0].dwords[0] + (iteration + 1u + worker * 16u) * 256u;
                        current.snapshot.userData.back() = capture.snapshot.userData.back() + iteration + static_cast<std::uint32_t>(worker);
                        const auto values = prepared.MaterializeValues(current);
                        const auto reference = prepared.Materialize(current);
                        RequireSameResult(*values, *reference, "concurrent materialization differs");
                        if (previous) RequireSameResult(*previous, *expectedPrevious, "concurrent refill changed a result still owned");
                        previous = values;
                        expectedPrevious = *reference;
                    }
                } catch (...) {
                    failures[worker] = std::current_exception();
                }
            });
        }
        for (auto& worker : workers) worker.join();
        for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    }
    if (reuse) {
        auto evicted = prepared.MaterializeValues(capture);
        for (std::uint32_t stride = 1u; stride <= 40u; ++stride) {
            auto specialized = capture;
            specialized.snapshot.buffers[0].dwords[1] = (specialized.snapshot.buffers[0].dwords[1] & 0xffffu) | (stride << 16u);
            const auto values = prepared.MaterializeValues(specialized);
            RequireSameResult(*values, *prepared.Materialize(specialized), "storage refill after a specialization change");
        }
        RequireSameResult(*evicted, *original, "template eviction changed an owned result");
        const auto before = GetMaterializeValuesCounts();
        std::thread releaseEvicted([owned = std::move(evicted)]() mutable { owned.reset(); });
        releaseEvicted.join();
        Require(GetMaterializeValuesCounts().storageRetainedBytes == before.storageRetainedBytes, "evicted template kept recycled storage after its last owner released it");
        bool hit = false;
        const auto rebuilt = prepared.MaterializeValues(capture, &hit);
        Require(!hit, "storage lifetime fixture did not evict the original specialization");
        RequireSameResult(*rebuilt, *original, "recreated template has stale values");
    }
    Require(GetMaterializeValuesCounts().storageRetainedBytes <= 32u * 1024u * 1024u, "materialized storage exceeded its retained-byte budget");
}

void VerifyTemplates() {
    std::array<std::uint32_t, 32> table{};
    const auto tableAddress = reinterpret_cast<std::uintptr_t>(table.data());
    table[0] = 0x00123400u;
    table[1] = (56u << 20u) | (3u << 30u);
    table[2] = 3u << 14u;
    table[3] = 0xfacu | (9u << 28u);
    table[16] = 0x12340000u;
    table[18] = 16u;
    table[19] = 0xfacu;
    table[20] = 1u;
    std::array<std::uint32_t, 3> userData{static_cast<std::uint32_t>(tableAddress), static_cast<std::uint32_t>(tableAddress >> 32u), 7u};
    constexpr std::array<std::uint32_t, 16> code{
        0xf4000400u, 0xfa000050u, 0xf4080500u, 0xfa000020u, 0xf4080600u, 0xfa000040u, 0xbf8cc07fu,
        0xf40c0200u, 0xfa000000u, 0xbf8cc07fu, 0xf09c0f08u, 0x00a20000u, 0xbf8c3f70u, 0xe0700000u, 0x80060000u, 0xbf810000u
    };
    constexpr std::array<std::uint32_t, 4> capabilities{29u, spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    constexpr std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    RecompileRequest request{};
    request.shader = {ShaderStage::Compute, 0x46000u, code, 0, {}};
    request.context.waveSize = 32;
    request.context.userDataBaseRegister = 0;
    request.context.userData = userData;
    request.context.compute = ShaderComputeStageInfo{{32u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 32;
    request.target.supportedCapabilities = capabilities;
    request.target.supportedExtensions = extensions;
    request.target.bdaAbiVersion = BdaAbi::Version;
    request.target.fragmentShaderBarycentricEnabled = false;
    request.layout.pushConstantSizeBytes = 128;
    const auto handle = PrepareShader(request);
    const auto prepared = PreparedShaderInvocation::TryCreate(request, handle);
    Require(prepared.has_value(), "prepared materialization request does not match");
    SrtRuntime runtime{};
    runtime.userData = userData;
    runtime.userContext = &table;
    runtime.readMemory = [](void* context, std::uint64_t address, std::uint32_t* value) {
        const auto& words = *static_cast<const std::array<std::uint32_t, 32>*>(context);
        const auto begin = reinterpret_cast<std::uintptr_t>(words.data());
        if (address < begin || address - begin > sizeof(words) - sizeof(*value)) return false;
        std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value));
        return true;
    };
    runtime.readSpecializationMemory = runtime.readMemory;
    const auto captured = prepared->Capture(runtime);
    Require(!captured->snapshot.buffers.empty() && !captured->snapshot.images.empty() && !captured->snapshot.samplers.empty(), "materialization fixture did not capture all descriptor kinds");
    const bool templates = std::getenv("APS5_NO_MATERIALIZE_TEMPLATES") == nullptr;
    const auto check = [&](const ResourceCapture& capture, bool expectedHit, const std::string& message) {
        bool hit = false;
        const auto values = prepared->MaterializeValues(capture, &hit);
        const auto reference = prepared->Materialize(capture);
        Require(hit == (templates && expectedHit), message + ": unexpected template hit");
        RequireSameResult(*values, *reference, message);
    };
    check(*captured, false, "first materialization");
    const auto originalKey = SpecializationKey(*handle, request, captured->snapshot);
    auto changed = *captured;
    changed.snapshot.buffers[0].dwords[0] += 256u;
    changed.snapshot.buffers[0].dwords[1] ^= 1u;
    changed.snapshot.buffers[0].dwords[2] += 4u;
    changed.snapshot.images[0].dwords[0] += 256u;
    changed.snapshot.images[0].dwords[1] ^= 1u;
    changed.snapshot.userData.back() ^= 1u;
    if (!changed.snapshot.flattenedSrt.empty()) changed.snapshot.flattenedSrt.back() ^= 1u;
    Require(SpecializationKey(*handle, request, changed.snapshot) == originalKey, "descriptor values changed the specialization key");
    check(changed, true, "moved buffers and images with changed data");
    const auto specialized = [&](auto change, const std::string& message) {
        auto capture = *captured;
        change(capture.snapshot);
        Require(SpecializationKey(*handle, request, capture.snapshot) != originalKey, message + ": specialization bits were omitted from the key");
        check(capture, false, message);
        check(capture, true, message + " repeated");
    };
    specialized([](ResourceSnapshot& snapshot) { snapshot.buffers[0].dwords[1] ^= 1u << 16u; }, "buffer stride");
    specialized([](ResourceSnapshot& snapshot) { snapshot.buffers[0].dwords[0] += 1u; }, "buffer alignment");
    specialized([](ResourceSnapshot& snapshot) { snapshot.buffers[0].dwords[3] ^= 1u; }, "buffer format selectors");
    specialized([](ResourceSnapshot& snapshot) { snapshot.buffers[0].dwords[0] = 0u; snapshot.buffers[0].dwords[1] &= 0xffff0000u; }, "null buffer");
    specialized([](ResourceSnapshot& snapshot) { snapshot.images[0].dwords[2] ^= 1u; }, "image extent");
    specialized([](ResourceSnapshot& snapshot) { snapshot.images[0].dwords[0] = 0u; snapshot.images[0].dwords[1] &= ~0xffu; }, "null image");
    specialized([](ResourceSnapshot& snapshot) { snapshot.samplers[0].dwords[2] ^= 1u; }, "sampler state");
    auto offsetRequest = request;
    offsetRequest.layout.pushConstantOffsetBytes = 4u;
    offsetRequest.layout.pushConstantSizeBytes = 124u;
    const auto offsetPrepared = PreparedShaderInvocation::TryCreate(offsetRequest, handle);
    Require(offsetPrepared.has_value(), "push-offset request could not use the prepared shader");
    Require(SpecializationKey(*handle, offsetRequest, captured->snapshot) != originalKey, "push offset was omitted from the specialization key");
    bool hit = false;
    const auto offset = offsetPrepared->MaterializeValues(*captured, &hit);
    Require(!hit, "changed push offset reused a materialized template");
    RequireSameResult(*offset, *offsetPrepared->Materialize(*captured), "changed push offset");
    check(*captured, true, "original template after specialization misses");
    VerifyStorageReuse(*prepared, *captured, templates);
}

}

int main() {
    try {
        VerifyTemplates();
        std::cout << "materialized templates match full results across value and specialization changes" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

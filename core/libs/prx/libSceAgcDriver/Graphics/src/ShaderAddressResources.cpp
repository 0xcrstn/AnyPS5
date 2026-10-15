#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BdaResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ResidentMirrors.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ResidentStaging.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace AgcDriver::Graphics {

void ShaderResources::prepareAddressBindings(std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots) {
    static const bool loopGuarded = std::getenv("APS5_LOOP_GUARD") != nullptr;
    for (const auto& shader : shaders) {
        Require(shader.program != nullptr, "missing compiled shader");
        std::uint32_t tables = 0;
        std::uint32_t faults = 0;
        for (const auto& binding : shader.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::BdaPagetable && binding.role != ShaderRecompiler::DescriptorRole::FaultBuffer) continue;
            Require(binding.kind == ShaderRecompiler::DescriptorKind::StorageBuffer && binding.count == 1 && binding.guestDescriptor.empty() && !binding.readOnly, "invalid BDA descriptor contract");
            if (binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable) ++tables;
            else ++faults;
        }
        const bool faultOnly = tables == 0 && faults == 1 && shader.program->bdaAbiVersion == ShaderRecompiler::BdaAbi::Version;
        Require((tables == faults || faultOnly) && tables <= 1 && faults <= 1, "invalid BDA table and fault descriptors");
        Require(shader.program->bdaAbiVersion == (faults == 0 ? 0u : ShaderRecompiler::BdaAbi::Version), "incompatible BDA ABI version");
        // Rect-list validation needs a fault buffer, but never accesses guest addresses.
        usesBda = usesBda || tables != 0;
        usesFaultBuffer = usesFaultBuffer || faults != 0;
        const bool tableFaultsOnly = std::any_of(shader.program->bindings.begin(), shader.program->bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ImageTableMap; });
        faultChecks = faultChecks || tables != 0 || (faults != 0 && (!tableFaultsOnly || shader.program->imageTableFaults != 0 || shader.program->poisonedSrtReads != 0 || loopGuarded));
    }
    bdaReadOnly = usesBda && std::all_of(shaders.begin(), shaders.end(), [](const CompiledShader& shader) { return !shader.program->bdaWrites; });
    mirrorReads = bdaReadOnly && context.recorder != nullptr && ResidentMirrorsEnabled();
    placeWrites = usesBda && !bdaReadOnly && context.recorder != nullptr && PlacementSetting() != ShaderRecompiler::BdaPlacement::Mode::Host && std::all_of(shaders.begin(), shaders.end(), [](const CompiledShader& shader) { return !shader.program->bdaSpinWait; });
    if (usesBda) {
        Require(context.bufferDeviceAddress, "buffer device address is not enabled");
        static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        guestMemory.AcquireRegistered();
        const auto snapshotsStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        for (const auto& snapshot : snapshots) guestMemory.AddSnapshot(snapshot);
        if (profile) GuestBufferMemory::CountAddressBuild(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - snapshotsStart).count());
    }
}

void ShaderResources::UseMirrors(Recorder& recorder) const {
    if (bda != nullptr) Graphics::UseMirrors(recorder, bda->Mirrors());
    if (!bufferMirrors.empty()) Graphics::UseMirrors(recorder, bufferMirrors);
}

bool ShaderResources::HasPlacements() const {
    return bda != nullptr && bda->HasPlacements();
}

void ShaderResources::RecordPlacementCopyBack(Recorder&, VkCommandBuffer commands) const {
    if (bda == nullptr || !bda->HasPlacements()) return;
    bda->RecordPlacementCopyBack(context, commands);
    constexpr VkAccessFlags placedAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, placedAccess);
}

void ShaderResources::PublishResidentReads(Recorder& recorder) const {
    if (!AnyDeviceResident()) return;
    for (const auto& [begin, end] : guestMemory.DeviceReads()) {
        if (end > begin) PublishDeviceResident(recorder, begin, static_cast<std::size_t>(end - begin), ResidentSite::InPlace);
    }
}

void ShaderResources::RefreshMirrorsForDirectUse(Recorder& recorder) const {
    std::vector<std::shared_ptr<ResidentMirror>> mirrors(bufferMirrors);
    if (bda != nullptr) mirrors.insert(mirrors.end(), bda->Mirrors().begin(), bda->Mirrors().end());
    Graphics::RefreshMirrorsForDirectUse(recorder, mirrors);
}

VkDescriptorBufferInfo ShaderResources::descriptor(Allocation& allocation) {
    if (allocation.guest) return allocation.written ? guestMemory.Descriptor(allocation.address, allocation.size, allocation.adjustment) : guestMemory.MirroredDescriptor(allocation.address, allocation.size, allocation.adjustment, bufferMirrors);
    if (allocation.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
        Require(context.emptyBuffer != VK_NULL_HANDLE, "no placeholder buffer for a null V#");
        return {context.emptyBuffer, 0, allocation.size};
    }
    if (allocation.role == ShaderRecompiler::DescriptorRole::BdaPagetable || allocation.role == ShaderRecompiler::DescriptorRole::FaultBuffer) {
        Require(bda != nullptr, "BDA descriptors have no memory owner");
        return allocation.role == ShaderRecompiler::DescriptorRole::BdaPagetable ? bda->Table() : bda->Fault();
    }
    Require(allocation.buffer != nullptr, "shader data has no buffer owner");
    return {allocation.buffer->Handle(), 0, allocation.size};
}

}

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESIDENTMIRRORS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESIDENTMIRRORS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "BdaPlacement.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Graphics {

class Recorder;
struct HostImport;
struct ResidentMirror;

bool ResidentMirrorsEnabled();
bool BufferMirrorsEnabled();
std::shared_ptr<ResidentMirror> MirrorOfImport(const Context& context, const HostImport& import);
VkDeviceAddress MirrorDeviceAddress(const ResidentMirror& mirror);
ShaderRecompiler::BdaPlacement::Mode PlacementSetting();
std::shared_ptr<ResidentMirror> PlacementOfImport(const Context& context, const HostImport& import);
void CountPlacedBuild();
std::uint64_t PlacedBuilds();
VkDeviceAddress MirrorPristineAddress(const ResidentMirror& mirror);
void RecordPlacementCopyBack(const Context& context, VkCommandBuffer commands, VkDeviceAddress record, VkDeviceAddress table, bool verify);
VkBuffer MirrorBuffer(const ResidentMirror& mirror);
void UseMirrors(Recorder& recorder, std::span<const std::shared_ptr<ResidentMirror>> mirrors);
void RefreshMirrorsForDirectUse(Recorder& recorder, std::span<const std::shared_ptr<ResidentMirror>> mirrors);
void NoteMirroredWrite(std::uint64_t begin, std::uint64_t end);
void NotePointerWrite(const Recorder& recorder, std::uint64_t begin, std::uint64_t end);
void SubmitMirrors(Recorder& recorder);
void ForgetMirrors(VkDevice device, std::uint64_t begin, std::uint64_t end);
void ClearMirrors(VkDevice device);

struct ResidentMirrorCounters {
    std::uint64_t live = 0;
    std::uint64_t liveBytes = 0;
    std::uint64_t made = 0;
    std::uint64_t refused = 0;
    std::uint64_t uses = 0;
    std::uint64_t refreshes = 0;
    std::uint64_t refreshedBlocks = 0;
    std::uint64_t refreshedBytes = 0;
    std::uint64_t midBatchSyncs = 0;
    std::uint64_t midBatchBytes = 0;
    std::uint64_t invalidatedBlocks = 0;
    std::uint64_t deferred = 0;
    std::uint64_t denied = 0;
    std::uint64_t verified = 0;
    std::uint64_t verifyDiffering = 0;
    double collectMs = 0;
};
ResidentMirrorCounters MirrorTotals();

}

#endif

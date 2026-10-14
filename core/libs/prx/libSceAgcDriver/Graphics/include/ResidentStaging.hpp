#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESIDENTSTAGING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESIDENTSTAGING_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class Buffer;
class Recorder;

enum class ResidentSite : std::uint8_t { Hook, Scanout, InPlace, CopyIn, AddressBased, Indirect, Copy, Label, Upload, Completion, Eviction, Retire, Count };

bool LazyStagingOut();
bool LazyStagingVerify();
void NoteDeviceResident(VkDevice device, std::uint64_t begin, std::uint64_t end, std::shared_ptr<Buffer> shadow, std::uint64_t shadowBase, VkBuffer import, std::uint64_t importBase, std::uint64_t generation);
void RefreshDeviceResident(std::uint64_t begin, std::uint64_t end, std::uint64_t since, std::uint64_t generation);
bool AnyDeviceResident();
bool AnyDeviceResidentOverlaps(std::uint64_t address, std::size_t bytes);
std::vector<std::pair<std::uint64_t, std::uint64_t>> DeviceResidentRanges(VkDevice device);
std::uint64_t PublishDeviceResident(Recorder& recorder, std::uint64_t address, std::size_t bytes, ResidentSite site);
std::uint64_t PublishAllDeviceResident(Recorder& recorder, ResidentSite site);
std::uint64_t PublishDeviceResidentOfImport(Recorder& recorder, VkBuffer import, ResidentSite site);
void ClearDeviceResident(VkDevice device);

struct ResidentCounters {
    std::uint64_t live = 0;
    std::uint64_t liveBytes = 0;
    std::uint64_t liveBufferBytes = 0;
    std::uint64_t noted = 0;
    std::uint64_t notedBytes = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(ResidentSite::Count)> published{};
    std::array<std::uint64_t, static_cast<std::size_t>(ResidentSite::Count)> publishedBytes{};
    std::uint64_t refusedStoredOver = 0;
    std::uint64_t refusedStoredOverBytes = 0;
    std::uint64_t verified = 0;
    std::uint64_t verifiedBytes = 0;
    std::uint64_t verifyDiffering = 0;
    std::uint64_t verifyDifferingBytes = 0;
};
ResidentCounters DeviceResidentTotals();
std::string ResidentReport();
const char* ResidentSiteName(ResidentSite site);

}

#endif

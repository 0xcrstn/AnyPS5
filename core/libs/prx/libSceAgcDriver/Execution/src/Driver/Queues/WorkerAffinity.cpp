#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/HostThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask() {
    static const std::uint64_t mask = [] {
        if (std::getenv("APS5_NO_WORKER_AFFINITY") != nullptr) return std::uint64_t{0};
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_WORKER_AFFINITY_MASK");
        if (requested != 0) return requested;
#ifndef __linux__
        if (std::getenv("APS5_WORKER_AFFINITY") == nullptr) return std::uint64_t{0};
#endif
        const auto& layout = CpuTopology::Get();
        return layout.hybrid ? layout.performant : std::uint64_t{0};
    }();
    return mask;
}

void PinWorkerThread(const char* role) {
    NameHostThread(role);
    TracyZone::NameThread(role);
#ifdef _WIN32
    const HRESULT nameResult = ThreadName::SetCurrent(role);
    if (FAILED(nameResult)) std::fprintf(stderr, "[thread-name] could not name %s (0x%08lx)\n", role, static_cast<unsigned long>(nameResult));
#endif
    const auto mask = WorkerAffinityMask();
    if (mask == 0) return;
    static std::once_flag summary;
    std::call_once(summary, [mask] {
        const auto& layout = CpuTopology::Get();
        std::fprintf(stderr, "[affinity] queue workers and presenter -> 0x%llx (hybrid=%d efficient=0x%llx performant=0x%llx process=0x%llx)\n", static_cast<unsigned long long>(mask), layout.hybrid ? 1 : 0, static_cast<unsigned long long>(layout.efficient), static_cast<unsigned long long>(layout.performant), static_cast<unsigned long long>(layout.process));
    });
    CpuTopology::PinTraced(role, nullptr, mask);
}

std::uint64_t PreparationAffinityMask() {
    static const std::uint64_t mask = [] {
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_PREPARE_AFFINITY_MASK");
        if (requested != 0) return requested;
        const char* cores = std::getenv("APS5_PREPARE_AFFINITY");
        if (cores == nullptr || *cores == '\0') return std::uint64_t{0};
        const bool efficient = std::strcmp(cores, "efficient") == 0;
        if (!efficient && std::strcmp(cores, "performant") != 0) throw std::runtime_error(std::string("APS5_PREPARE_AFFINITY=") + cores + " is not efficient or performant");
        const auto& layout = CpuTopology::Get();
        if (!layout.hybrid) {
            std::fprintf(stderr, "[affinity] APS5_PREPARE_AFFINITY=%s: no hybrid CPU layout; shader preparation threads are not pinned\n", cores);
            return std::uint64_t{0};
        }
        return efficient ? layout.efficient : layout.performant;
    }();
    return mask;
}

void PinPreparationThread(const char* role) {
    const auto mask = PreparationAffinityMask();
    if (mask == 0) return;
    static std::once_flag summary;
    std::call_once(summary, [mask] {
        std::fprintf(stderr, "[affinity] shader preparation -> 0x%llx\n", static_cast<unsigned long long>(mask));
    });
    CpuTopology::PinTraced(role, nullptr, mask);
}

}

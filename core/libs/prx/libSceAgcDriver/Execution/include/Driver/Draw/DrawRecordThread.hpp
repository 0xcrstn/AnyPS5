#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWRECORDTHREAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWRECORDTHREAD_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DrawRecordEffects {
    std::uint64_t writeGenerationBefore = 0;
    std::uint64_t writeGenerationAfter = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> stores;
};

struct PreparedRead {
    std::uint64_t address;
    std::size_t bytes;
};

enum class DrawPrepareStale : std::uint8_t { No, PendingWrite, Stored };

inline DrawPrepareStale StaleDrawPrepare(const DrawRecordEffects& effects, std::span<const PreparedRead> reads, const std::function<bool(std::uint64_t, std::size_t)>& pendingWrite) {
    const bool noted = effects.writeGenerationAfter != effects.writeGenerationBefore;
    if (!noted && effects.stores.empty()) return DrawPrepareStale::No;
    for (const auto& read : reads) {
        if (read.bytes == 0) continue;
        const auto end = read.address + read.bytes;
        for (const auto& [begin, stop] : effects.stores) {
            if (begin < end && read.address < stop) return DrawPrepareStale::Stored;
        }
        if (noted && pendingWrite(read.address, read.bytes)) return DrawPrepareStale::PendingWrite;
    }
    return DrawPrepareStale::No;
}

inline DrawPrepareStale StaleAfterEach(std::span<const std::shared_ptr<const DrawRecordEffects>> jobs, std::span<const PreparedRead> reads, const std::function<bool(std::uint64_t, std::size_t)>& pendingWrite) {
    for (const auto& job : jobs) {
        const auto verdict = StaleDrawPrepare(*job, reads, pendingWrite);
        if (verdict != DrawPrepareStale::No) return verdict;
    }
    return DrawPrepareStale::No;
}

inline std::uint64_t AheadWaitTicket(std::uint64_t posted, std::uint64_t finished, std::uint32_t depth) {
    if (depth == 0 || posted - finished < depth) return 0;
    return posted + 1 - depth;
}

template <class TJob>
class PrepareAheadWindow {
public:
    struct Provisional {
        std::vector<PreparedRead> reads;
        std::vector<std::shared_ptr<const DrawRecordEffects>> earlier;
        std::uint64_t storeSequence = 0;
    };

    void Begin(std::uint64_t finished) {
        start = finished;
        while (!jobs.empty() && jobs.front()->ticket <= finished) jobs.pop_front();
        provisional = {};
    }
    void Add(std::shared_ptr<TJob> job) { jobs.push_back(std::move(job)); }
    std::uint64_t Start() const { return start; }
    std::size_t Size() const { return jobs.size(); }
    std::vector<std::shared_ptr<const DrawRecordEffects>> Between(std::uint64_t after, std::uint64_t through) const {
        std::vector<std::shared_ptr<const DrawRecordEffects>> effects;
        for (const auto& job : jobs) {
            if (job->ticket > after && job->ticket <= through) effects.emplace_back(job, &job->effects);
        }
        return effects;
    }
    DrawPrepareStale CheckFinished(std::span<const PreparedRead> reads, std::uint64_t finished, const std::function<bool(std::uint64_t, std::size_t)>& pendingWrite) const {
        const auto effects = Between(start, finished);
        return StaleAfterEach(effects, reads, pendingWrite);
    }
    bool Touched(std::uint64_t through) const {
        for (const auto& job : jobs) {
            if (job->ticket <= start || job->ticket > through) continue;
            if (job->effects.writeGenerationAfter != job->effects.writeGenerationBefore || !job->effects.stores.empty()) return true;
        }
        return false;
    }
    template <class TVisit>
    void ForFinished(std::uint64_t finished, TVisit&& visit) {
        for (const auto& job : jobs) {
            if (job->ticket <= finished) visit(*job);
        }
    }
    void Hold(Provisional held) { provisional = std::move(held); }
    Provisional Take() { return std::exchange(provisional, Provisional{}); }

private:
    std::deque<std::shared_ptr<TJob>> jobs;
    std::uint64_t start = 0;
    Provisional provisional;
};

enum class CommandProcessorReads : std::uint8_t { Drain, Overlap, Verify };

bool ParseMemoryPacketJobs(const char* text);
bool MemoryPacketJobs();
CommandProcessorReads ParseCommandProcessorReads(const char* text);
CommandProcessorReads CommandProcessorReadsMode();
void SetCommandProcessorReadsMode(CommandProcessorReads mode);

struct CommandProcessorReadCounters {
    std::uint64_t reads = 0;
    std::uint64_t ahead = 0;
    std::uint64_t verified = 0;
    std::uint64_t registerState = 0;
    std::uint64_t syncsDeferred = 0;
    std::uint64_t syncDrains = 0;
    std::uint64_t labelDrains = 0;
    std::uint64_t waitsSatisfied = 0;
};

CommandProcessorReadCounters CommandProcessorReadTotals();

struct LabelRecordJobCounters {
    std::uint64_t packets = 0;
    std::uint64_t jobs = 0;
    std::uint64_t labels = 0;
    std::uint64_t endOfPipe = 0;
    std::uint64_t memoryPackets = 0;
};

LabelRecordJobCounters LabelRecordJobTotals();

inline bool RegisterStatePacket(std::uint32_t header) {
    const auto opcode = (header >> 8u) & 0xffu;
    if (opcode == 0x11 || opcode == 0x12) return true;
    if (opcode != 0x10) return false;
    switch ((header >> 2u) & 0x3fu) {
    case 0x00: case 0x09: case 0x0b: case 0x0c: case 0x1a:
        return true;
    default:
        return false;
    }
}

inline bool CommandProcessorSyncPacket(std::uint32_t header) {
    const auto opcode = (header >> 8u) & 0xffu;
    return opcode == 0x42 || opcode == 0x46 || opcode == 0x58;
}

inline bool OverlapsDrawRecords(std::uint32_t opcode, bool predicated, bool dispatches = false) {
    if (predicated) return false;
    if (dispatches && (opcode == 0x15 || opcode == 0x16)) return true;
    switch (opcode) {
    case 0x13: case 0x26: case 0x28: case 0x2a: case 0x2f:
    case 0x69: case 0x76: case 0x79: case 0x7a:
    case 0x27: case 0x2d: case 0x35: case 0x24: case 0x25: case 0x2c: case 0x38:
        return true;
    default:
        return false;
    }
}

}

#endif

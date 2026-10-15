#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/PrepareWorkers.hpp"
#include "prx/libSceAgcDriver/Execution/include/RecordQueue.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using AgcDriver::RecordQueue;
using namespace AgcDriver::DriverDetail;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

bool NoPendingWrite(std::uint64_t, std::size_t) {
    return false;
}

enum class Kind : std::uint8_t { Draw, Dispatch };

struct Packet {
    Kind kind;
    int id;
    std::size_t reads = 0;
    std::optional<std::size_t> stores;
    std::uint32_t value = 0;
    FrozenFallback refused = FrozenFallback::None;
};

struct Prepared {
    std::uint64_t start = 0;
    std::uint32_t read = 0;
    FrozenFallback refused = FrozenFallback::None;
};

struct Recorded {
    Kind kind;
    int id;
    std::uint64_t ticket;
};

struct Guest {
    std::array<std::uint32_t, 8> memory{};
    std::array<bool, 8> pending{};
    std::mutex mutex;
    static std::uint64_t Address(std::size_t slot) { return 0x1000 + slot * sizeof(std::uint32_t); }
    std::uint32_t Read(std::size_t slot) {
        std::lock_guard lock(mutex);
        return memory[slot];
    }
    bool Pending(std::uint64_t address, std::size_t) {
        std::lock_guard lock(mutex);
        for (std::size_t slot = 0; slot < pending.size(); ++slot) {
            if (pending[slot] && Address(slot) == address) return true;
        }
        return false;
    }
};

void testMixedTicketsEmitInPacketOrder() {
    RecordQueue records;
    std::mutex logMutex;
    std::vector<Recorded> log;
    std::atomic<int> concurrent{0};
    std::atomic<int> peak{0};
    std::vector<Packet> packets;
    for (int id = 1; id <= 48; ++id) packets.push_back({id % 3 == 0 ? Kind::Draw : Kind::Dispatch, id});
    {
        OrderedPrepare<Packet, Prepared> pipeline(4, 6, [&](Packet& packet) {
            const auto now = ++concurrent;
            auto seen = peak.load();
            while (seen < now && !peak.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(packet.kind == Kind::Dispatch ? 1 + packet.id % 5 : 6 - packet.id % 5));
            --concurrent;
            return Prepared{records.Finished(), 0, FrozenFallback::None};
        }, [&](Packet& packet, std::optional<Prepared>& prepared, std::exception_ptr error) {
            Expect(error == nullptr && prepared.has_value(), "every mixed preparation succeeded");
            records.Post([&, packet] {
                std::lock_guard lock(logMutex);
                log.push_back({packet.kind, packet.id, records.Finished() + 1});
            });
        });
        for (const auto& packet : packets) pipeline.Submit(packet);
        pipeline.WaitEmitted();
    }
    records.RunPending();
    Expect(log.size() == packets.size(), "every draw and dispatch was recorded");
    bool ordered = log.size() == packets.size();
    for (std::size_t index = 0; ordered && index < log.size(); ++index) ordered = log[index].id == packets[index].id && log[index].kind == packets[index].kind && log[index].ticket == index + 1;
    Expect(ordered, "draws and dispatches share one ticket sequence and record in packet order");
    Expect(peak.load() >= 2, "dispatch and draw preparations ran concurrently");
}

void testStaleDispatchIsRedoneInOrder() {
    Guest guest;
    RecordQueue records;
    RecordHistory history(64);
    std::vector<std::uint32_t> recorded(5, 0xffffffffu);
    std::atomic<int> redoneStale{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FrozenFallback::Count)> refusals{};
    const std::array<Packet, 5> packets{
        Packet{Kind::Draw, 0, 0, 0, 7},
        Packet{Kind::Dispatch, 1, 0},
        Packet{Kind::Dispatch, 2, 1},
        Packet{Kind::Dispatch, 3, 1},
        Packet{Kind::Dispatch, 4, 5, std::nullopt, 0, FrozenFallback::FillHle},
    };
    {
        OrderedPrepare<std::size_t, Prepared> pipeline(3, 8, [&](std::size_t& index) {
            const auto& packet = packets[index];
            if (packet.refused != FrozenFallback::None) return Prepared{records.Finished(), 0, packet.refused};
            return Prepared{records.Finished(), guest.Read(packet.reads), FrozenFallback::None};
        }, [&](std::size_t& index, std::optional<Prepared>& prepared, std::exception_ptr error) {
            Expect(error == nullptr && prepared.has_value(), "the preparation succeeded");
            records.Post([&, index, prepared = *prepared] {
                const auto ticket = records.Finished() + 1;
                const auto& packet = packets[index];
                auto effects = std::make_shared<DrawRecordEffects>();
                effects->writeGenerationBefore = 1;
                effects->writeGenerationAfter = 1;
                auto value = prepared.read;
                const std::array<PreparedRead, 1> reads{PreparedRead{Guest::Address(packet.reads), 4}};
                if (prepared.refused != FrozenFallback::None) {
                    ++refusals[static_cast<std::size_t>(prepared.refused)];
                    value = guest.Read(packet.reads);
                } else if (CheckFrozenPrepare(history, prepared.start, ticket, reads, [&](std::uint64_t address, std::size_t bytes) { return guest.Pending(address, bytes); }) != FrozenPrepareVerdict::Fresh) {
                    ++redoneStale;
                    value = guest.Read(packet.reads);
                }
                recorded[index] = value;
                if (packet.stores) {
                    std::lock_guard lock(guest.mutex);
                    guest.memory[*packet.stores] = packet.value;
                    effects->stores.emplace_back(Guest::Address(*packet.stores), Guest::Address(*packet.stores) + 4);
                }
                if (index == 1) {
                    std::lock_guard lock(guest.mutex);
                    guest.pending[1] = true;
                    guest.memory[1] = 11;
                    effects->writeGenerationAfter = 2;
                }
                history.Note(ticket, effects);
            });
        });
        for (std::size_t index = 0; index < packets.size(); ++index) pipeline.Submit(index);
        pipeline.WaitEmitted();
    }
    records.RunPending();
    Expect(records.Finished() == packets.size(), "every record ran");
    Expect(recorded[0] == 0, "the draw reads before its own store");
    Expect(recorded[1] == 7, "a dispatch prepared before an earlier draw's store is redone and reads the stored value");
    Expect(recorded[2] == 11, "a dispatch reading a range an earlier dispatch left a pending GPU write over is redone");
    Expect(recorded[3] == 11, "a later dispatch over the same range is redone as well while the write is pending");
    Expect(redoneStale.load() == 3, "only the stale dispatch preparations were redone");
    Expect(refusals[static_cast<std::size_t>(FrozenFallback::FillHle)].load() == 1, "a dispatch refused by its worker is redone in order and counted by reason");
    Expect(std::strcmp(FrozenFallbackNames[static_cast<std::size_t>(FrozenFallback::FillHle)], "fill HLE") == 0 && std::strcmp(FrozenFallbackNames[static_cast<std::size_t>(FrozenFallback::CopyHle)], "copy HLE") == 0 && std::strcmp(FrozenFallbackNames[static_cast<std::size_t>(FrozenFallback::ArgumentRead)], "CPU argument read") == 0, "dispatch fallback reasons have their names");
}

void testStageCopyStoredByEarlierRecordIsRedone() {
    Guest guest;
    RecordQueue records;
    RecordHistory history(16);
    constexpr std::size_t SnapshotSlot = 2;
    constexpr std::size_t CopiedSlot = 6;
    std::uint32_t recordedCopy = 0xffffffffu;
    FrozenPrepareVerdict snapshotsOnly = FrozenPrepareVerdict::Unchecked;
    FrozenPrepareVerdict withCopies = FrozenPrepareVerdict::Unchecked;
    {
        OrderedPrepare<int, Prepared> pipeline(2, 4, [&](int& id) {
            return Prepared{records.Finished(), id == 2 ? guest.Read(CopiedSlot) : 0, FrozenFallback::None};
        }, [&](int& id, std::optional<Prepared>& prepared, std::exception_ptr error) {
            Expect(error == nullptr && prepared.has_value(), "the preparation succeeded");
            records.Post([&, id, prepared = *prepared] {
                const auto ticket = records.Finished() + 1;
                auto effects = std::make_shared<DrawRecordEffects>();
                if (id == 1) {
                    std::lock_guard lock(guest.mutex);
                    guest.memory[CopiedSlot] = 23;
                    effects->stores.emplace_back(Guest::Address(CopiedSlot), Guest::Address(CopiedSlot) + 4);
                } else {
                    const std::array<PreparedRead, 1> snapshots{PreparedRead{Guest::Address(SnapshotSlot), 4}};
                    const std::array<PreparedRead, 2> reads{PreparedRead{Guest::Address(SnapshotSlot), 4}, PreparedRead{Guest::Address(CopiedSlot), 4}};
                    snapshotsOnly = CheckFrozenPrepare(history, prepared.start, ticket, snapshots, NoPendingWrite);
                    withCopies = CheckFrozenPrepare(history, prepared.start, ticket, reads, NoPendingWrite);
                    recordedCopy = withCopies == FrozenPrepareVerdict::Fresh ? prepared.read : guest.Read(CopiedSlot);
                }
                history.Note(ticket, effects);
            });
        });
        pipeline.Submit(1);
        pipeline.Submit(2);
        pipeline.WaitEmitted();
    }
    records.RunPending();
    Expect(snapshotsOnly == FrozenPrepareVerdict::Fresh, "the snapshot ranges alone miss a store into a range stage A copied on the CPU");
    Expect(withCopies == FrozenPrepareVerdict::Stored, "a store by an earlier record into a range stage A copied makes the dispatch stale");
    Expect(recordedCopy == 23, "the stale dispatch is redone in order and copies the stored bytes");
}

void testInlineFallbackDrainsFirst() {
    RecordQueue records;
    records.Start([&] { records.RunPending(); });
    std::mutex logMutex;
    std::vector<int> log;
    std::atomic<bool> drainedBeforeInline{false};
    {
        OrderedPrepare<int, int> pipeline(3, 8, [&](int& id) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4 + id % 3));
            return id;
        }, [&](int& id, std::optional<int>&, std::exception_ptr) {
            records.Post([&, id] {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::lock_guard lock(logMutex);
                log.push_back(id);
            });
        });
        for (int id = 1; id <= 6; ++id) pipeline.Submit(id);
        pipeline.WaitEmitted();
        Expect(pipeline.Idle(), "an inline fallback drains the frozen preparations before it prepares");
        records.WaitIdle();
        {
            std::lock_guard lock(logMutex);
            drainedBeforeInline = log.size() == 6;
        }
        records.Post([&] {
            std::lock_guard lock(logMutex);
            log.push_back(100);
        });
        for (int id = 7; id <= 9; ++id) pipeline.Submit(id);
        pipeline.WaitEmitted();
    }
    records.WaitIdle();
    records.Stop();
    Expect(drainedBeforeInline.load(), "every earlier frozen record finished before the inline dispatch prepared");
    const std::vector<int> expected{1, 2, 3, 4, 5, 6, 100, 7, 8, 9};
    Expect(log == expected, "the inline dispatch records between the frozen packets around it");
}

}

int main() {
    testMixedTicketsEmitInPacketOrder();
    testStaleDispatchIsRedoneInOrder();
    testStageCopyStoredByEarlierRecordIsRedone();
    testInlineFallbackDrainsFirst();
    if (failures != 0) {
        std::fprintf(stderr, "%d prepare dispatch checks failed\n", failures);
        return 1;
    }
    std::printf("prepare dispatch checks passed\n");
    return 0;
}

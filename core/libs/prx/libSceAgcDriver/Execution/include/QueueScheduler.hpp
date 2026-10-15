#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUESCHEDULER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_QUEUESCHEDULER_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace AgcDriver {

enum class SchedulerMode : std::uint8_t { Off, Serial, Dag, Replay };

struct ScheduleRecording {
    std::string capture;
    std::string replay;
};

struct ParkedOp {
    std::uint32_t queue = 0;
    std::uint64_t submission = 0;
    std::size_t cursor = 0;
    std::string what;
    std::function<bool(std::function<void()>)> arm;
    std::function<void()> resume;
};

class QueueScheduler {
public:
    static constexpr std::uint32_t NoQueue = 0xffffffffu;

    struct Statistics {
        std::uint64_t turns = 0;
        std::uint64_t handoffs = 0;
        std::uint64_t blocks = 0;
        std::uint64_t parks = 0;
        std::uint64_t parksSettledAtArm = 0;
        double parkedMs = 0;
        double turnWaitMs = 0;
        std::uint64_t capturedTurns = 0;
        std::uint64_t capturedWaits = 0;
        std::uint64_t replayedTurns = 0;
        std::uint64_t replayedWaits = 0;
        std::uint64_t replayMismatches = 0;
    };

    class Turn {
    public:
        Turn() = default;
        Turn(const Turn&) = delete;
        Turn& operator=(const Turn&) = delete;
        Turn(Turn&& other) noexcept : scheduler(other.scheduler), queue(other.queue) { other.scheduler = nullptr; }
        Turn& operator=(Turn&&) = delete;
        ~Turn();

    private:
        friend class QueueScheduler;
        Turn(QueueScheduler* scheduler, std::uint32_t queue) : scheduler(scheduler), queue(queue) {}
        QueueScheduler* scheduler = nullptr;
        std::uint32_t queue = NoQueue;
    };

    class Blocked {
    public:
        Blocked() = default;
        Blocked(const Blocked&) = delete;
        Blocked& operator=(const Blocked&) = delete;
        Blocked(Blocked&& other) noexcept : scheduler(other.scheduler), queue(other.queue), depth(other.depth) { other.scheduler = nullptr; }
        Blocked& operator=(Blocked&&) = delete;
        ~Blocked();

    private:
        friend class QueueScheduler;
        Blocked(QueueScheduler* scheduler, std::uint32_t queue, std::uint32_t depth) : scheduler(scheduler), queue(queue), depth(depth) {}
        QueueScheduler* scheduler = nullptr;
        std::uint32_t queue = NoQueue;
        std::uint32_t depth = 0;
    };

    QueueScheduler(SchedulerMode mode, bool verify, const ScheduleRecording& recording = {});
    ~QueueScheduler();
    QueueScheduler(const QueueScheduler&) = delete;
    QueueScheduler& operator=(const QueueScheduler&) = delete;

    static SchedulerMode ParseMode(const char* value);
    static SchedulerMode ConfiguredMode();
    static bool ConfiguredVerify();
    static ScheduleRecording ConfiguredRecording();

    SchedulerMode Mode() const { return mode; }
    bool Verifying() const { return verify; }
    bool Capturing() const { return capture != nullptr; }
    bool Active() const { return mode != SchedulerMode::Off || verify || capture != nullptr; }
    void NoteWait(std::uint32_t queue, std::uint64_t value, std::uint32_t source);
    void Flush();

    Turn Begin(std::uint32_t queue, std::uint64_t submission, std::size_t cursor, std::uint64_t received);
    Blocked Block(std::uint32_t queue);
    void Park(ParkedOp op);
    void Close(std::exception_ptr reason);
    std::uint32_t Holder() const;
    Statistics Take();

private:
    struct Stream {
        std::uint64_t submission = 0;
        std::uint64_t index = 0;
        std::size_t cursor = 0;
        bool started = false;
        std::uint64_t received = 0;
        bool requesting = false;
        bool beginning = false;
        bool parked = false;
        bool woken = false;
        std::uint32_t depth = 0;
    };

    std::uint32_t lastHolder = NoQueue;

    using Position = std::tuple<std::uint32_t, std::uint64_t, std::size_t>;
    void acquireLocked(std::unique_lock<std::mutex>& lock, std::uint32_t queue, bool begin = false);
    bool serialOrder() const { return mode == SchedulerMode::Serial || mode == SchedulerMode::Replay; }
    void captureTurnLocked(const Stream& stream, std::uint32_t queue);
    void loadReplay(const std::string& path);
    std::string describe(const Position& position) const;
    void releaseLocked(std::uint32_t queue);
    void endTurn(std::uint32_t queue) noexcept;
    void reacquire(std::uint32_t queue, std::uint32_t depth) noexcept;
    bool grantableLocked(std::uint32_t queue) const;
    [[noreturn]] void fail(const std::string& what) const;
    void rethrowClosedLocked() const;

    const SchedulerMode mode;
    const bool verify;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<std::uint32_t, Stream> streams;
    std::uint32_t holder = NoQueue;
    std::exception_ptr closed;
    bool isClosed = false;
    Statistics statistics;
    std::FILE* capture = nullptr;
    std::vector<Position> replayTurns;
    std::size_t replayNext = 0;
    std::map<Position, std::pair<std::uint64_t, std::uint32_t>> replayWaits;
};

}

#endif

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_PREPARATIONADMISSION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_PREPARATIONADMISSION_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace AgcDriver::DriverDetail {

class PreparationAdmission {
public:
    struct Limits {
        std::size_t bytes = std::size_t{512} << 20u;
        std::size_t jobs = 64;
        std::uint64_t processBytes = 0;
    };

    struct Statistics {
        std::size_t bytes = 0;
        std::size_t jobs = 0;
        std::size_t peakBytes = 0;
        std::size_t peakJobs = 0;
        std::uint64_t admitted = 0;
        std::uint64_t deferred = 0;
        std::uint64_t demandBypasses = 0;
        std::uint64_t generation = 0;
        bool demandBypass = false;
        bool closed = false;
    };

private:
    struct State {
        explicit State(Limits limits) : limits(limits) {}
        const Limits limits;
        mutable std::mutex mutex;
        Statistics stats;
    };

public:
    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        Reservation(Reservation&& other) noexcept : state(std::move(other.state)), bytes(other.bytes), bypass(other.bypass) {}

        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                Reset();
                state = std::move(other.state);
                bytes = other.bytes;
                bypass = other.bypass;
            }
            return *this;
        }

        ~Reservation() {
            Reset();
        }

        explicit operator bool() const noexcept {
            return state != nullptr;
        }

        void Reset() noexcept {
            auto owned = std::move(state);
            if (owned == nullptr) return;
            std::lock_guard lock(owned->mutex);
            owned->stats.bytes -= bytes;
            --owned->stats.jobs;
            if (bypass) owned->stats.demandBypass = false;
            ++owned->stats.generation;
        }

    private:
        friend class PreparationAdmission;
        Reservation(std::shared_ptr<State> state, std::size_t bytes, bool bypass) : state(std::move(state)), bytes(bytes), bypass(bypass) {}
        std::shared_ptr<State> state;
        std::size_t bytes = 0;
        bool bypass = false;
    };

    explicit PreparationAdmission(Limits limits) : state(std::make_shared<State>(limits)) {
        if (limits.bytes == 0 || limits.jobs == 0) throw std::invalid_argument("invalid preparation admission limits");
    }

    PreparationAdmission(const PreparationAdmission&) = delete;
    PreparationAdmission& operator=(const PreparationAdmission&) = delete;

    Reservation TryAcquire(std::size_t bytes, bool demanded = false, std::uint64_t processBytes = 0, bool checkProcess = true) {
        std::lock_guard lock(state->mutex);
        auto& stats = state->stats;
        const auto& limits = state->limits;
        if (stats.closed) return {};
        const bool overflow = bytes > std::numeric_limits<std::size_t>::max() - stats.bytes || stats.jobs == std::numeric_limits<std::size_t>::max();
        const bool localFit = stats.jobs < limits.jobs && stats.bytes <= limits.bytes && bytes <= limits.bytes - stats.bytes;
        const bool processFit = !checkProcess || limits.processBytes == 0 || (processBytes <= limits.processBytes && stats.bytes <= limits.processBytes - processBytes && bytes <= limits.processBytes - processBytes - stats.bytes);
        const bool bypass = !localFit || !processFit;
        if (overflow || (bypass && (!demanded || stats.demandBypass))) {
            ++stats.deferred;
            return {};
        }
        stats.bytes += bytes;
        ++stats.jobs;
        ++stats.admitted;
        if (stats.bytes > stats.peakBytes) stats.peakBytes = stats.bytes;
        if (stats.jobs > stats.peakJobs) stats.peakJobs = stats.jobs;
        if (bypass) {
            stats.demandBypass = true;
            ++stats.demandBypasses;
        }
        return Reservation(state, bytes, bypass);
    }

    Statistics Counters() const {
        std::lock_guard lock(state->mutex);
        return state->stats;
    }

    void Close() {
        std::lock_guard lock(state->mutex);
        state->stats.closed = true;
        ++state->stats.generation;
    }

    static std::size_t Estimate(std::size_t codeBytes, std::size_t headerBytes) noexcept {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        constexpr std::size_t minimum = std::size_t{8} << 20u;
        constexpr std::size_t expansion = 64;
        if (headerBytes > maximum - minimum || codeBytes > (maximum - minimum - headerBytes) / expansion) return maximum;
        return minimum + headerBytes + codeBytes * expansion;
    }

private:
    const std::shared_ptr<State> state;
};

}

#endif

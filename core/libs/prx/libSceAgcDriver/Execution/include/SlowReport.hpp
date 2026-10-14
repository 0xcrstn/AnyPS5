#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SLOWREPORT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SLOWREPORT_HPP

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace AgcDriver {

inline double SlowReportThresholdMs() {
    static const double threshold = [] {
        const char* text = std::getenv("APS5_SLOW_REPORT_MS");
        if (text == nullptr || *text == '\0') return 250.0;
        char* end = nullptr;
        const double value = std::strtod(text, &end);
        if (end == text || *end != '\0' || value < 0) throw std::runtime_error("APS5_SLOW_REPORT_MS must be a number of milliseconds, 0 to disable: " + std::string(text));
        return value;
    }();
    return threshold;
}

template <typename TDescribe>
class SlowReport {
public:
    explicit SlowReport(TDescribe describe) : describe(std::move(describe)), start(std::chrono::steady_clock::now()) {}
    SlowReport(const SlowReport&) = delete;
    SlowReport& operator=(const SlowReport&) = delete;

    ~SlowReport() {
        const auto threshold = SlowReportThresholdMs();
        if (threshold == 0) return;
        const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (milliseconds < threshold) return;
        try {
            const std::string what = describe();
            std::fprintf(stderr, "[slow] %.0f ms: %s\n", milliseconds, what.c_str());
        } catch (...) {
        }
    }

private:
    TDescribe describe;
    std::chrono::steady_clock::time_point start;
};

}

#endif

#include "prx/libSceAgcDriver/Execution/include/WaitBroker.hpp"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver {

WaitBroker& WaitBroker::Instance() {
    static auto* const broker = new WaitBroker();
    return *broker;
}

bool WaitBroker::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_WAIT_BROKER");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

std::chrono::microseconds WaitBroker::LongestSleep() {
    static const std::chrono::microseconds longest = [] {
        const char* value = std::getenv("APS5_WAIT_BROKER_MAX_US");
        if (value == nullptr) return std::chrono::microseconds(2000);
        char* end = nullptr;
        const auto parsed = std::strtoll(value, &end, 10);
        if (end == value || *end != '\0' || parsed < 50 || parsed > 100000) throw std::runtime_error(std::string("APS5_WAIT_BROKER_MAX_US: expected 50 to 100000 microseconds, got ") + value);
        return std::chrono::microseconds(parsed);
    }();
    return longest;
}

}

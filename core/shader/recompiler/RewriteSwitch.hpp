#ifndef CORE_SHADER_RECOMPILER_REWRITESWITCH_HPP
#define CORE_SHADER_RECOMPILER_REWRITESWITCH_HPP

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ShaderRecompiler {

enum class RewriteMode : std::uint32_t { Off, On, Verify };

inline RewriteMode ParseRewriteMode(std::string_view name, const char* value) {
    if (value == nullptr) return RewriteMode::Off;
    const std::string_view text(value);
    if (text == "off") return RewriteMode::Off;
    if (text == "on") return RewriteMode::On;
    if (text == "verify") return RewriteMode::Verify;
    throw std::runtime_error(std::string(name) + " must be off, on or verify, not \"" + std::string(text) + "\"");
}

inline RewriteMode LazyLog2RepairSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_LAZY_LOG2_REPAIR", std::getenv("APS5_LAZY_LOG2_REPAIR"));
    return mode;
}

inline RewriteMode LazyTrigSeriesSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_LAZY_TRIG_SERIES", std::getenv("APS5_LAZY_TRIG_SERIES"));
    return mode;
}

inline RewriteMode BdaSpanHelpersSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_BDA_SPAN_HELPERS", std::getenv("APS5_BDA_SPAN_HELPERS"));
    return mode;
}

}

#endif

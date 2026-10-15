#ifndef CORE_SHADER_RECOMPILER_REWRITESWITCH_HPP
#define CORE_SHADER_RECOMPILER_REWRITESWITCH_HPP

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ShaderRecompiler {

enum class RewriteMode : std::uint32_t { Off, On, Verify };

inline RewriteMode ParseRewriteMode(std::string_view name, const char* value, RewriteMode fallback = RewriteMode::Off) {
    if (value == nullptr) return fallback;
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

inline RewriteMode BdaWriteNotesSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_BDA_WRITE_NOTES", std::getenv("APS5_BDA_WRITE_NOTES"), RewriteMode::On);
    return mode;
}

inline RewriteMode FlushProofsSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_FLUSH_PROOFS", std::getenv("APS5_FLUSH_PROOFS"), RewriteMode::On);
    return mode;
}

inline RewriteMode BdaProvenanceSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_BDA_PROVENANCE", std::getenv("APS5_BDA_PROVENANCE"), RewriteMode::On);
    return mode;
}

inline RewriteMode WindowRegistersSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_WINDOW_REGISTERS", std::getenv("APS5_WINDOW_REGISTERS"), RewriteMode::On);
    return mode;
}

inline RewriteMode BallotReuseSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_BALLOT_REUSE", std::getenv("APS5_BALLOT_REUSE"), RewriteMode::On);
    return mode;
}

inline RewriteMode InterpolationFactsSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_INTERPOLATION_FACTS", std::getenv("APS5_INTERPOLATION_FACTS"), RewriteMode::On);
    return mode;
}

inline RewriteMode BufferSelectsSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_BUFFER_SELECTS", std::getenv("APS5_BUFFER_SELECTS"), RewriteMode::On);
    return mode;
}

inline RewriteMode BdaSpanHelpersSetting() {
    static const RewriteMode mode = ParseRewriteMode("APS5_BDA_SPAN_HELPERS", std::getenv("APS5_BDA_SPAN_HELPERS"), RewriteMode::On);
    return mode;
}

}

#endif

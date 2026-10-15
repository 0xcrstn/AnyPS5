#include "SpirvBackend/SpirvModuleShape.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace ShaderRecompiler {

namespace {

struct Instruction {
    spv::Op op;
    std::span<const std::uint32_t> words;
};

std::vector<Instruction> Instructions(std::span<const std::uint32_t> words) {
    if (words.size() < 5u || words[0] != spv::MagicNumber) throw std::runtime_error("SPIR-V module has no valid header");
    std::vector<Instruction> instructions;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        if (count == 0u || count > words.size() - cursor) throw std::runtime_error("SPIR-V module has a truncated instruction");
        instructions.push_back({static_cast<spv::Op>(words[cursor] & 0xffffu), words.subspan(cursor, count)});
        cursor += count;
    }
    return instructions;
}

std::string LiteralString(std::span<const std::uint32_t> words, std::size_t& index) {
    std::string text;
    while (index < words.size()) {
        const auto word = words[index++];
        for (std::uint32_t byte = 0; byte < 4u; ++byte) {
            const auto character = static_cast<char>((word >> (byte * 8u)) & 0xffu);
            if (character == '\0') return text;
            text += character;
        }
    }
    throw std::runtime_error("SPIR-V literal string is not terminated");
}

class InterfaceDescription {
public:
    explicit InterfaceDescription(std::span<const std::uint32_t> words) : instructions(Instructions(words)) {
        for (const auto& instruction : instructions) {
            const auto& w = instruction.words;
            switch (instruction.op) {
            case spv::OpDecorate:
            case spv::OpDecorateId:
                if (w.size() >= 3u) decorations[w[1]].push_back(decorationText(w.subspan(2), instruction.op == spv::OpDecorateId));
                break;
            case spv::OpMemberDecorate:
                if (w.size() >= 4u) decorations[w[1]].push_back("m" + std::to_string(w[2]) + ":" + decorationText(w.subspan(3), false));
                break;
            case spv::OpConstant:
                if (w.size() >= 4u) constants[w[2]] = numbers(w.subspan(3));
                break;
            case spv::OpSpecConstant:
            case spv::OpSpecConstantTrue:
            case spv::OpSpecConstantFalse:
                if (w.size() >= 3u) specConstants[w[2]] = instruction;
                break;
            default:
                if (w.size() >= 2u && isType(instruction.op)) types[w[1]] = instruction;
                break;
            }
        }
    }

    std::vector<std::string> Lines() {
        std::vector<std::string> lines;
        std::map<std::uint32_t, std::string> entryNames;
        for (const auto& instruction : instructions) {
            const auto& w = instruction.words;
            if (instruction.op == spv::OpCapability && w.size() >= 2u) lines.push_back("capability " + std::to_string(w[1]));
            else if (instruction.op == spv::OpExtension) {
                std::size_t index = 1;
                lines.push_back("extension " + LiteralString(w, index));
            } else if (instruction.op == spv::OpEntryPoint && w.size() >= 4u) {
                std::size_t index = 3;
                const auto name = LiteralString(w, index);
                entryNames[w[2]] = std::to_string(w[1]) + ":" + name;
                lines.push_back("entry " + entryNames[w[2]]);
            }
        }
        for (const auto& instruction : instructions) {
            const auto& w = instruction.words;
            if ((instruction.op == spv::OpExecutionMode || instruction.op == spv::OpExecutionModeId) && w.size() >= 3u) {
                const auto found = entryNames.find(w[1]);
                lines.push_back("mode " + (found != entryNames.end() ? found->second : std::string("?")) + " " + numbers(w.subspan(2)));
            } else if (instruction.op == spv::OpVariable && w.size() >= 4u) {
                const auto storage = static_cast<spv::StorageClass>(w[3]);
                if (storage == spv::StorageClassFunction || storage == spv::StorageClassPrivate) continue;
                lines.push_back("variable " + std::to_string(w[3]) + " " + typeName(w[1]) + decorated(w[2]));
            }
        }
        for (const auto& [id, instruction] : specConstants) lines.push_back("specialization " + typeName(instruction.words[1]) + " " + std::to_string(static_cast<std::uint32_t>(instruction.op)) + " " + numbers(instruction.words.subspan(3)) + decorated(id));
        std::sort(lines.begin(), lines.end());
        return lines;
    }

private:
    static bool isType(spv::Op op) {
        switch (op) {
        case spv::OpTypeVoid: case spv::OpTypeBool: case spv::OpTypeInt: case spv::OpTypeFloat: case spv::OpTypeVector:
        case spv::OpTypeMatrix: case spv::OpTypeImage: case spv::OpTypeSampler: case spv::OpTypeSampledImage: case spv::OpTypeArray:
        case spv::OpTypeRuntimeArray: case spv::OpTypeStruct: case spv::OpTypePointer: case spv::OpTypeFunction:
        case spv::OpTypeAccelerationStructureKHR: case spv::OpTypeForwardPointer:
            return true;
        default:
            return false;
        }
    }

    static std::string numbers(std::span<const std::uint32_t> words) {
        std::string text;
        for (const auto word : words) text += (text.empty() ? "" : ",") + std::to_string(word);
        return text;
    }

    std::string decorationText(std::span<const std::uint32_t> words, bool ids) {
        std::string text = std::to_string(words[0]);
        for (std::size_t index = 1; index < words.size(); ++index) text += "," + (ids ? std::string("id") : std::to_string(words[index]));
        return text;
    }

    std::string decorated(std::uint32_t id) {
        const auto found = decorations.find(id);
        if (found == decorations.end()) return {};
        auto list = found->second;
        std::sort(list.begin(), list.end());
        std::string text = " [";
        for (std::size_t index = 0; index < list.size(); ++index) text += (index == 0 ? "" : ";") + list[index];
        return text + "]";
    }

    std::string length(std::uint32_t id) {
        if (const auto constant = constants.find(id); constant != constants.end()) return constant->second;
        if (const auto spec = specConstants.find(id); spec != specConstants.end()) return "spec" + decorated(id);
        return "?";
    }

    std::string typeName(std::uint32_t id) {
        if (const auto known = names.find(id); known != names.end()) return known->second;
        if (!visiting.insert(id).second) return "recursive";
        const auto found = types.find(id);
        std::string text;
        if (found == types.end()) text = "unknown";
        else {
            const auto& w = found->second.words;
            switch (found->second.op) {
            case spv::OpTypeVoid: text = "void"; break;
            case spv::OpTypeBool: text = "bool"; break;
            case spv::OpTypeInt: text = "i" + numbers(w.subspan(2)); break;
            case spv::OpTypeFloat: text = "f" + numbers(w.subspan(2)); break;
            case spv::OpTypeVector: text = "v" + std::to_string(w[3]) + "(" + typeName(w[2]) + ")"; break;
            case spv::OpTypeMatrix: text = "m" + std::to_string(w[3]) + "(" + typeName(w[2]) + ")"; break;
            case spv::OpTypeImage: text = "image(" + typeName(w[2]) + "," + numbers(w.subspan(3)) + ")"; break;
            case spv::OpTypeSampler: text = "sampler"; break;
            case spv::OpTypeSampledImage: text = "sampled(" + typeName(w[2]) + ")"; break;
            case spv::OpTypeArray: text = "a" + length(w[3]) + "(" + typeName(w[2]) + ")"; break;
            case spv::OpTypeRuntimeArray: text = "ra(" + typeName(w[2]) + ")"; break;
            case spv::OpTypePointer: text = "p" + std::to_string(w[2]) + "(" + typeName(w[3]) + ")"; break;
            case spv::OpTypeStruct: {
                text = "s{";
                for (std::size_t member = 2; member < w.size(); ++member) text += (member == 2 ? "" : ",") + typeName(w[member]);
                text += "}";
                break;
            }
            default: text = "op" + std::to_string(static_cast<std::uint32_t>(found->second.op)); break;
            }
            text += decorated(id);
        }
        visiting.erase(id);
        names[id] = text;
        return text;
    }

    std::vector<Instruction> instructions;
    std::map<std::uint32_t, std::vector<std::string>> decorations;
    std::map<std::uint32_t, std::string> constants;
    std::map<std::uint32_t, Instruction> specConstants;
    std::map<std::uint32_t, Instruction> types;
    std::map<std::uint32_t, std::string> names;
    std::set<std::uint32_t> visiting;
};

}

SpirvModuleShape DescribeSpirvShape(std::span<const std::uint32_t> words) {
    SpirvModuleShape shape;
    shape.words = words.size();
    const auto instructions = Instructions(words);
    std::set<std::uint32_t> entries;
    for (const auto& instruction : instructions) {
        if (instruction.op == spv::OpEntryPoint && instruction.words.size() >= 3u) entries.insert(instruction.words[2]);
    }
    std::size_t functionWords = 0;
    bool entryFunction = false;
    bool inFunction = false;
    for (const auto& instruction : instructions) {
        const auto& w = instruction.words;
        if (instruction.op == spv::OpFunction && w.size() >= 3u) {
            inFunction = true;
            entryFunction = entries.contains(w[2]);
            functionWords = 0;
            ++shape.functions;
        }
        if (inFunction) functionWords += w.size();
        if (instruction.op == spv::OpSwitch) {
            const auto targets = w.size() >= 3u ? (w.size() - 3u) / 2u + 1u : 0u;
            ++shape.switches;
            shape.switchTargets += targets;
            shape.largestSwitchTargets = std::max(shape.largestSwitchTargets, targets);
        }
        if (instruction.op == spv::OpFunctionCall) ++shape.functionCalls;
        if (instruction.op == spv::OpFunctionEnd && inFunction) {
            inFunction = false;
            if (entryFunction) shape.entryFunctionWords += functionWords;
            else {
                shape.helperFunctionWords += functionWords;
                shape.largestHelperWords = std::max(shape.largestHelperWords, functionWords);
            }
        }
    }
    return shape;
}

std::string DescribeSpirvInterface(std::span<const std::uint32_t> words) {
    std::string text;
    for (const auto& line : InterfaceDescription(words).Lines()) text += line + "\n";
    return text;
}

std::optional<std::string> CompareSpirvInterfaces(std::span<const std::uint32_t> reference, std::span<const std::uint32_t> candidate) {
    const auto expected = InterfaceDescription(reference).Lines();
    const auto actual = InterfaceDescription(candidate).Lines();
    std::vector<std::string> missing, extra;
    std::set_difference(expected.begin(), expected.end(), actual.begin(), actual.end(), std::back_inserter(missing));
    std::set_difference(actual.begin(), actual.end(), expected.begin(), expected.end(), std::back_inserter(extra));
    if (missing.empty() && extra.empty()) return std::nullopt;
    std::string text;
    if (!missing.empty()) text += "only in the reference: " + missing.front() + (missing.size() > 1u ? " (+" + std::to_string(missing.size() - 1u) + " more)" : "");
    if (!extra.empty()) text += std::string(text.empty() ? "" : "; ") + "only in the candidate: " + extra.front() + (extra.size() > 1u ? " (+" + std::to_string(extra.size() - 1u) + " more)" : "");
    return text;
}

}

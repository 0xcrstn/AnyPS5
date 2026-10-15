#include "SpirvBackend/SpirvStatistics.hpp"
#include <spirv/unified1/spirv.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ShaderRecompiler {

namespace {

std::string DecodeString(std::span<const std::uint32_t> operands) {
    std::string text;
    for (const auto word : operands) {
        for (std::uint32_t byte = 0; byte < 4u; ++byte) {
            const auto character = static_cast<char>((word >> (byte * 8u)) & 0xffu);
            if (character == '\0') return text;
            text.push_back(character);
        }
    }
    return text;
}

bool IsConversion(std::uint32_t opcode) {
    switch (opcode) {
    case spv::OpConvertFToU:
    case spv::OpConvertFToS:
    case spv::OpConvertSToF:
    case spv::OpConvertUToF:
    case spv::OpUConvert:
    case spv::OpSConvert:
    case spv::OpFConvert:
    case spv::OpQuantizeToF16:
        return true;
    default:
        return false;
    }
}

bool IsFloatArithmetic(std::uint32_t opcode) {
    switch (opcode) {
    case spv::OpFNegate:
    case spv::OpFAdd:
    case spv::OpFSub:
    case spv::OpFMul:
    case spv::OpFDiv:
    case spv::OpFRem:
    case spv::OpFMod:
    case spv::OpExtInst:
        return true;
    default:
        return false;
    }
}

bool IsShuffle(std::uint32_t opcode) {
    switch (opcode) {
    case spv::OpGroupNonUniformShuffle:
    case spv::OpGroupNonUniformShuffleXor:
    case spv::OpGroupNonUniformShuffleUp:
    case spv::OpGroupNonUniformShuffleDown:
    case spv::OpGroupNonUniformBroadcast:
    case spv::OpGroupNonUniformBroadcastFirst:
    case spv::OpGroupNonUniformQuadBroadcast:
    case spv::OpGroupNonUniformQuadSwap:
        return true;
    default:
        return false;
    }
}

bool IsBallot(std::uint32_t opcode) {
    switch (opcode) {
    case spv::OpGroupNonUniformBallot:
    case spv::OpGroupNonUniformInverseBallot:
    case spv::OpGroupNonUniformBallotBitExtract:
    case spv::OpGroupNonUniformBallotBitCount:
    case spv::OpGroupNonUniformBallotFindLSB:
    case spv::OpGroupNonUniformBallotFindMSB:
        return true;
    default:
        return false;
    }
}

}

SpirvStatistics AnalyzeSpirv(std::span<const std::uint32_t> words) {
    if (words.size() < 5u || words[0] != spv::MagicNumber) throw std::runtime_error("SPIR-V statistics need a SPIR-V module");
    SpirvStatistics statistics;
    statistics.words = words.size();
    std::unordered_map<std::uint32_t, std::string> names;
    std::unordered_set<std::uint32_t> float64Types;
    std::unordered_set<std::uint32_t> arrayTypes;
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> pointerTypes;
    std::unordered_set<std::uint32_t> physicalPointers;
    std::string currentFunction;
    for (std::size_t at = 5; at < words.size();) {
        const auto opcode = words[at] & spv::OpCodeMask;
        const auto count = words[at] >> spv::WordCountShift;
        if (count == 0u || at + count > words.size()) throw std::runtime_error("SPIR-V statistics found a malformed instruction");
        const auto operands = words.subspan(at + 1u, count - 1u);
        ++statistics.instructions;
        if (!currentFunction.empty()) ++statistics.instructionsByFunction[currentFunction];
        const auto resultType = operands.empty() ? 0u : operands[0];
        switch (opcode) {
        case spv::OpName:
            if (!operands.empty()) names[operands[0]] = DecodeString(operands.subspan(1));
            break;
        case spv::OpTypeFloat:
            if (operands.size() >= 2u && operands[1] == 64u) float64Types.insert(operands[0]);
            break;
        case spv::OpTypeVector:
            if (operands.size() >= 2u && float64Types.contains(operands[1])) float64Types.insert(operands[0]);
            break;
        case spv::OpTypeArray:
        case spv::OpTypeRuntimeArray:
            if (!operands.empty()) arrayTypes.insert(operands[0]);
            break;
        case spv::OpTypePointer:
            if (operands.size() >= 3u) pointerTypes[operands[0]] = {operands[1], operands[2]};
            break;
        case spv::OpFunction: {
            ++statistics.functions;
            const auto name = operands.size() >= 2u ? names.find(operands[1]) : names.end();
            currentFunction = name != names.end() ? name->second : "%" + std::to_string(operands.size() >= 2u ? operands[1] : 0u);
            ++statistics.instructionsByFunction[currentFunction];
            break;
        }
        case spv::OpFunctionEnd:
            currentFunction.clear();
            break;
        case spv::OpFunctionCall: {
            ++statistics.calls;
            const auto callee = operands.size() >= 3u ? names.find(operands[2]) : names.end();
            ++statistics.callsByCallee[callee != names.end() ? callee->second : "%" + std::to_string(operands.size() >= 3u ? operands[2] : 0u)];
            break;
        }
        case spv::OpVariable:
            if (operands.size() >= 3u && operands[2] == spv::StorageClassFunction) {
                const auto pointer = pointerTypes.find(operands[0]);
                if (pointer != pointerTypes.end() && arrayTypes.contains(pointer->second.second)) ++statistics.functionArrays;
            }
            break;
        case spv::OpConvertUToPtr:
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
        case spv::OpPtrAccessChain:
            if (operands.size() >= 2u) {
                const auto pointer = pointerTypes.find(operands[0]);
                if (pointer != pointerTypes.end() && pointer->second.first == spv::StorageClassPhysicalStorageBuffer) physicalPointers.insert(operands[1]);
            }
            break;
        case spv::OpLoad:
            if (operands.size() >= 3u && physicalPointers.contains(operands[2])) ++statistics.physicalLoads;
            break;
        case spv::OpStore:
            if (!operands.empty() && physicalPointers.contains(operands[0])) ++statistics.physicalStores;
            break;
        case spv::OpControlBarrier:
            ++statistics.controlBarriers;
            break;
        case spv::OpMemoryBarrier:
            ++statistics.memoryBarriers;
            break;
        case spv::OpLoopMerge:
            ++statistics.loops;
            break;
        case spv::OpSelectionMerge:
            ++statistics.selections;
            break;
        case spv::OpLine:
            ++statistics.lines;
            break;
        default:
            break;
        }
        if (opcode >= spv::OpAtomicLoad && opcode <= spv::OpAtomicXor) ++statistics.atomics;
        if (IsConversion(opcode)) ++statistics.conversions;
        if (IsFloatArithmetic(opcode) && float64Types.contains(resultType)) ++statistics.float64Arithmetic;
        if (IsShuffle(opcode)) ++statistics.shuffles;
        else if (IsBallot(opcode)) ++statistics.ballots;
        else if (opcode >= spv::OpGroupNonUniformElect && opcode <= spv::OpGroupNonUniformQuadSwap) ++statistics.otherGroupOperations;
        at += count;
    }
    return statistics;
}

std::string SpirvStatisticsJson(const SpirvStatistics& statistics) {
    std::ostringstream json;
    const auto quote = [&](const std::string& text) {
        json << '"';
        for (const auto character : text) {
            if (character == '"' || character == '\\') json << '\\';
            if (static_cast<unsigned char>(character) >= 0x20u) json << character;
        }
        json << '"';
    };
    const auto map = [&](const std::map<std::string, std::uint64_t>& values) {
        json << '{';
        bool first = true;
        for (const auto& [name, value] : values) {
            if (!first) json << ',';
            first = false;
            quote(name);
            json << ':' << value;
        }
        json << '}';
    };
    json << "{\"words\":" << statistics.words << ",\"instructions\":" << statistics.instructions << ",\"functions\":" << statistics.functions << ",\"calls\":" << statistics.calls
         << ",\"float64Arithmetic\":" << statistics.float64Arithmetic << ",\"conversions\":" << statistics.conversions << ",\"shuffles\":" << statistics.shuffles << ",\"ballots\":" << statistics.ballots
         << ",\"otherGroupOperations\":" << statistics.otherGroupOperations << ",\"controlBarriers\":" << statistics.controlBarriers << ",\"memoryBarriers\":" << statistics.memoryBarriers
         << ",\"functionArrays\":" << statistics.functionArrays << ",\"physicalLoads\":" << statistics.physicalLoads << ",\"physicalStores\":" << statistics.physicalStores << ",\"atomics\":" << statistics.atomics
         << ",\"loops\":" << statistics.loops << ",\"selections\":" << statistics.selections << ",\"lines\":" << statistics.lines << ",\"callsByCallee\":";
    map(statistics.callsByCallee);
    json << ",\"instructionsByFunction\":";
    map(statistics.instructionsByFunction);
    json << '}';
    return json.str();
}

}

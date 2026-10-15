#include "Optimization/SpinWaitAnalysis.hpp"
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

namespace {

bool ReadsDeviceMemory(IrOpcode opcode) {
    const auto buffer = BufferAccessOf(opcode);
    const auto address = AddressOpcodeInfoOf(opcode).access;
    const auto image = ImageOpcodeInfoOf(opcode).access;
    return buffer == BufferAccess::Read || buffer == BufferAccess::Atomic || address == AddressAccess::Read || address == AddressAccess::Atomic || image == ImageAccess::Read || image == ImageAccess::Atomic;
}

}

bool HasMemorySpinWait(const IrProgram& program) {
    const auto& blocks = program.BlockOrder();
    if (blocks.empty()) return false;
    const auto& blockInfo = program.Metadata().blockInfo;
    if (blockInfo.size() != blocks.size()) throw std::runtime_error("spin-wait analysis: block metadata is inconsistent");
    std::unordered_map<const IrBlock*, std::size_t> index;
    std::vector<std::pair<IrBlock*, std::size_t>> stack{{blocks.front(), 0u}};
    index.emplace(blocks.front(), 0u);
    std::vector<IrBlock*> postorder;
    while (!stack.empty()) {
        auto& [block, next] = stack.back();
        if (next < block->Successors().size()) {
            IrBlock* successor = block->Successors()[next++];
            if (index.emplace(successor, 0u).second) stack.emplace_back(successor, 0u);
            continue;
        }
        postorder.push_back(block);
        stack.pop_back();
    }
    const std::vector<IrBlock*> order(postorder.rbegin(), postorder.rend());
    for (std::size_t position = 0; position < order.size(); ++position) index[order[position]] = position;
    constexpr std::size_t none = static_cast<std::size_t>(-1);
    std::vector<std::size_t> dominator(order.size(), none);
    dominator[0] = 0;
    const auto intersect = [&](std::size_t left, std::size_t right) {
        while (left != right) {
            while (left > right) left = dominator[left];
            while (right > left) right = dominator[right];
        }
        return left;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t position = 1; position < order.size(); ++position) {
            std::size_t candidate = none;
            for (const IrBlock* predecessor : order[position]->Predecessors()) {
                const auto found = index.find(predecessor);
                if (found == index.end() || dominator[found->second] == none) continue;
                candidate = candidate == none ? found->second : intersect(candidate, found->second);
            }
            if (candidate != dominator[position]) {
                dominator[position] = candidate;
                changed = true;
            }
        }
    }
    const auto dominates = [&](std::size_t header, std::size_t block) {
        while (block != header && block != 0u) block = dominator[block];
        return block == header;
    };
    std::unordered_map<const IrBlock*, const BlockInfo*> infoOf;
    for (std::size_t position = 0; position < blocks.size(); ++position) infoOf.emplace(blocks[position], &blockInfo[position]);
    for (std::size_t latch = 0; latch < order.size(); ++latch) {
        for (const IrBlock* successor : order[latch]->Successors()) {
            const auto header = index.find(successor);
            if (header == index.end() || !dominates(header->second, latch)) continue;
            std::unordered_set<const IrBlock*> body{successor};
            std::vector<const IrBlock*> pending;
            if (body.insert(order[latch]).second) pending.push_back(order[latch]);
            while (!pending.empty()) {
                const IrBlock* block = pending.back();
                pending.pop_back();
                for (const IrBlock* predecessor : block->Predecessors()) {
                    if (index.contains(predecessor) && body.insert(predecessor).second) pending.push_back(predecessor);
                }
            }
            std::vector<const IrValue*> roots;
            for (const IrBlock* block : body) {
                const bool exits = std::any_of(block->Successors().begin(), block->Successors().end(), [&](const IrBlock* next) { return !body.contains(next); });
                const BlockInfo* info = infoOf.at(block);
                if (exits && info->condition != nullptr) roots.push_back(info->condition);
                if (info->indirectTarget != nullptr) roots.push_back(info->indirectTarget);
            }
            std::unordered_set<const IrValue*> seen;
            while (!roots.empty()) {
                const IrValue* value = roots.back()->Resolve();
                roots.pop_back();
                if (!seen.insert(value).second) continue;
                if (value->Parent() != nullptr && body.contains(value->Parent()) && ReadsDeviceMemory(value->Opcode())) return true;
                for (std::size_t argument = 0; argument < value->ArgumentCount(); ++argument) {
                    if (const IrValue* operand = value->Argument(argument); operand != nullptr) roots.push_back(operand);
                }
            }
        }
    }
    return false;
}

}

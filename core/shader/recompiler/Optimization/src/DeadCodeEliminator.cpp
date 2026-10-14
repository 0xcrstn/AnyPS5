#include "Optimization/DeadCodeEliminator.hpp"
#include <algorithm>
#include <vector>

namespace ShaderRecompiler {

void DeadCodeEliminator::RemoveIdentities(std::span<IrBlock* const> blocks) const {
    for (IrBlock* block : blocks) {
        auto& instructions = block->Instructions();
        for (auto it = instructions.begin(); it != instructions.end();) {
            IrValue* inst = *it;
            if (inst->Opcode() != IrOpcode::Identity) {
                ++it;
                continue;
            }
            IrValue* replacement = inst->Argument(0);
            inst->ReplaceUsesWith(replacement, false);
            it = instructions.erase(it);
            inst->SetParent(nullptr);
        }
    }
}

void DeadCodeEliminator::RemoveIdentities(IrProgram& program) const {
    RemoveIdentities(program.BlockOrder());
}

void DeadCodeEliminator::Eliminate(std::span<IrBlock* const> blocks) const {
    std::vector<const IrBlock*> scope(blocks.begin(), blocks.end());
    std::ranges::sort(scope);
    std::vector<IrBlock*> changed;
    std::vector<IrValue*> pending;
    const auto removable = [&](const IrValue* value) {
        return value->Parent() != nullptr && !value->HasUses() && !value->MayHaveSideEffects() && std::ranges::binary_search(scope, static_cast<const IrBlock*>(value->Parent()));
    };
    const auto remove = [&](IrValue* inst) {
        IrBlock* block = inst->Parent();
        if (changed.empty() || changed.back() != block) changed.push_back(block);
        const auto first = pending.size();
        for (IrValue* argument : inst->Arguments()) {
            if (argument != nullptr) pending.push_back(argument);
        }
        inst->Invalidate();
        inst->SetParent(nullptr);
        for (auto index = first; index < pending.size();) {
            if (removable(pending[index])) {
                ++index;
            } else {
                pending[index] = pending.back();
                pending.pop_back();
            }
        }
    };
    for (auto blockIt = blocks.rbegin(); blockIt != blocks.rend(); ++blockIt) {
        auto& instructions = (*blockIt)->Instructions();
        for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
            if (removable(*it)) remove(*it);
            while (!pending.empty()) {
                IrValue* next = pending.back();
                pending.pop_back();
                if (removable(next)) remove(next);
            }
        }
    }
    std::ranges::sort(changed);
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (IrBlock* block : changed) block->Instructions().remove_if([](const IrValue* inst) { return inst->Parent() == nullptr; });
}

void DeadCodeEliminator::Eliminate(IrProgram& program) const {
    Eliminate(program.BlockOrder());
}

}

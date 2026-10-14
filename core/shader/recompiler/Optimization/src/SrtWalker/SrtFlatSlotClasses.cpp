#include "Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ShaderRecompiler::Detail {

std::vector<std::uint8_t> ComputePureFlatSlots(const IrResourcePlan& plan) {
    std::vector<std::uint8_t> pure(plan.srtReads.size(), 1u);
    if (pure.empty()) return pure;
    const bool disqualified = !plan.srtPlanComplete || plan.requiresSpecializationMemory
        || std::any_of(plan.descriptorSources.begin(), plan.descriptorSources.end(), [](const DescriptorSource& source) { return source.indirectImage.has_value(); })
        || (plan.uniformFill.fill.kind != UniformFillKind::None && plan.uniformFill.fill.words > plan.uniformFill.values.size());
    if (disqualified) {
        std::fill(pure.begin(), pure.end(), 0u);
        return pure;
    }
    // The raw read node of each slot: a descriptor source dword may point at it directly (the
    // tracker resolved the handle argument before the plan builder patched the operands).
    std::unordered_map<const IrValue*, std::uint32_t> readSlots;
    for (std::uint32_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        if (plan.srtReads[slot].value != nullptr) readSlots.emplace(plan.srtReads[slot].value->Resolve(), slot);
    }
    std::unordered_set<const IrValue*> visited;
    std::vector<IrValue*> pending;
    const auto mark = [&](IrValue* root) {
        if (root == nullptr) return;
        pending.push_back(root);
        while (!pending.empty()) {
            IrValue* value = pending.back()->Resolve();
            pending.pop_back();
            if (!visited.insert(value).second) continue;
            if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
                const IrValue* slot = value->Argument(1)->Resolve();
                if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < pure.size()) {
                    const auto index = slot->ImmediateU32();
                    pure[index] = 0u;
                    pending.push_back(plan.srtReads[index].value);
                }
            }
            if (const auto found = readSlots.find(value); found != readSlots.end()) pure[found->second] = 0u;
            for (std::size_t i = 0; i < value->ArgumentCount(); ++i) {
                if (value->Argument(i) != nullptr) pending.push_back(value->Argument(i));
            }
        }
    };
    for (const auto& source : plan.descriptorSources) {
        for (std::uint32_t i = 0; i < source.dwordCount && i < source.dwords.size(); ++i) mark(source.dwords[i]);
    }
    for (const auto& block : plan.controlFlow) mark(block.condition);
    if (plan.uniformFill.fill.kind != UniformFillKind::None) {
        for (std::uint32_t i = 0; i < plan.uniformFill.fill.words; ++i) mark(plan.uniformFill.values[i]);
    }
    // The address cones: the arguments of every slot's raw read, never the read itself.
    for (const auto& read : plan.srtReads) {
        if (read.value == nullptr) continue;
        IrValue* raw = read.value->Resolve();
        for (std::size_t i = 0; i < raw->ArgumentCount(); ++i) mark(raw->Argument(i));
    }
    return pure;
}

std::vector<std::pair<const IrValue*, RawReadUse>> ClassifyRawReads(const IrResourcePlan& plan) {
    std::vector<std::pair<const IrValue*, RawReadUse>> result;
    const bool disqualified = !plan.srtPlanComplete || plan.requiresSpecializationMemory
        || std::any_of(plan.descriptorSources.begin(), plan.descriptorSources.end(), [](const DescriptorSource& source) { return source.indirectImage.has_value(); })
        || (plan.uniformFill.fill.kind != UniformFillKind::None && plan.uniformFill.fill.words > plan.uniformFill.values.size());
    if (disqualified) return result;
    std::unordered_map<const IrValue*, RawReadUse> uses;
    std::vector<IrValue*> raws;
    {
        std::unordered_set<const IrValue*> seen;
        std::vector<IrValue*> walk;
        for (const auto& read : plan.srtReads) {
            if (read.value != nullptr) walk.push_back(read.value);
        }
        for (const auto& source : plan.descriptorSources) {
            for (std::uint32_t i = 0; i < source.dwordCount && i < source.dwords.size(); ++i) {
                if (source.dwords[i] != nullptr) walk.push_back(source.dwords[i]);
            }
        }
        for (const auto& block : plan.controlFlow) {
            if (block.condition != nullptr) walk.push_back(block.condition);
        }
        if (plan.uniformFill.fill.kind != UniformFillKind::None) {
            for (std::uint32_t i = 0; i < plan.uniformFill.fill.words && i < plan.uniformFill.values.size(); ++i) {
                if (plan.uniformFill.values[i] != nullptr) walk.push_back(plan.uniformFill.values[i]);
            }
        }
        while (!walk.empty()) {
            IrValue* value = walk.back()->Resolve();
            walk.pop_back();
            if (!seen.insert(value).second) continue;
            if (IsRawRead(plan, *value)) {
                raws.push_back(value);
                uses.emplace(value, RawReadUse::Unseen);
            }
            for (std::size_t i = 0; i < value->ArgumentCount(); ++i) {
                if (value->Argument(i) != nullptr) walk.push_back(value->Argument(i));
            }
            if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
                const IrValue* slot = value->Argument(1)->Resolve();
                if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < plan.srtReads.size() && plan.srtReads[slot->ImmediateU32()].value != nullptr) walk.push_back(plan.srtReads[slot->ImmediateU32()].value);
            }
        }
    }
    const auto raise = [&](const IrValue* raw, RawReadUse use) {
        auto& held = uses[raw];
        if (static_cast<std::uint8_t>(use) > static_cast<std::uint8_t>(held)) held = use;
    };
    for (std::uint32_t slot = 0; slot < plan.srtReads.size() && slot < plan.pureFlatSlots.size(); ++slot) {
        if (plan.pureFlatSlots[slot] != 0u && plan.srtReads[slot].value != nullptr) raise(plan.srtReads[slot].value->Resolve(), RawReadUse::Leaf);
    }
    const auto mark = [&](IrValue* root, RawReadUse use, const IrValue* exemptBase) {
        if (root == nullptr) return;
        std::unordered_set<const IrValue*> visited;
        std::vector<std::pair<IrValue*, RawReadUse>> pending{{root, use}};
        while (!pending.empty()) {
            auto [value, reason] = pending.back();
            value = value->Resolve();
            pending.pop_back();
            if (!visited.insert(value).second || value->HasImmediate()) continue;
            if (IsRawRead(plan, *value)) {
                raise(value, reason);
                continue;
            }
            if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
                const IrValue* slot = value->Argument(1)->Resolve();
                if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < plan.srtReads.size() && plan.srtReads[slot->ImmediateU32()].value != nullptr) {
                    raise(plan.srtReads[slot->ImmediateU32()].value->Resolve(), reason);
                    continue;
                }
            }
            if (IsCarryOfBase(plan, *value, exemptBase)) continue;
            if (value->Opcode() == IrOpcode::CompositeExtractU32x2 && value->ArgumentCount() >= 2 && value->Argument(0) != nullptr && value->Argument(1) != nullptr) {
                const IrValue* index = value->Argument(1)->Resolve();
                IrValue* source = value->Argument(0)->Resolve();
                if (index->HasImmediate() && index->Type() == IrType::U32 && index->ImmediateU32() == 1u && source->Opcode() == IrOpcode::IAddCarry32) {
                    for (std::size_t i = 0; i < source->ArgumentCount(); ++i) {
                        if (source->Argument(i) != nullptr) pending.emplace_back(source->Argument(i), RawReadUse::Carry);
                    }
                    continue;
                }
            }
            for (std::size_t i = 0; i < value->ArgumentCount(); ++i) {
                if (value->Argument(i) != nullptr) pending.emplace_back(value->Argument(i), reason);
            }
        }
    };
    for (const auto& source : plan.descriptorSources) {
        for (std::uint32_t i = 0; i < source.dwordCount && i < source.dwords.size(); ++i) mark(source.dwords[i], RawReadUse::Value, nullptr);
    }
    for (const auto& block : plan.controlFlow) mark(block.condition, RawReadUse::Value, nullptr);
    if (plan.uniformFill.fill.kind != UniformFillKind::None) {
        for (std::uint32_t i = 0; i < plan.uniformFill.fill.words; ++i) mark(plan.uniformFill.values[i], RawReadUse::Value, nullptr);
    }
    for (IrValue* raw : raws) {
        IrValue* handle = raw->ArgumentCount() != 0u ? raw->Argument(0) : nullptr;
        if (handle == nullptr) continue;
        handle = handle->Resolve();
        if (handle->ArgumentCount() == 0u) {
            mark(handle, RawReadUse::Value, nullptr);
            continue;
        }
        const IrValue* base = LowBaseRead(plan, handle->Argument(0));
        if (base != nullptr) raise(base, RawReadUse::BaseOnly);
        else mark(handle->Argument(0), RawReadUse::UnderArithmetic, nullptr);
        for (std::size_t i = 1; i < handle->ArgumentCount(); ++i) mark(handle->Argument(i), RawReadUse::HighWordOrOffset, base);
        for (std::size_t i = 1; i < raw->ArgumentCount(); ++i) mark(raw->Argument(i), RawReadUse::HighWordOrOffset, nullptr);
    }
    result.reserve(raws.size());
    for (const IrValue* raw : raws) result.emplace_back(raw, uses[raw]);
    return result;
}

std::vector<std::uint32_t> ComputeGuardedFlatSlots(const IrResourcePlan& plan) {
    std::unordered_map<const IrValue*, std::vector<const IrValue*>> users;
    std::unordered_set<const IrValue*> seen;
    std::vector<const IrValue*> pending;
    std::vector<const IrValue*> poisonable;
    for (const auto& read : plan.srtReads) {
        if (read.value != nullptr) pending.push_back(read.value->Resolve());
    }
    while (!pending.empty()) {
        const IrValue* value = pending.back();
        pending.pop_back();
        if (!seen.insert(value).second) continue;
        const IrValue* handle = value->ArgumentCount() != 0u && value->Argument(0) != nullptr ? value->Argument(0)->Resolve() : nullptr;
        if (IsRawRead(plan, *value) && handle != nullptr && LoadedFromMemory(*handle)) poisonable.push_back(value);
        const auto follow = [&](const IrValue* argument) {
            if (argument == nullptr) return;
            argument = argument->Resolve();
            users[argument].push_back(value);
            pending.push_back(argument);
        };
        for (std::size_t i = 0; i < value->ArgumentCount(); ++i) follow(value->Argument(i));
        if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
            const IrValue* slot = value->Argument(1)->Resolve();
            if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < plan.srtReads.size()) follow(plan.srtReads[slot->ImmediateU32()].value);
        }
    }
    std::unordered_set<const IrValue*> reaching(poisonable.begin(), poisonable.end());
    while (!poisonable.empty()) {
        const IrValue* value = poisonable.back();
        poisonable.pop_back();
        const auto found = users.find(value);
        if (found == users.end()) continue;
        for (const IrValue* user : found->second) {
            if (reaching.insert(user).second) poisonable.push_back(user);
        }
    }
    std::vector<std::uint32_t> guarded;
    for (std::uint32_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        if (plan.srtReads[slot].value != nullptr && reaching.contains(plan.srtReads[slot].value->Resolve())) guarded.push_back(slot);
    }
    return guarded;
}

}

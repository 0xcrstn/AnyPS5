#include "Materialize.hpp"
#include "Recompiler.hpp"
#include "ShaderManifest.hpp"
#include "StencilReplay.hpp"
#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include "CacheKey.hpp"
#include "CompileTrace.hpp"
#include "SpirvIntern.hpp"
#include "CompiledVariant.hpp"
#include "VertexInputSpecialization.hpp"
#include "FragmentOutputSpecialization.hpp"
#include "SpirvBackend/SpirvSpecialization.hpp"
#include "RdnaDecoder/RdnaDescriptorFormat.hpp"
#include "ShaderDiskCache.hpp"
#include <list>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include "ControlFlow/include/ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/include/ControlFlow/Structurizer.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "IntermediateRepresentation/include/IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/include/Optimization/BindingAllocator.hpp"
#include "Optimization/include/Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "Optimization/include/Optimization/ConstantFolder.hpp"
#include "Optimization/include/Optimization/DeadCodeEliminator.hpp"
#include "Optimization/include/Optimization/DenormalFlushEliminator.hpp"
#include "Optimization/include/Optimization/DescriptorBindingBuilder.hpp"
#include "Optimization/include/Optimization/HostInterpolationChecker.hpp"
#include "Optimization/include/Optimization/MaskedSelectEliminator.hpp"
#include "Optimization/include/Optimization/ReadLaneEliminator.hpp"
#include "Optimization/include/Optimization/RedundantValueEliminator.hpp"
#include "Optimization/include/Optimization/RequestMemoryView.hpp"
#include "Optimization/include/Optimization/ResourceMaterializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Optimization/include/Optimization/ResourceTracker.hpp"
#include "Optimization/include/Optimization/ShaderInfoCollector.hpp"
#include "Optimization/include/Optimization/SrtWalker.hpp"
#include "Optimization/include/Optimization/SsaBuilder.hpp"
#include "Optimization/include/Optimization/TessellationLowering.hpp"
#include "SpirvBackend/include/SpirvBackend/SpirvEmitter.hpp"
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "SpirvBackend/SpirvOptimizer.hpp"
#endif
#include "SpirvBackend/SpirvModuleShape.hpp"
#include "SpirvBackend/SpirvMemory/SpirvInputOutput.hpp"
#include "Translation/include/Translation/InstructionTranslator.hpp"
#include "Translation/include/Translation/ShaderInputInfoBuilder.hpp"
#include <exception>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <ControlFlow/RequestSerializer.hpp>

namespace ShaderRecompiler::Materialization {

struct SpecializedModule {
    std::uint64_t specializationId = 0;
    SharedSpirv spirv;
    std::vector<std::uint32_t> bindings;
    bool pushData = false;
};

std::shared_ptr<const SpecializedModule> buildSpecializedModule(const CompiledShaderArtifact& artifact, std::span<const std::uint32_t> classes, std::span<const PipelineSpecializationConstant> constants, const SpirvTarget& target) {
    ShaderCompileTrace::Event specializeEvent("specialize");
    if (specializeEvent.Active()) {
        std::vector<std::uint32_t> vector;
        for (const auto& constant : constants) vector.insert(vector.end(), {constant.id, constant.value});
        const auto input = artifact.spirv.Resolved();
        specializeEvent.Hex("variant", artifact.variantId).Number("spirv_words_in", input.size()).Hex("spirv_in_hash", ShaderCompileTrace::HashOf(input.Words())).Number("constants", constants.size()).Hex("specialization_hash", ShaderCompileTrace::HashOf(std::span<const std::uint32_t>(vector))).Hex("vertex_classes_hash", ShaderCompileTrace::HashOf(classes));
    }
    auto source = SpecializeVertexInputTypes(artifact, classes);
    std::map<std::uint32_t, std::uint32_t> supplied;
    for (const auto& constant : constants) {
        if (!supplied.emplace(constant.id, constant.value).second) throw std::runtime_error("duplicate prepared specialization ID");
    }
    std::map<std::uint32_t, std::uint32_t> values;
    std::set<std::uint32_t> specializedIds;
    std::set<std::uint32_t> pushTypes;
    std::optional<std::uint32_t> replaySlotBytes;
    const auto& words = source.Words();
    if (words.size() < 5u || words[0] != spv::MagicNumber) throw std::runtime_error("invalid prepared specialization module");
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        const auto op = static_cast<spv::Op>(words[cursor] & 0xffffu);
        if (count == 0u || count > words.size() - cursor) throw std::runtime_error("truncated prepared specialization instruction");
        if (!artifact.stencilReferences.empty() && op == spv::OpTypePointer && count == 4u && words[cursor + 2u] == spv::StorageClassPushConstant) pushTypes.insert(words[cursor + 3u]);
        if (op == spv::OpDecorate && count == 4u && words[cursor + 2u] == spv::DecorationSpecId) {
            const auto found = supplied.find(words[cursor + 3u]);
            if (found == supplied.end()) throw std::runtime_error("missing prepared specialization value");
            auto value = found->second;
            if (!artifact.stencilReferences.empty()) {
                if (found->first == PipelineSpecialization::StencilReferenceOffset) replaySlotBytes = value / PushData::DwordCount * NativePushSlotSize;
                if (found->first == PipelineSpecialization::StencilReferenceOffset || found->first == PipelineSpecialization::PushDataOffset) value %= PushData::DwordCount;
            }
            if (!values.emplace(words[cursor + 1u], value).second) throw std::runtime_error("duplicate prepared specialization value");
            specializedIds.insert(found->first);
        }
        cursor += count;
    }
    std::vector<std::uint32_t> materialized(words.begin(), words.begin() + 5);
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        const auto op = static_cast<spv::Op>(words[cursor] & 0xffffu);
        if (op == spv::OpSpecConstant) {
            if (count != 4u || !values.contains(words[cursor + 2u])) throw std::runtime_error("invalid prepared specialization constant");
            materialized.insert(materialized.end(), {(4u << 16u) | spv::OpConstant, words[cursor + 1u], words[cursor + 2u], values.at(words[cursor + 2u])});
        } else if (op == spv::OpMemberDecorate && count == 5u && words[cursor + 3u] == spv::DecorationOffset && pushTypes.contains(words[cursor + 1u])) {
            if (!replaySlotBytes || words[cursor + 2u] != 0u || *replaySlotBytes >= NativePushConstantSize) throw std::runtime_error("invalid stencil replay push constant slot");
            materialized.insert(materialized.end(), words.begin() + cursor, words.begin() + cursor + count - 1u);
            materialized.push_back(*replaySlotBytes);
        } else if (!(op == spv::OpDecorate && count == 4u && words[cursor + 2u] == spv::DecorationSpecId)) {
            materialized.insert(materialized.end(), words.begin() + cursor, words.begin() + cursor + count);
        }
        cursor += count;
    }
    materialized = SpecializeFragmentOutputs(SpecializeSpirv(materialized), constants, specializedIds);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    materialized = ValidateAndOptimizeSpirv(materialized, target.vulkanVersion, target.spirvVersion, target.nonConstantImageOffsets, true, true);
#endif
    std::map<std::uint32_t, std::uint32_t> descriptorVariables;
    std::set<std::uint32_t> usedDescriptors;
    for (std::size_t cursor = 5; cursor < materialized.size();) {
        const auto count = materialized[cursor] >> 16u;
        const auto op = static_cast<spv::Op>(materialized[cursor] & 0xffffu);
        if (op == spv::OpDecorate && count == 4u && materialized[cursor + 2u] == spv::DecorationBinding) descriptorVariables.emplace(materialized[cursor + 1u], materialized[cursor + 3u]);
        if ((op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain || op == spv::OpPtrAccessChain || op == spv::OpInBoundsPtrAccessChain || op == spv::OpLoad || op == spv::OpCopyObject) && count >= 4u) usedDescriptors.insert(materialized[cursor + 3u]);
        cursor += count;
    }
    const auto unused = [&](std::uint32_t id) { return descriptorVariables.contains(id) && !usedDescriptors.contains(id); };
    std::vector<std::uint32_t> compact(materialized.begin(), materialized.begin() + 5);
    for (std::size_t cursor = 5; cursor < materialized.size();) {
        const auto count = materialized[cursor] >> 16u;
        const auto op = static_cast<spv::Op>(materialized[cursor] & 0xffffu);
        if (op == spv::OpEntryPoint) {
            const auto start = compact.size();
            std::size_t interfaceIndex = 3u;
            for (; interfaceIndex < count; ++interfaceIndex) {
                const auto word = materialized[cursor + interfaceIndex];
                if ((word & 0xffu) == 0u || (word & 0xff00u) == 0u || (word & 0xff0000u) == 0u || (word & 0xff000000u) == 0u) { ++interfaceIndex; break; }
            }
            compact.insert(compact.end(), materialized.begin() + cursor, materialized.begin() + cursor + interfaceIndex);
            for (auto index = interfaceIndex; index < count; ++index) if (!unused(materialized[cursor + index])) compact.push_back(materialized[cursor + index]);
            compact[start] = (static_cast<std::uint32_t>(compact.size() - start) << 16u) | spv::OpEntryPoint;
        } else if (!((op == spv::OpVariable && unused(materialized[cursor + 2u])) || ((op == spv::OpDecorate || op == spv::OpName) && unused(materialized[cursor + 1u])))) {
            compact.insert(compact.end(), materialized.begin() + cursor, materialized.begin() + cursor + count);
        }
        cursor += count;
    }
    materialized = std::move(compact);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    materialized = ValidateAndOptimizeSpirv(materialized, target.vulkanVersion, target.spirvVersion, target.nonConstantImageOffsets, false);
#endif
    auto module = std::make_shared<SpecializedModule>();
    std::map<std::uint32_t, std::uint32_t> bindingNumbers;
    for (std::size_t cursor = 5; cursor < materialized.size();) {
        const auto count = materialized[cursor] >> 16u;
        const auto op = static_cast<spv::Op>(materialized[cursor] & 0xffffu);
        if (op == spv::OpDecorate && count == 4u && materialized[cursor + 2u] == spv::DecorationBinding) bindingNumbers.emplace(materialized[cursor + 1u], materialized[cursor + 3u]);
        if (op == spv::OpVariable && count >= 4u) {
            if (const auto found = bindingNumbers.find(materialized[cursor + 2u]); found != bindingNumbers.end()) module->bindings.push_back(found->second);
            module->pushData |= materialized[cursor + 3u] == spv::StorageClassPushConstant;
        }
        cursor += count;
    }
    module->spirv = InternSpirv(std::move(materialized));
    module->specializationId = constants.empty() ? 0u : nextVariantId();
    if (specializeEvent.Active()) {
        const auto interned = SpirvInternTotals();
        specializeEvent.Number("spirv_words", module->spirv.size()).Hex("spirv_hash", ShaderCompileTrace::HashOf(module->spirv.Words())).Number("intern_lookups", interned.lookups).Number("intern_shared", interned.shared).Number("intern_shared_bytes", interned.sharedBytes);
    }
    retainedCounters().specializedModules.fetch_add(1, std::memory_order_relaxed);
    retainedCounters().specializedSpirvBytes.fetch_add(module->spirv.Words().size() * sizeof(std::uint32_t), std::memory_order_relaxed);
    return module;
}

struct SpecializedModuleEntry {
    std::once_flag ready;
    std::exception_ptr failure;
    std::shared_ptr<const SpecializedModule> module;
};

struct PreparedModuleEntry {
    std::once_flag ready;
    std::exception_ptr failure;
    std::shared_ptr<const SpecializedModule> module;
    DescriptorBindingPlan bindings;
};

struct PreparedBindingPlan {
    std::once_flag ready;
    std::exception_ptr failure;
    DescriptorBindingPlan bindings;
    PrimedSharedMutex mutex;
    std::map<std::vector<std::uint32_t>, std::shared_ptr<PreparedModuleEntry>> modules;
};

struct RegistrationModuleEntry {
    std::once_flag ready;
    std::exception_ptr failure;
    std::shared_ptr<const SpecializedModule> module;
};

std::shared_ptr<const SpecializedModule> registrationModule(std::uint64_t variantId, std::span<const std::uint32_t> moduleKey, const std::function<std::shared_ptr<const SpecializedModule>()>& specialize) {
    std::vector<std::uint64_t> key{variantId};
    key.insert(key.end(), moduleKey.begin(), moduleKey.end());
    static PrimedSharedMutex mutex;
    static std::map<std::vector<std::uint64_t>, std::shared_ptr<RegistrationModuleEntry>> modules;
    std::shared_ptr<RegistrationModuleEntry> entry;
    {
        std::shared_lock lock(mutex);
        if (const auto found = modules.find(key); found != modules.end()) entry = found->second;
    }
    if (entry == nullptr) {
        std::unique_lock lock(mutex);
        const auto found = modules.find(key);
        if (found != modules.end()) {
            entry = found->second;
        } else {
            entry = modules.emplace(key, std::make_shared<RegistrationModuleEntry>()).first->second;
            retainedCounters().registrationModules.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::call_once(entry->ready, [&] {
        try {
            entry->module = specialize();
        } catch (...) {
            entry->failure = std::current_exception();
        }
    });
    if (entry->failure) std::rethrow_exception(entry->failure);
    return entry->module;
}

void bindingPlanKeyWords(const CompiledVariant& variant, const ResourceSnapshot& snapshot, std::span<const std::uint8_t> exports, std::vector<std::uint64_t>& key) {
    key.clear();
    key.push_back(variant.artifact.variantId);
    for (std::size_t index = 0; index < variant.info.info.buffers.size(); ++index) {
        const auto& descriptor = snapshot.buffers.at(index);
        key.push_back(descriptor.dwordCount);
        key.push_back(descriptor.dwords[1] & 0xffff0000u);
        key.push_back(descriptor.dwords[3]);
        const bool present = descriptor.dwords[2] != 0u && (descriptor.dwords[0] != 0u || (descriptor.dwords[1] & 0xffffu) != 0u);
        key.push_back(present ? 4u | (descriptor.dwords[0] & 3u) : 0u);
    }
    for (std::size_t index = 0; index < variant.info.info.images.size(); ++index) {
        const auto& descriptor = snapshot.images.at(index);
        key.push_back(descriptor.dwordCount);
        key.push_back(descriptor.dwords[0] != 0u || (descriptor.dwords[1] & 0xffu) != 0u);
        key.push_back(descriptor.dwords[1] & ~0xffu);
        for (std::size_t word = 2; word < 8; ++word) key.push_back(descriptor.dwords[word]);
    }
    for (std::size_t index = 0; index < variant.info.info.samplers.size(); ++index) {
        const auto& descriptor = snapshot.samplers.at(index);
        key.push_back(descriptor.dwordCount);
        for (std::size_t word = 0; word < 4; ++word) key.push_back(descriptor.dwords[word]);
    }
    key.push_back(exports.size());
    for (const auto mapping : exports) key.push_back(mapping);
}

std::shared_ptr<PreparedBindingPlan> preparedBindingPlan(const CompiledVariant& variant, const ResourceSnapshot& snapshot, std::span<const std::uint8_t> exports, bool fragmentDescriptorSet) {
    struct BindingPlanKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, BindingPlanKeyStorage>();
    bindingPlanKeyWords(variant, snapshot, exports, key);
    static PrimedSharedMutex mutex;
    static std::map<std::vector<std::uint64_t>, std::shared_ptr<PreparedBindingPlan>> plans;
    std::shared_ptr<PreparedBindingPlan> plan;
    {
        std::shared_lock lock(mutex);
        if (const auto found = plans.find(key); found != plans.end()) plan = found->second;
    }
    if (plan == nullptr) {
        std::unique_lock lock(mutex);
        const auto found = plans.find(key);
        if (found != plans.end()) {
            plan = found->second;
        } else {
            plan = plans.emplace(key, std::make_shared<PreparedBindingPlan>()).first->second;
            retainedCounters().bindingPlans.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::call_once(plan->ready, [&] {
        try {
            plan->bindings = DescriptorBindingBuilder{}.Prepare(variant.bindings.layout, variant.info.info, variant.info.stage, snapshot, exports, variant.artifact.registrationModule, fragmentDescriptorSet);
        } catch (...) {
            plan->failure = std::current_exception();
        }
    });
    if (plan->failure) std::rethrow_exception(plan->failure);
    return plan;
}

std::shared_ptr<const SpecializedModule> specializeModule(const CompiledShaderArtifact& artifact, std::span<const std::uint32_t> classes, std::span<const PipelineSpecializationConstant> constants, const SpirvTarget& target) {
    struct ModuleKeyStorage {};
    auto& key = HostThreadLocal<std::vector<std::uint64_t>, ModuleKeyStorage>();
    key.clear();
    key.push_back(artifact.variantId);
    for (const auto& constant : constants) key.push_back((static_cast<std::uint64_t>(constant.id) << 32u) | constant.value);
    static PrimedSharedMutex mutex;
    static std::map<std::vector<std::uint64_t>, std::shared_ptr<SpecializedModuleEntry>> modules;
    std::shared_ptr<SpecializedModuleEntry> entry;
    {
        std::shared_lock lock(mutex);
        if (const auto found = modules.find(key); found != modules.end()) entry = found->second;
    }
    if (entry == nullptr) {
        std::unique_lock lock(mutex);
        const auto found = modules.find(key);
        entry = found != modules.end() ? found->second : modules.emplace(key, std::make_shared<SpecializedModuleEntry>()).first->second;
    }
    std::call_once(entry->ready, [&] {
        try {
            entry->module = buildSpecializedModule(artifact, classes, constants, target);
        } catch (...) {
            entry->failure = std::current_exception();
        }
    });
    if (entry->failure) std::rethrow_exception(entry->failure);
    return entry->module;
}

RecompileResult materializeSkeleton(const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot, bool registrationDefaults, MaterializedParts& parts) {
    RecompileResult result;
    const auto& artifact = variant.artifact;
    static_cast<CompiledShaderArtifact&>(result) = {
        .memoryOffsetDword = artifact.memoryOffsetDword,
        .shaderDataDwords = artifact.shaderDataDwords,
        .imageMetadataDword = artifact.imageMetadataDword,
        .runtimeImageCount = artifact.runtimeImageCount,
        .runtimeImageResources = artifact.runtimeImageResources,
        .registrationModule = artifact.registrationModule,
        .bdaAbiVersion = artifact.bdaAbiVersion,
        .runtimeAbiVersion = artifact.runtimeAbiVersion,
        .vertexInputs = artifact.vertexInputs,
        .vertexOffsetSgpr = artifact.vertexOffsetSgpr,
        .instanceOffsetSgpr = artifact.instanceOffsetSgpr,
        .vertexOffsetShared = artifact.vertexOffsetShared,
        .instanceOffsetShared = artifact.instanceOffsetShared,
        .vertexOffsetConflict = artifact.vertexOffsetConflict,
        .instanceOffsetConflict = artifact.instanceOffsetConflict,
        .hostSubgroupSize = artifact.hostSubgroupSize,
        .parameterExports = artifact.parameterExports,
        .fragmentParameters = artifact.fragmentParameters,
        .barycentricEmulation = artifact.barycentricEmulation,
        .stencilReferences = artifact.stencilReferences,
        .stencilReplaySafe = artifact.stencilReplaySafe,
        .stencilReferenceDword = artifact.stencilReferenceDword,
        .variantId = artifact.variantId
    };
    const auto plan = preparedBindingPlan(variant, snapshot, request.context.pixel ? std::span<const std::uint8_t>(request.context.pixel->targetExportMapping) : std::span<const std::uint8_t>{}, request.target.registrationModules);
    parts.plan = plan;
    const DescriptorBindingPlan* bindingPlan = &plan->bindings;
    std::shared_ptr<PreparedModuleEntry> entry;
    struct LocalModuleKeyStorage {};
    auto& moduleKey = HostThreadLocal<std::vector<std::uint32_t>, LocalModuleKeyStorage>();
    moduleKey.clear();
    if (variant.bindings.layout.UsesPushData()) moduleKey.push_back(request.layout.pushConstantOffsetBytes / 4u);
    if (!artifact.stencilReferences.empty()) moduleKey.push_back(request.layout.pushConstantOffsetBytes / 4u + artifact.stencilReferenceDword);
    if (request.context.pixel) {
        for (const auto packing : request.context.pixel->targetExportPacking) moduleKey.push_back(static_cast<std::uint32_t>(packing));
        moduleKey.push_back(request.context.pixel->dualSourceBlend ? 1u : 0u);
        if (variant.info.info.rasterPrimitive) moduleKey.push_back(static_cast<std::uint32_t>(request.context.pixel->rasterPrimitive));
        if (variant.info.info.layerExported) moduleKey.push_back(request.context.pixel->layerExported ? 1u : 0u);
    }
    result.vertexAttributes.reserve(result.vertexInputs.size());
    std::array<std::uint32_t, ShaderVertexStageInfo::MaxResources> vertexClasses{};
    auto& runtimeSelectors = parts.runtimeSelectors;
    for (const auto& input : result.vertexInputs) {
        if (!request.context.vertex || input.location >= request.context.vertex->resourcesNum || input.location >= request.context.vertex->resources.size()) throw std::runtime_error("Shader cache: invalid vertex attribute metadata");
        const auto& vertex = *request.context.vertex;
        const auto& resource = vertex.resources[input.location];
        const auto fetchIndex = vertex.fetchEmbedded ? vertex.resourcesDst[input.location].fetchIndex : input.fetchIndex;
        result.vertexAttributes.push_back({input.location, input.components, resource, fetchIndex});
        if (!artifact.vertexInputPatches.empty()) {
            const auto format = static_cast<IrBufferFormat>((resource.fields[3] >> 12u) & 0x7fu);
            const auto numeric = registrationDefaults && format == IrBufferFormat::Invalid ? IrTextureNumericClass::Float : VertexInputNumericClass(format);
            if (numeric == IrTextureNumericClass::Unsupported) throw std::runtime_error("unsupported prepared vertex format");
            const auto kind = numeric == IrTextureNumericClass::Float ? 0u : numeric == IrTextureNumericClass::Sint ? 1u : 2u;
            vertexClasses.at(input.location) = kind;
            std::uint32_t selectors = kind << 12u;
            auto& formatComponents = result.vertexAttributes.back().formatComponents;
            formatComponents = 1u;
            for (std::uint32_t component = 0; component < 4u; ++component) {
                const auto selector = (input.outputMask & (1u << component)) != 0u ? (resource.fields[3] >> (component * 3u)) & 7u : 0u;
                if (selector == 2u || selector == 3u) throw std::runtime_error("reserved prepared vertex component selector");
                if (selector >= 4u) formatComponents = std::max(formatComponents, selector - 3u);
                selectors |= selector << (component * 3u);
            }
            moduleKey.push_back(artifact.registrationModule ? selectors & ~0xfffu : selectors);
            if (artifact.registrationModule) runtimeSelectors.at(input.location / RuntimeAbi::VertexSelectorsPerDword) |= (selectors & 0xfffu) << (input.location % RuntimeAbi::VertexSelectorsPerDword * 16u);
        }
    }
    if (!plan->bindings.specialization.empty() || variant.bindings.layout.UsesPushData() || !artifact.vertexInputPatches.empty() || request.context.pixel) {
        {
            std::shared_lock lock(plan->mutex);
            if (const auto found = plan->modules.find(moduleKey); found != plan->modules.end()) entry = found->second;
        }
        if (entry == nullptr) {
            std::unique_lock lock(plan->mutex);
            const auto found = plan->modules.find(moduleKey);
            entry = found != plan->modules.end() ? found->second : plan->modules.emplace(moduleKey, std::make_shared<PreparedModuleEntry>()).first->second;
        }
        const auto prepare = [&] {
            auto constants = plan->bindings.specialization;
            std::size_t index = 0;
            if (variant.bindings.layout.UsesPushData()) constants.push_back({PipelineSpecialization::PushDataOffset, moduleKey[index++]});
            if (!artifact.stencilReferences.empty()) constants.push_back({PipelineSpecialization::StencilReferenceOffset, moduleKey[index++]});
            if (request.context.pixel) {
                for (std::uint32_t target = 0; target < request.context.pixel->targetExportPacking.size(); ++target) constants.push_back({PipelineSpecialization::ExportPackingBase + target, moduleKey[index++]});
                constants.push_back({PipelineSpecialization::DualSourceBlend, moduleKey[index++]});
                if (variant.info.info.rasterPrimitive) constants.push_back({PipelineSpecialization::RasterPrimitive, moduleKey[index++]});
                if (variant.info.info.layerExported) constants.push_back({PipelineSpecialization::LayerExported, moduleKey[index++]});
            }
            if (!artifact.vertexInputPatches.empty()) {
                for (const auto& input : result.vertexInputs) {
                    const auto selectors = moduleKey[index++];
                    const auto first = PipelineSpecialization::VertexBase + input.location * PipelineSpecialization::VertexWords;
                    for (std::uint32_t component = 0; component < 4u; ++component) constants.push_back({first + component, (selectors >> (component * 3u)) & 7u});
                    const auto kind = selectors >> 12u;
                    constants.push_back({first + 4u, kind == 0u ? 0x3f800000u : 1u});
                    constants.push_back({first + 5u, kind});
                }
            }
            const auto module = artifact.registrationModule && plan->bindings.specialization.empty() ? registrationModule(artifact.variantId, moduleKey, [&] { return specializeModule(variant.artifact, vertexClasses, constants, request.target); }) : specializeModule(variant.artifact, vertexClasses, constants, request.target);
            if (variant.bindings.layout.UsesPushData() && !module->pushData) throw std::runtime_error("specialization removed the prepared push constant interface");
            auto selected = DescriptorBindingBuilder{}.Select(plan->bindings, module->bindings);
            entry->bindings = std::move(selected);
            entry->module = module;
        };
        std::call_once(entry->ready, [&] {
            try {
                prepare();
            } catch (...) {
                entry->failure = std::current_exception();
            }
        });
        if (entry->failure) std::rethrow_exception(entry->failure);
        const auto& module = entry->module;
        result.specializationId = module->specializationId;
        result.spirv = module->spirv;
        bindingPlan = &entry->bindings;
        parts.module = entry;
    } else {
        result.spirv = artifact.spirv.Resolved();
    }
    parts.bindings = bindingPlan;
    if (std::none_of(bindingPlan->bindings.begin(), bindingPlan->bindings.end(), [](const PreparedDescriptorBinding& binding) { return binding.descriptor.role == DescriptorRole::FaultBuffer; })) result.bdaAbiVersion = 0u;
    return result;
}

void fillMaterializedValues(RecompileResult& result, const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot, const MaterializedParts& parts) {
    const auto& artifact = variant.artifact;
    for (std::size_t index = 0; index < result.vertexInputs.size() && index < result.vertexAttributes.size(); ++index) {
        const auto& input = result.vertexInputs[index];
        const auto& vertex = *request.context.vertex;
        result.vertexAttributes[index].resource = vertex.resources[input.location];
        result.vertexAttributes[index].fetchIndex = vertex.fetchEmbedded ? vertex.resourcesDst[input.location].fetchIndex : input.fetchIndex;
    }
    const DescriptorBindingPlan* bindingPlan = parts.bindings;
    const auto& runtimeSelectors = parts.runtimeSelectors;
    const auto tables = ResourceMaterializer::ResolveImageTables(variant.info.info, snapshot, artifact.variantId);
    DescriptorBindingBuilder{}.Populate(result, variant.bindings, *bindingPlan, variant.info.userDataBase, snapshot, partialThreads(request), &tables);
    result.workgroupMemoryDwords = WorkgroupMemoryStrideDwords(variant.info.info);
    if (!artifact.stencilReferences.empty()) {
        if (artifact.stencilReferenceDword * sizeof(std::uint32_t) != result.pushConstants.size() || result.pushConstants.size() + sizeof(std::uint32_t) > request.layout.pushConstantSizeBytes) throw std::runtime_error("stencil replay push constant lies outside the stage range");
        result.pushConstants.resize(result.pushConstants.size() + sizeof(std::uint32_t));
    }
    const auto& layout = variant.bindings.layout;
    if (artifact.registrationModule && request.shader.stage != ShaderStage::Fragment && layout.runtimeStateDwords != 0u) {
        const auto first = layout.RuntimeStateDword() + RuntimeAbi::StateVertexSelectors;
        if (layout.runtimeStateDwords > runtimeSelectors.size()) throw std::runtime_error("vertex selector state exceeds its capacity");
        if (layout.UsesPushData()) {
            if ((first + layout.runtimeStateDwords) * sizeof(std::uint32_t) > result.pushConstants.size()) throw std::runtime_error("vertex selector state lies outside the push data");
            std::memcpy(result.pushConstants.data() + first * sizeof(std::uint32_t), runtimeSelectors.data(), layout.runtimeStateDwords * sizeof(std::uint32_t));
        } else {
            const auto data = std::ranges::find_if(result.bindings, [](const DescriptorBinding& binding) { return binding.role == DescriptorRole::ShaderData; });
            if (data == result.bindings.end() || first + layout.runtimeStateDwords > data->guestDescriptor.size()) throw std::runtime_error("vertex selector state lies outside the shader data");
            std::copy_n(runtimeSelectors.begin(), layout.runtimeStateDwords, data->guestDescriptor.begin() + first);
        }
    }
    result.poisonedSrtReads = static_cast<std::uint32_t>(snapshot.srtPoison.size()) + snapshot.nullRootReads;
    result.bdaWrites = variant.info.info.bdaWrites;
    result.bdaAtomics = variant.info.info.bdaAtomics;
    result.bdaCoherent = variant.info.info.bdaCoherent;
    result.bdaSpinWait = variant.info.info.bdaSpinWait;
    result.imageTablePoison.clear();
    result.imageTableRanges.clear();
    result.imageTableReadRanges.clear();
    result.imageTableShader = 0;
    result.imageTableFaults = 0;
    if (!tables.map.empty()) {
        result.imageTablePoison.reserve(tables.poison.size());
        for (const auto& poison : tables.poison) result.imageTablePoison.push_back({poison.words, poison.dwordCount, poison.resource, static_cast<std::uint32_t>(poison.reason)});
        result.imageTableRanges = snapshot.tables.ranges;
        result.imageTableReadRanges.reserve(snapshot.tables.tables.size());
        for (const auto& table : snapshot.tables.tables) result.imageTableReadRanges.push_back(table.readRanges);
        result.imageTableShader = snapshot.tables.shader;
        result.imageTableFaults = tables.faults;
    }
}

RecompileResult materializeResult(const CompiledVariant& variant, const RecompileRequest& request, const ResourceSnapshot& snapshot, bool registrationDefaults) {
    MaterializedParts parts;
    auto result = materializeSkeleton(variant, request, snapshot, registrationDefaults, parts);
    fillMaterializedValues(result, variant, request, snapshot, parts);
    return result;
}

}

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_BDASHADER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_BDASHADER_HPP

#include "BdaTable.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <vector>

void SetBdaTestTranslation(ShaderRecompiler::BdaTable::Translation translation);
ShaderRecompiler::BdaTable::Translation BdaTestTranslation();
void SetBdaTestRelative(ShaderRecompiler::BdaTable::Relative relative);
ShaderRecompiler::BdaTable::Relative BdaTestRelative();
std::vector<std::uint32_t> MakeBdaCapabilityTestShader(std::uint64_t address, std::uint64_t capabilityBase);
std::vector<std::uint32_t> MakeBdaResolveTestShader(std::uint32_t permission);
std::vector<std::uint32_t> MakeBdaTestShader(std::uint64_t address, std::uint32_t bits, std::int64_t offset = 0);
std::vector<std::uint32_t> MakeBdaDwordReadTestShader(std::uint64_t address, std::uint32_t dwords, bool coherent, bool stops);
std::vector<std::uint32_t> MakeBdaSpanReadTestShader(std::uint64_t address, std::uint32_t offset, std::uint32_t extracted, bool coherent, bool stops);
std::vector<std::uint32_t> MakeBdaDwordWriteTestShader(std::uint64_t address, std::uint32_t dwords, const std::uint32_t* values);
std::vector<std::uint32_t> MakeBdaStoreTestShader(std::uint64_t address, std::uint32_t value, std::uint32_t bits, bool coherent);
std::vector<std::uint32_t> MakeBdaWriteTestShader(std::uint64_t address, std::uint32_t value, std::uint32_t bits, bool coherent);
void RunBdaStoreExecutionTests(const AgcDriver::Graphics::Context& context);
void RunBdaExecutionTests(const AgcDriver::Graphics::Context& context);
void RunBdaContractTests();
void RunBdaTableContractTests();
void RunSpirvStatisticsTests();
void RunBdaTranslationTests(const AgcDriver::Graphics::Context& context);

#endif

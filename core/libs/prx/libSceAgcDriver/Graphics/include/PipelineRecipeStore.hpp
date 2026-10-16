#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINERECIPESTORE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINERECIPESTORE_HPP

#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipe.hpp"
#include <filesystem>
#include <mutex>

namespace AgcDriver::Graphics {

struct PipelineRecipeStoreLimits {
    std::size_t entries = 4096;
    std::uint64_t bytes = 256u * 1024u * 1024u;
    std::size_t entryBytes = PipelineRecipeMaxBytes;
    bool evict = true;
};

enum class PipelineRecipeStoreStatus { Stored, Present, Loaded, Absent, Rejected, Full, Failed };

class PipelineRecipeStore {
public:
    explicit PipelineRecipeStore(std::filesystem::path directory, PipelineRecipeStoreLimits limits = {});
    [[nodiscard]] PipelineRecipeStoreStatus Store(const PipelineRecipe& recipe);
    [[nodiscard]] PipelineRecipeStoreStatus Load(const std::string& identity, PipelineRecipe& recipe) const;
    [[nodiscard]] PipelineRecipeStoreStatus LoadInputs(const std::string& identity, PipelineRecipe& recipe) const;
    [[nodiscard]] std::vector<std::string> List(std::size_t maximum) const;
    [[nodiscard]] std::vector<std::string> ListForPreparation(std::size_t maximum) const;

private:
    std::filesystem::path directory;
    PipelineRecipeStoreLimits limits;
    mutable std::mutex mutex;
};

}

#endif

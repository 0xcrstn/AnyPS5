#include "prx/libSceAgcDriver/Graphics/include/PipelineRecipeStore.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using namespace AgcDriver::Graphics;
using namespace ShaderRecompiler;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

PipelineRecipe fixture(std::uint32_t value) {
    auto program = std::make_shared<RecompileResult>();
    program->spirv = std::vector<std::uint32_t>{0x07230203u, 0x00010000u, 0u, 2u, 0u, 0x00020011u, 1u};
    program->specialization = {{1u, value}};
    PipelineRecipe recipe;
    recipe.stages.push_back({ShaderStage::Vertex, 0u, program, std::string(64, 'b')});
    recipe.state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    return recipe;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("anyps5-recipe-test-" + std::to_string(nonce));
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("test directory already exists");
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};

void store() {
    TemporaryDirectory temporary;
    const auto first = fixture(1), second = fixture(2), third = fixture(3);
    const auto oneSize = EncodePipelineRecipe(first).size();
    PipelineRecipeStore store(temporary.path, {2, oneSize * 2u, PipelineRecipeMaxBytes});
    require(store.Store(first) == PipelineRecipeStoreStatus::Stored, "first store failed");
    require(store.Store(second) == PipelineRecipeStoreStatus::Stored, "second store failed");
    require(store.Store(first) == PipelineRecipeStoreStatus::Present, "duplicate recipe was rewritten");
    const auto firstId = PipelineRecipeIdentity(first), secondId = PipelineRecipeIdentity(second), thirdId = PipelineRecipeIdentity(third);
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
    std::filesystem::last_write_time(temporary.path / (secondId + ".recipe"), old);
    require(store.Store(third) == PipelineRecipeStoreStatus::Stored, "full store refused replacement");
    require(store.List(100).size() == 2 && store.List(1).size() == 1, "entry or enumeration bound exceeded");
    require(store.ListForPreparation(2).size() == 2, "complete preparation enumeration failed");
    bool partialThrew = false;
    try { static_cast<void>(store.ListForPreparation(1)); } catch (const std::exception&) { partialThrew = true; }
    require(partialThrew, "preparation silently accepted partial recipe coverage");
    PipelineRecipe loaded;
    require(store.Load(secondId, loaded) == PipelineRecipeStoreStatus::Absent, "least recently used recipe was retained");
    require(store.Load(firstId, loaded) == PipelineRecipeStoreStatus::Loaded, "recent recipe was evicted");
    require(store.LoadInputs(firstId, loaded) == PipelineRecipeStoreStatus::Loaded && loaded.stages[0].program == nullptr, "inputs-only store read failed");
    require(store.Load("../escape", loaded) == PipelineRecipeStoreStatus::Rejected, "invalid filename accepted");
    {
        std::ofstream file(temporary.path / (firstId + ".recipe"), std::ios::binary | std::ios::trunc);
        file << "partial";
    }
    require(store.Load(firstId, loaded) == PipelineRecipeStoreStatus::Rejected, "torn recipe accepted");
    require(store.Load(thirdId, loaded) == PipelineRecipeStoreStatus::Loaded, "corrupt sibling poisoned valid recipe");
    require(store.Store(first) == PipelineRecipeStoreStatus::Stored, "corrupt content file could not be repaired");
    require(store.Load(firstId, loaded) == PipelineRecipeStoreStatus::Loaded, "repaired recipe failed to load");
    const auto smallPath = temporary.path / "small";
    PipelineRecipeStore small(smallPath, {2, oneSize * 2u, oneSize - 1u});
    require(small.Store(first) == PipelineRecipeStoreStatus::Full && small.List(100).empty(), "oversized entry was written");
    PipelineRecipeStore complete(temporary.path / "complete", {1, oneSize * 2u, PipelineRecipeMaxBytes, false});
    require(complete.Store(first) == PipelineRecipeStoreStatus::Stored && complete.Store(second) == PipelineRecipeStoreStatus::Full, "complete-output store silently evicted a prepared recipe");
    require(complete.Load(firstId, loaded) == PipelineRecipeStoreStatus::Loaded, "complete-output refusal removed earlier recipe");
    const auto invalidSibling = temporary.path / (std::string(32, 'c') + ".recipe");
    std::filesystem::create_directory(invalidSibling);
    require(store.List(100).size() == 2 && store.Load(firstId, loaded) == PipelineRecipeStoreStatus::Loaded, "nonregular sibling poisoned runtime enumeration");
    bool siblingThrew = false;
    try { static_cast<void>(store.ListForPreparation(100)); } catch (const std::exception&) { siblingThrew = true; }
    require(siblingThrew, "preparation ignored unreadable matching sibling");
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path)) require(entry.path().extension() != ".tmp", "atomic store left temporary file");
}

void concurrent() {
    TemporaryDirectory temporary;
    PipelineRecipeStore left(temporary.path, {2, 1024u * 1024u, PipelineRecipeMaxBytes});
    PipelineRecipeStore right(temporary.path, {2, 1024u * 1024u, PipelineRecipeMaxBytes});
    PipelineRecipeStoreStatus leftStatus = PipelineRecipeStoreStatus::Failed;
    PipelineRecipeStoreStatus rightStatus = PipelineRecipeStoreStatus::Failed;
    std::thread writerLeft([&] { leftStatus = left.Store(fixture(1)); });
    std::thread writerRight([&] { rightStatus = right.Store(fixture(2)); });
    writerLeft.join();
    writerRight.join();
    require(leftStatus == PipelineRecipeStoreStatus::Stored && rightStatus == PipelineRecipeStoreStatus::Stored, "independent store instances lost concurrent writes");
    const auto identities = left.List(100);
    require(identities.size() == 2, "concurrent stores broke entry accounting");
    for (const auto& identity : identities) {
        PipelineRecipe recipe;
        require(right.Load(identity, recipe) == PipelineRecipeStoreStatus::Loaded, "concurrent store exposed partial file");
    }
}

}

int main() {
    try {
        store();
        concurrent();
        std::cout << "pipeline recipe store tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "prx/libSceAgcDriver/Graphics/include/PassSignatures.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string>

namespace {

using AgcDriver::Graphics::PassSignatures;
using AgcDriver::Graphics::Require;

void RecordDraw(std::uint64_t pass, std::uint64_t identity, std::uint64_t pipeline, std::uint32_t vertices) {
    PassSignatures::BeginDraw(pass, identity);
    Require(PassSignatures::Active(), "a draw scope did not start with the counter enabled");
    PassSignatures::Note(1u, {pipeline}, {});
    const std::uint64_t push[2] = {0x1111u, 0x2222u};
    PassSignatures::Note(18u, {7u, 16u}, std::span<const std::uint64_t>(push));
    PassSignatures::Note(21u, {vertices, 1u, 0u, 0u}, {});
    PassSignatures::EndDraw();
    Require(!PassSignatures::Active(), "a draw scope stayed open after EndDraw");
}

void RecordFrame(std::uint64_t firstPass, std::uint32_t lastVertices, std::uint64_t lastIdentity) {
    RecordDraw(firstPass, 100u, 7u, 3u);
    RecordDraw(firstPass, 100u, 7u, 6u);
    RecordDraw(firstPass + 1u, 200u, 9u, 3u);
    RecordDraw(firstPass + 2u, lastIdentity, 9u, lastVertices);
    PassSignatures::NoteFrame();
}

void CountsRepeatedPasses() {
    PassSignatures::SetEnabled(true);
    const auto start = PassSignatures::Totals();
    RecordFrame(10u, 3u, 300u);
    const auto first = PassSignatures::Totals();
    Require(first.passes - start.passes == 3u && first.repeated == start.repeated && first.draws - start.draws == 4u, "the first frame counted " + std::to_string(first.passes - start.passes) + " passes and " + std::to_string(first.repeated - start.repeated) + " repeats, expected 3 and none");
    RecordFrame(20u, 3u, 300u);
    const auto same = PassSignatures::Totals();
    Require(same.repeated - first.repeated == 3u && same.repeatedDraws - first.repeatedDraws == 4u, "an identical frame repeated " + std::to_string(same.repeated - first.repeated) + " of 3 passes");
    RecordFrame(30u, 4u, 300u);
    const auto changedDraw = PassSignatures::Totals();
    Require(changedDraw.repeated - same.repeated == 2u && changedDraw.repeatedDraws - same.repeatedDraws == 3u, "a frame whose last draw changed its vertex count repeated " + std::to_string(changedDraw.repeated - same.repeated) + " passes, expected the 2 unchanged ones");
    RecordFrame(40u, 4u, 301u);
    const auto changedPass = PassSignatures::Totals();
    Require(changedPass.repeated - changedDraw.repeated == 2u, "a pass with other targets matched the previous frame's pass");
    RecordDraw(50u, 100u, 7u, 3u);
    RecordDraw(51u, 100u, 7u, 3u);
    PassSignatures::NoteFrame();
    RecordDraw(60u, 100u, 7u, 3u);
    RecordDraw(61u, 100u, 7u, 3u);
    RecordDraw(62u, 100u, 7u, 3u);
    PassSignatures::NoteFrame();
    const auto multiset = PassSignatures::Totals();
    Require(multiset.repeated - changedPass.repeated == 2u, "three identical passes matched " + std::to_string(multiset.repeated - changedPass.repeated) + " passes of a frame that had two, expected 2");
    PassSignatures::SetEnabled(false);
    PassSignatures::BeginDraw(70u, 100u);
    Require(!PassSignatures::Active(), "a draw scope started with the counter disabled");
}

}

int main() {
    try {
        CountsRepeatedPasses();
        std::cout << "Pass signatures repeat exactly the passes whose command streams match the previous frame" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

#pragma once

#include <cstdint>
#include <span>

namespace sogen::fuzz
{
    // One fuzz iteration against the property-list reader, which parses bytes taken straight from the
    // sample under analysis (a bundle's Info.plist).
    //
    // Engine-independent by design: this is the single entry point every driver calls. Wire it to
    // libFuzzer's LLVMFuzzerTestOneInput, a standalone file-replay main, or AFL.
    void run(std::span<const uint8_t> data);
}

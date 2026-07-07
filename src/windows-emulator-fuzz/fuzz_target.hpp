#pragma once

#include <cstdint>
#include <span>

namespace sogen::fuzz
{
    // One fuzz iteration: feed attacker-controlled bytes to the target io_device through a
    // persistent (constructed-once) headless emulator, and drive its io_control().
    //
    // Engine-independent by design: this is the single entry point every driver calls. Wire it to
    // libFuzzer's LLVMFuzzerTestOneInput, a standalone file-replay main, AFL, a unit test, or the
    // in-house fuzzing-engine's executer — the core setup lives here, not in the driver.
    void run(std::span<const uint8_t> data);
}

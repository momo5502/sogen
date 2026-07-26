#include "emulation_test_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace sogen::test
{
    // Loads a serialization-mismatch dump captured by serialization_test.cpp (locally, or downloaded from a CI
    // artifact) and deserializes it through the real windows_emulator::deserialize() path. Point
    // SOGEN_OFFSET_FINDER_FILE at the dump and set SOGEN_TRACE_OFFSETS=1 (plus SOGEN_TRACE_TARGET_OFFSET once
    // the first differing byte offset between the .a.bin/.b.bin pair is known, e.g. via `cmp -l`) to have
    // windows_emulator.cpp / memory_manager.cpp print which component and guest memory region owns that offset.
    // Skipped when the env var isn't set, so it never runs as part of a normal test pass.
    TEST(SerializationOffsetFinder, Run)
    {
        const auto* path = std::getenv("SOGEN_OFFSET_FINDER_FILE");
        if (!path)
        {
            GTEST_SKIP() << "SOGEN_OFFSET_FINDER_FILE not set";
        }

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(file.is_open()) << "Failed to open " << path;

        const auto size = static_cast<size_t>(file.tellg());
        file.seekg(0);

        std::vector<std::byte> buffer(size);
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
        file.close();

        std::printf("Loaded %zu bytes from %s\n", buffer.size(), path);

        utils::buffer_deserializer deserializer{buffer};

        auto emu = create_sample_emulator();
        emu.deserialize(deserializer);

        std::printf("Deserialized successfully. Final offset: %zu\n", deserializer.get_offset());
    }
} // namespace sogen::test

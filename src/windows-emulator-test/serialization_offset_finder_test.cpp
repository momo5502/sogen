#include "emulation_test_utils.hpp"

#include <fstream>

namespace sogen::test
{
    // TEMPORARY debug tool: deserializes a captured buffer dump (see serialization_test.cpp's
    // dump_buffer_to_file) with SOGEN_TRACE_OFFSETS=1 set, so windows_emulator::deserialize's boundary
    // trace prints which top-level component's byte range contains a given offset. Point
    // SOGEN_OFFSET_FINDER_FILE at the dump; skipped otherwise. Revert once the cause is identified.
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

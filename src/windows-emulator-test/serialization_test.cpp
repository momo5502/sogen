#include "emulation_test_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace sogen::test
{
    namespace
    {
        // On a serialization mismatch, dumps both buffers to <label>.a.bin/<label>.b.bin under
        // $GITHUB_WORKSPACE (or the working directory outside CI) and fails the test. CI uploads these as
        // artifacts; SerializationOffsetFinder.Run (serialization_offset_finder_test.cpp) replays a downloaded
        // dump through the real deserialize() code with SOGEN_TRACE_OFFSETS/SOGEN_TRACE_TARGET_OFFSET
        // (windows_emulator.cpp / memory_manager.cpp) to pin down which component owns the divergence.
        void dump_buffer_to_file(const char* label, const char* suffix, const std::vector<std::byte>& buffer)
        {
            const char* dir = std::getenv("GITHUB_WORKSPACE");
            const std::string path = (dir ? std::string(dir) + "/" : std::string()) + label + "." + suffix + ".bin";

            FILE* file = std::fopen(path.c_str(), "wb");
            if (!file)
            {
                std::printf("[serial-diff] %s: failed to open %s for writing\n", label, path.c_str());
                return;
            }

            std::fwrite(buffer.data(), 1, buffer.size(), file);
            std::fclose(file);
            std::printf("[serial-diff] %s: dumped %zu bytes to %s\n", label, buffer.size(), path.c_str());
        }

        void dump_and_expect_equal(const char* label, const std::vector<std::byte>& a, const std::vector<std::byte>& b)
        {
            if (a == b)
            {
                return;
            }

            dump_buffer_to_file(label, "a", a);
            dump_buffer_to_file(label, "b", b);

            ADD_FAILURE() << "[serial-diff] " << label << ": serialized buffers differ (dumps written above for offline diagnosis)";
        }
    }

    TEST(SerializationTest, ResettingEmulatorWorks)
    {
        auto emu = create_sample_emulator();

        utils::buffer_serializer start_state{};
        emu.serialize(start_state);

        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer end_state1{};
        emu.serialize(end_state1);

        utils::buffer_deserializer deserializer{start_state};
        emu.deserialize(deserializer);

        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer end_state2{};
        emu.serialize(end_state2);

        dump_and_expect_equal("ResettingEmulatorWorks", end_state1.get_buffer(), end_state2.get_buffer());
    }

    TEST(SerializationTest, SerializedDataIsReproducible)
    {
        auto emu1 = create_sample_emulator();
        emu1.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu1);

        utils::buffer_serializer serializer1{};
        emu1.serialize(serializer1);

        utils::buffer_deserializer deserializer{serializer1};

        auto new_emu = create_empty_emulator();
        new_emu.deserialize(deserializer);

        utils::buffer_serializer serializer2{};
        new_emu.serialize(serializer2);

        dump_and_expect_equal("SerializedDataIsReproducible", serializer1.get_buffer(), serializer2.get_buffer());
    }

    TEST(SerializationTest, EmulationIsReproducible)
    {
        auto emu1 = create_sample_emulator();
        emu1.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu1);

        utils::buffer_serializer serializer1{};
        emu1.serialize(serializer1);

        auto emu2 = create_sample_emulator();
        emu2.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu2);

        utils::buffer_serializer serializer2{};
        emu2.serialize(serializer2);

        dump_and_expect_equal("EmulationIsReproducible", serializer1.get_buffer(), serializer2.get_buffer());
    }

    TEST(SerializationTest, DeserializedEmulatorBehavesLikeSource)
    {
        auto emu = create_sample_emulator();
        emu.start(100);

        utils::buffer_serializer serializer{};
        emu.serialize(serializer);

        utils::buffer_deserializer deserializer{serializer};

        auto new_emu = create_empty_emulator();
        new_emu.deserialize(deserializer);

        new_emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(new_emu);

        emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer serializer1{};
        utils::buffer_serializer serializer2{};

        emu.serialize(serializer1);
        new_emu.serialize(serializer2);

        dump_and_expect_equal("DeserializedEmulatorBehavesLikeSource", serializer1.get_buffer(), serializer2.get_buffer());
    }
} // namespace sogen::test

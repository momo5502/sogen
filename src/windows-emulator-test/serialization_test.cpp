#include "emulation_test_utils.hpp"

namespace test
{
    TEST(SerializationTest, SerializedDataIsReproducible)
    {
        auto emu1 = create_sample_emulator();
        emu1.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu1);

        utils::buffer_serializer serializer1{};
        emu1.serialize(serializer1);

        utils::buffer_deserializer deserializer{serializer1.get_buffer()};

        windows_emulator new_emu{};
        new_emu.deserialize(deserializer);

        utils::buffer_serializer serializer2{};
        new_emu.serialize(serializer2);

        auto buffer1 = serializer1.move_buffer();
        auto buffer2 = serializer2.move_buffer();

        ASSERT_EQ(serializer1.get_buffer(), serializer2.get_buffer());
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

        ASSERT_EQ(serializer1.get_buffer(), serializer2.get_buffer());
    }

    TEST(SerializationTest, DeserializedEmulatorBehavesLikeSource)
    {
        auto emu = create_sample_emulator();
        emu.start({}, 100);

        utils::buffer_serializer serializer{};
        emu.serialize(serializer);

        utils::buffer_deserializer deserializer{serializer.get_buffer()};

        windows_emulator new_emu{};
        new_emu.log.disable_output(true);
        new_emu.deserialize(deserializer);

        new_emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(new_emu);

        emu.start();
        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        utils::buffer_serializer serializer1{};
        utils::buffer_serializer serializer2{};

        emu.serialize(serializer1);
        new_emu.serialize(serializer2);

        ASSERT_EQ(serializer1.get_buffer(), serializer2.get_buffer());
    }
}

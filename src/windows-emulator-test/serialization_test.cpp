#include "emulation_test_utils.hpp"

namespace test
{
	TEST(SerializationTest, SerializedDataIsReproducible)
	{
		windows_emulator emu1{"./test-sample.exe"};
		emu1.logger.disable_output(true);
		emu1.start();

		ASSER_TERMINATED_SUCCESSFULLY(emu1);

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

	TEST(SerializationTest, DISABLED_EmulationIsReproducible)
	{
		windows_emulator emu1{"./test-sample.exe"};
		emu1.logger.disable_output(true);
		emu1.start();

		ASSER_TERMINATED_SUCCESSFULLY(emu1);

		utils::buffer_serializer serializer1{};
		emu1.serialize(serializer1);

		windows_emulator emu2{"./test-sample.exe"};
		emu2.logger.disable_output(true);
		emu2.start();

		ASSER_TERMINATED_SUCCESSFULLY(emu2);

		utils::buffer_serializer serializer2{};
		emu2.serialize(serializer2);

		ASSERT_EQ(serializer1.get_buffer(), serializer2.get_buffer());
	}

	TEST(SerializationTest, DeserializedEmulatorBehavesLikeSource)
	{
		windows_emulator emu{"./test-sample.exe"};
		emu.logger.disable_output(true);
		emu.start({}, 100);

		utils::buffer_serializer serializer{};
		emu.serialize(serializer);

		utils::buffer_deserializer deserializer{serializer.get_buffer()};

		windows_emulator new_emu{};
		new_emu.logger.disable_output(true);
		new_emu.deserialize(deserializer);

		new_emu.start();
		ASSER_TERMINATED_SUCCESSFULLY(new_emu);

		emu.start();
		ASSER_TERMINATED_SUCCESSFULLY(emu);

		// TODO: Fix that :(
		/*utils::buffer_serializer serializer1{};
		utils::buffer_serializer serializer2{};

		emu.serialize(serializer1);
		new_emu.serialize(serializer2);

		ASSERT_EQ(serializer1.get_buffer(), serializer2.get_buffer());*/
	}
}

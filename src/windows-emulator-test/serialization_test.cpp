#include <gtest/gtest.h>
#include <windows_emulator.hpp>


namespace test
{
	TEST(SerializationTest, BasicSerializationWorks)
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
		emu.start();

		utils::buffer_serializer serializer1{};
		utils::buffer_serializer serializer2{};

		emu.serialize(serializer1);
		new_emu.serialize(serializer2);

		ASSERT_EQ(serializer1.get_buffer(), serializer2.get_buffer());
	}
}

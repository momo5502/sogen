#include "emulation_test_utils.hpp"

namespace sogen::test
{
    // A WHP partition with several vCPUs saves and restores vCPU 0, which runs the one thread a snapshot keeps.
    TEST(MultiVcpuSnapshotTest, WhpPartitionWithSeveralVcpusSavesItsFirstVcpu)
    {
        if (get_x86_64_emulator_backend_from_environment() != backend_type::whp)
        {
            GTEST_SKIP() << "Multiple vCPUs need WHP (EMULATOR_WHP=1)";
        }

        const auto backend = create_x86_64_emulator_from_environment(2);
        backend->reg(x86_register::rax, 0x1234);
        backend->reg(x86_register::rip, 0x5000);

        utils::buffer_serializer state{};
        backend->serialize_state(state, false);
        backend->reg(x86_register::rax, 0);
        backend->reg(x86_register::rip, 0);

        utils::buffer_deserializer deserializer{state};
        backend->deserialize_state(deserializer, false);
        EXPECT_EQ(0x1234u, backend->reg(x86_register::rax));
        EXPECT_EQ(0x5000u, backend->reg(x86_register::rip));
    }
}

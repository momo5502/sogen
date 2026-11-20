#include "std_include.hpp"

#include "rpc_test_utils.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtAlpcCreateResourceReserve(const syscall_context& c, handle port_handle, ULONG flags, uint64_t message_size,
                                                emulator_object<handle> resource_id);
    NTSTATUS handle_NtAlpcDeleteResourceReserve(const syscall_context& c, handle port_handle, ULONG flags, handle resource_id);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t message_size = 0x100;

        class AlpcResourceReserveTest : public RpcProtocolTest
        {
          protected:
            void SetUp() override
            {
                RpcProtocolTest::SetUp();
                this->first_port = this->add_port();
                this->second_port = this->add_port();
            }

            handle add_port()
            {
                return this->emu->process.ports.store(port_container{u"", *this->emu, {}});
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            std::optional<handle> create(const handle port)
            {
                const emulator_object<handle> id{this->emu->memory, this->storage};
                if (!NT_SUCCESS(syscalls::handle_NtAlpcCreateResourceReserve(this->context(), port, 0, message_size, id)))
                {
                    return std::nullopt;
                }
                return id.read();
            }

            NTSTATUS remove(const handle port, const handle reserve)
            {
                return syscalls::handle_NtAlpcDeleteResourceReserve(this->context(), port, 0, reserve);
            }

            handle first_port{};
            handle second_port{};
        };
    }

    TEST_F(AlpcResourceReserveTest, ReservesAreIndependentAndDeletedOnce)
    {
        const auto first = this->create(this->first_port);
        const auto second = this->create(this->first_port);
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        EXPECT_NE(first->bits, second->bits);

        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, *first));
        EXPECT_FALSE(NT_SUCCESS(this->remove(this->first_port, *first)));
        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, *second));
    }

    TEST_F(AlpcResourceReserveTest, AReserveBelongsToThePortThatCreatedIt)
    {
        const auto reserve = this->create(this->first_port);
        ASSERT_TRUE(reserve.has_value());
        EXPECT_FALSE(NT_SUCCESS(this->remove(this->second_port, *reserve)));
        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, *reserve));
    }

    TEST_F(AlpcResourceReserveTest, ReservesSurviveASnapshot)
    {
        const auto reserve = this->create(this->first_port);
        ASSERT_TRUE(reserve.has_value());

        auto& container = *this->emu->process.ports.get(this->first_port);
        utils::buffer_serializer serializer{};
        container.get_internal_port()->serialize(serializer);
        auto& restored = *container.get_internal_port();
        restored.resource_reserves.clear();
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        const auto next = this->create(this->first_port);
        ASSERT_TRUE(next.has_value());
        EXPECT_NE(reserve->bits, next->bits);
        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, *reserve));
    }

    TEST_F(AlpcResourceReserveTest, InvalidRequestsCreateNoReserve)
    {
        const emulator_object<handle> id{this->emu->memory, this->storage};
        EXPECT_FALSE(NT_SUCCESS(syscalls::handle_NtAlpcCreateResourceReserve(this->context(), this->first_port, 1, message_size, id)));
        EXPECT_FALSE(NT_SUCCESS(syscalls::handle_NtAlpcCreateResourceReserve(this->context(), this->first_port, 0, 0, id)));
        EXPECT_TRUE(this->emu->process.ports.get(this->first_port)->get_internal_port()->resource_reserves.empty());
    }
}

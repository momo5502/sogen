#include "std_include.hpp"

#include "rpc_test_utils.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtAlpcCreateResourceReserve(const syscall_context& c, handle port_handle, ULONG flags, uint64_t message_size,
                                                emulator_object<ULONG> resource_id);
    NTSTATUS handle_NtAlpcDeleteResourceReserve(const syscall_context& c, handle port_handle, ULONG flags, ULONG resource_id);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t message_size = 0x100;
        constexpr ULONG guard_value = 0xA5A5A5A5;
        constexpr ULONG reserve_id_bit = 0x80000000;

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

            // The identifier is a ULONG; the DWORD after it must stay untouched.
            NTSTATUS create(const handle port, ULONG& id, const uint64_t size = message_size)
            {
                this->emu->memory.write_memory(this->storage, ULONG{0});
                this->emu->memory.write_memory(this->storage + sizeof(ULONG), guard_value);
                const auto status =
                    syscalls::handle_NtAlpcCreateResourceReserve(this->context(), port, 0, size, {this->emu->memory, this->storage});
                id = this->emu->memory.read_memory<ULONG>(this->storage);
                EXPECT_EQ(guard_value, this->emu->memory.read_memory<ULONG>(this->storage + sizeof(ULONG)));
                return status;
            }

            NTSTATUS remove(const handle port, const ULONG id)
            {
                return syscalls::handle_NtAlpcDeleteResourceReserve(this->context(), port, 0, id);
            }

            size_t reserve_count(const handle port) const
            {
                return this->emu->process.ports.get(port)->get_internal_port()->resource_reserves.size();
            }

            handle first_port{};
            handle second_port{};
        };
    }

    TEST_F(AlpcResourceReserveTest, ReservesAreIndependentAndDeletedOnce)
    {
        ULONG first{};
        ULONG second{};
        ASSERT_EQ(STATUS_SUCCESS, this->create(this->first_port, first));
        ASSERT_EQ(STATUS_SUCCESS, this->create(this->first_port, second));
        EXPECT_NE(0u, first & reserve_id_bit);
        EXPECT_NE(first, second);

        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, first));
        EXPECT_FALSE(NT_SUCCESS(this->remove(this->first_port, first)));
        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, second));
    }

    TEST_F(AlpcResourceReserveTest, AReserveBelongsToThePortThatCreatedIt)
    {
        ULONG reserve{};
        ASSERT_EQ(STATUS_SUCCESS, this->create(this->first_port, reserve));
        EXPECT_FALSE(NT_SUCCESS(this->remove(this->second_port, reserve)));
        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, reserve));
    }

    TEST_F(AlpcResourceReserveTest, AnIdentifierWithoutTheReserveBitIsInvalid)
    {
        ULONG reserve{};
        ASSERT_EQ(STATUS_SUCCESS, this->create(this->first_port, reserve));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->remove(this->first_port, reserve & ~reserve_id_bit));
        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, reserve));
    }

    TEST_F(AlpcResourceReserveTest, ReservesSurviveASnapshot)
    {
        ULONG reserve{};
        ASSERT_EQ(STATUS_SUCCESS, this->create(this->first_port, reserve));

        auto& container = *this->emu->process.ports.get(this->first_port);
        utils::buffer_serializer serializer{};
        container.get_internal_port()->serialize(serializer);
        auto& restored = *container.get_internal_port();
        restored.resource_reserves.clear();
        utils::buffer_deserializer deserializer{serializer};
        restored.deserialize(deserializer);

        ULONG next{};
        ASSERT_EQ(STATUS_SUCCESS, this->create(this->first_port, next));
        EXPECT_NE(reserve, next);
        EXPECT_EQ(STATUS_SUCCESS, this->remove(this->first_port, reserve));
    }

    // AlpcpCreateReserve takes message sizes from a PORT_MESSAGE header (0x28) up to 0xFFD7.
    TEST_F(AlpcResourceReserveTest, MessageSizesOutsideTheAlpcLimitsCreateNoReserve)
    {
        ULONG id{};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create(this->first_port, id, 0x27));
        EXPECT_EQ(STATUS_BUFFER_OVERFLOW, this->create(this->first_port, id, 0xFFD8));
        EXPECT_EQ(0u, this->reserve_count(this->first_port));

        EXPECT_EQ(STATUS_SUCCESS, this->create(this->first_port, id, 0x28));
        EXPECT_EQ(STATUS_SUCCESS, this->create(this->first_port, id, 0xFFD7));
    }

    // AlpcpCreateReserve checks the size before whether the port is disconnected.
    TEST_F(AlpcResourceReserveTest, ADisconnectedPortReportsTheSizeErrorFirst)
    {
        this->emu->process.ports.get(this->first_port)->get_internal_port()->disconnected = true;
        ULONG id{};
        EXPECT_EQ(STATUS_BUFFER_OVERFLOW, this->create(this->first_port, id, 0xFFD8));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->create(this->first_port, id, 0x27));
        EXPECT_EQ(STATUS_PORT_DISCONNECTED, this->create(this->first_port, id));
    }

    TEST_F(AlpcResourceReserveTest, AnUnwritableIdentifierCreatesNoReserve)
    {
        EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                  syscalls::handle_NtAlpcCreateResourceReserve(this->context(), this->first_port, 0, message_size, {this->emu->memory}));
        EXPECT_FALSE(NT_SUCCESS(syscalls::handle_NtAlpcCreateResourceReserve(this->context(), this->first_port, 1, message_size,
                                                                             {this->emu->memory, this->storage})));
        EXPECT_EQ(0u, this->reserve_count(this->first_port));
    }
}

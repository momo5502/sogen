#include "std_include.hpp"

#include "rpc_test_utils.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, handle port_handle, ULONG flags,
                                              emulator_object<PORT_MESSAGE64> send_message,
                                              emulator_object<ALPC_MESSAGE_ATTRIBUTES> send_message_attributes,
                                              emulator_object<PORT_MESSAGE64> receive_message, emulator_object<uint64_t> buffer_length,
                                              emulator_object<ALPC_MESSAGE_ATTRIBUTES> receive_message_attributes,
                                              emulator_object<LARGE_INTEGER> timeout);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> endpoint_mapper_interface{0x08, 0x83, 0xaf, 0xe1, 0x1f, 0x5d, 0xc9, 0x11,
                                                                    0x91, 0xa4, 0x08, 0x00, 0x2b, 0x14, 0xa0, 0xfa};

        class AlpcReplyAttributeTest : public RpcProtocolTest
        {
          protected:
            void SetUp() override
            {
                RpcProtocolTest::SetUp();
                this->port_handle = this->emu->process.ports.store(port_container{u"\\RPC Control\\epmapper", *this->emu, {}});
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            // An RPC bind, which the endpoint mapper answers with a small reply that carries no view.
            NTSTATUS send_bind(const ULONG allocated, const ULONG valid, const std::optional<uint64_t> receive_attributes = std::nullopt)
            {
                std::array<uint8_t, 72> bind{};
                bind[0] = 1;
                std::memcpy(bind.data() + 12, endpoint_mapper_interface.data(), endpoint_mapper_interface.size());
                bind[32] = 3;

                PORT_MESSAGE64 message{};
                message.u1.s1.DataLength = static_cast<CSHORT>(bind.size());
                message.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(message) + bind.size());
                emulator_object<PORT_MESSAGE64>{this->emu->memory, this->storage + send_offset}.write(message);
                this->emu->memory.write_memory(this->storage + send_offset + sizeof(message), bind.data(), bind.size());

                this->emu->memory.set_memory(this->storage + attributes_offset, 0, 0x100);
                this->attributes().write({.AllocatedAttributes = allocated, .ValidAttributes = valid});
                emulator_object<uint64_t>{this->emu->memory, this->storage + length_offset}.write(0x1000);

                return syscalls::handle_NtAlpcSendWaitReceivePort(
                    this->context(), this->port_handle, 0, {this->emu->memory, this->storage + send_offset}, {this->emu->memory},
                    {this->emu->memory, this->storage + receive_offset}, {this->emu->memory, this->storage + length_offset},
                    receive_attributes ? emulator_object<ALPC_MESSAGE_ATTRIBUTES>{this->emu->memory, *receive_attributes}
                                       : this->attributes(),
                    {this->emu->memory});
            }

            emulator_object<ALPC_MESSAGE_ATTRIBUTES> attributes() const
            {
                return {this->emu->memory, this->storage + attributes_offset};
            }

            static constexpr uint64_t attributes_offset = 0x200;
            static constexpr uint64_t length_offset = 0x300;
            static constexpr uint64_t send_offset = 0x400;
            static constexpr uint64_t receive_offset = 0x2000;

            handle port_handle{};
        };
    }

    // Callers receive the reply into the attributes they sent the request with. A reply exposes only what it carries: this
    // one has no view, handles or token, so none of the request's bits stay valid, and the allocation is kept.
    TEST_F(AlpcReplyAttributeTest, ReplyReportsOnlyTheAttributesItCarries)
    {
        constexpr ULONG request_bits = ALPC_MESSAGE_VIEW_ATTRIBUTE | ALPC_MESSAGE_HANDLE_ATTRIBUTE | ALPC_MESSAGE_TOKEN_ATTRIBUTE;
        constexpr ULONG allocated = request_bits | ALPC_MESSAGE_CONTEXT_ATTRIBUTE;
        ASSERT_EQ(STATUS_SUCCESS, this->send_bind(allocated, request_bits));

        const auto attributes = this->attributes().read();
        EXPECT_EQ(allocated, attributes.AllocatedAttributes);
        EXPECT_EQ(0u, attributes.ValidAttributes & request_bits);
    }

    TEST_F(AlpcReplyAttributeTest, UnmappedReceiveAttributesFailTheCall)
    {
        const auto unmapped = this->emu->memory.find_free_allocation_base(0x1000);
        ASSERT_NE(0u, unmapped);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->send_bind(ALPC_MESSAGE_VIEW_ATTRIBUTE, ALPC_MESSAGE_VIEW_ATTRIBUTE, unmapped));
    }
}

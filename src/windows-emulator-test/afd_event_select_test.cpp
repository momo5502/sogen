#include "emulation_test_utils.hpp"

#include <devices/afd_types.hpp>

#include <array>

namespace sogen::test
{
    namespace
    {
        constexpr ULONG ioctl_afd_event_select = 0x12087;

        struct endpoint_creation_data
        {
            uint64_t flags{};
            std::array<char, 16> packet{};
            uint64_t group{};
            int32_t address_family{2};
            int32_t type{1};
            int32_t protocol{6};
        };

        class AfdEventSelectTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.socket_factory = network::create_static_socket_factory();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);

                const endpoint_creation_data creation{};
                this->emu->memory.write_memory(this->storage, &creation, sizeof(creation));
                this->endpoint = this->emu->process.devices.store(
                    io_device_container{u"Afd\\Endpoint", *this->emu, {.buffer = this->storage, .length = sizeof(creation)}});
            }

            NTSTATUS event_select(const handle event, const ULONG mask)
            {
                AFD_EVENT_SELECT_INFO info{};
                info.Event = event;
                info.PollEvents = mask;
                this->emu->memory.write_memory(this->storage + 0x100, &info, sizeof(info));

                io_device_context request{this->emu->memory};
                request.source_handle = this->endpoint;
                request.io_control_code = ioctl_afd_event_select;
                request.io_status_block = {this->emu->memory, this->storage + 0x200};
                request.input_buffer = this->storage + 0x100;
                request.input_buffer_length = sizeof(info);
                return this->emu->process.devices.get(this->endpoint)->execute_ioctl(*this->emu, request);
            }

            std::unique_ptr<windows_emulator> emu{};
            handle endpoint{};
            uint64_t storage{};
        };
    }

    TEST_F(AfdEventSelectTest, RejectsANonEventWithoutHoldingItOrDroppingTheRegistration)
    {
        const auto event_handle = this->emu->process.events.store(event{});
        ASSERT_EQ(STATUS_SUCCESS, this->event_select(event_handle, 1));
        EXPECT_EQ(2u, this->emu->process.events.get(event_handle)->ref_count);

        // The socket's own handle is not an event: AfdEventSelect references it with ExEventObjectType, so it must be
        // rejected without being held (holding it would pin the device past its last handle), and the registration kept.
        ASSERT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->event_select(this->endpoint, 1));
        EXPECT_EQ(1u, this->emu->process.devices.get(this->endpoint)->ref_count);
        EXPECT_EQ(2u, this->emu->process.events.get(event_handle)->ref_count);

        // A closed non-event handle has no table entry: it fails as invalid before the type is compared.
        const auto stale = this->emu->process.mutants.store(mutant{});
        ASSERT_TRUE(this->emu->process.mutants.erase(stale));
        ASSERT_EQ(STATUS_INVALID_HANDLE, this->event_select(stale, 1));
        EXPECT_EQ(2u, this->emu->process.events.get(event_handle)->ref_count);

        // With nothing pinning the device, closing its last handle releases it.
        ASSERT_TRUE(this->emu->process.devices.erase(this->endpoint));
        EXPECT_EQ(nullptr, this->emu->process.devices.get(this->endpoint));
    }
}

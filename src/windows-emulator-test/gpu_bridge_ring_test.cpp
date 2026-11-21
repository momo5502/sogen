#include "std_include.hpp"

#include <gpu_bridge_protocol.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
}

namespace sogen::test
{
    namespace
    {
        constexpr int32_t vk_success = 0;
        constexpr int32_t vk_error_initialization_failed = -3;
        constexpr uint64_t output_offset = 0x100;
        // Half a record header past the boundary every record starts on.
        constexpr uint32_t head_off_the_boundary = gpu_bridge::command_ring_alignment / 2;

        // A SogenGpu handle whose command ring a test writes as the shim does. No host Vulkan device is needed: a record for
        // a device the bridge does not know fails at once, as a record whose call failed on the host.
        class GpuBridgeRingTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->bridge = this->emu->process.devices.store(io_device_container{u"SogenGpu", *this->emu, {}});
            }

            syscall_context context() const
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            template <typename Response>
            Response call(const uint32_t code)
            {
                io_device_context request{this->emu->memory};
                request.source_handle = this->bridge;
                request.io_control_code = code;
                request.io_status_block = emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>{this->emu->memory, this->storage};
                request.output_buffer = this->storage + output_offset;
                request.output_buffer_length = sizeof(Response);
                request.vcpu = &this->emu->vcpu(0);
                EXPECT_EQ(STATUS_SUCCESS, this->emu->process.devices.get(this->bridge)->execute_ioctl(*this->emu, request));
                return this->emu->memory.read_memory<Response>(this->storage + output_offset);
            }

            // The ring's guest address, or 0 where the backend maps no host memory into the guest.
            uint64_t create_ring()
            {
                const auto response = this->call<gpu_bridge::create_command_ring_response>(gpu_bridge::ioctl_create_command_ring);
                return response.vk_result == vk_success ? response.guest_address : 0;
            }

            int32_t drain()
            {
                return this->call<gpu_bridge::result_response>(gpu_bridge::ioctl_drain_command_ring).vk_result;
            }

            gpu_bridge::command_ring_header header() const
            {
                return this->emu->memory.read_memory<gpu_bridge::command_ring_header>(this->ring);
            }

            void publish(const uint32_t head) const
            {
                this->emu->memory.write_memory(this->ring + offsetof(gpu_bridge::command_ring_header, head), &head, sizeof(head));
            }

            // Writes a record at `head` as the shim's ring_append does, and moves `head` past it.
            template <typename Payload>
            void append(uint32_t& head, const gpu_bridge::command command, const Payload& payload) const
            {
                const gpu_bridge::command_ring_record record{
                    .size = sizeof(payload), .command = static_cast<uint32_t>(command), .thread_id = 0, .sequence = 1};
                const auto at = this->ring + gpu_bridge::command_ring_data_offset + head;
                this->emu->memory.write_memory(at, &record, sizeof(record));
                this->emu->memory.write_memory(at + sizeof(record), &payload, sizeof(payload));
                head += gpu_bridge::command_ring_record_bytes(sizeof(payload));
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            handle bridge{};
            uint64_t ring{};
        };
    }

    // Closing the last SogenGpu handle unmaps its ring from the guest before the ring's host memory is freed: the guest
    // keeps the address, and its writes there reached freed host memory.
    TEST_F(GpuBridgeRingTest, ClosingTheHandleUnmapsTheRing)
    {
        this->ring = this->create_ring();
        if (this->ring == 0)
        {
            GTEST_SKIP() << "No command ring here: the " << this->emu->emu().get_name()
                         << " backend maps no host memory into the guest, or the host has no threads";
        }

        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), this->bridge));
        EXPECT_EQ(memory_region_kind::free, this->emu->memory.get_region_info(this->ring).kind);
    }

    // A head off the record boundary, which no writer of records publishes, stops the ring and is reported once. The tail
    // stays on the boundary, where every record header fits before the end of the data; moved to that head, it made the
    // next records read from past the end of the ring's host memory.
    TEST_F(GpuBridgeRingTest, AHeadOffTheRecordBoundaryStopsTheRing)
    {
        this->ring = this->create_ring();
        if (this->ring == 0)
        {
            GTEST_SKIP() << "No command ring here: the " << this->emu->emu().get_name()
                         << " backend maps no host memory into the guest, or the host has no threads";
        }

        this->publish(head_off_the_boundary);
        EXPECT_EQ(vk_error_initialization_failed, this->drain());
        EXPECT_EQ(0u, this->header().tail);
        EXPECT_EQ(1u, this->header().failed_records);

        this->drain();
        EXPECT_EQ(0u, this->header().tail);
        EXPECT_EQ(1u, this->header().failed_records) << "a stopped ring runs no record";
    }

    // A failure the app has not been told of stays in the ring across a snapshot, though the ring's header page is host
    // memory, which a snapshot leaves out.
    TEST_F(GpuBridgeRingTest, AFailureNotReportedYetSurvivesASnapshot)
    {
        this->ring = this->create_ring();
        if (this->ring == 0)
        {
            GTEST_SKIP() << "No command ring here: the " << this->emu->emu().get_name()
                         << " backend maps no host memory into the guest, or the host has no threads";
        }

        uint32_t head = 0;
        this->append(head, gpu_bridge::command::reset_command_pool, gpu_bridge::reset_command_pool_request{});
        this->publish(head);
        EXPECT_EQ(vk_error_initialization_failed, this->drain());

        utils::buffer_serializer snapshot{};
        this->emu->prepare_serialization();
        this->emu->serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        this->emu->deserialize(input);

        const auto restored = this->header();
        EXPECT_EQ(vk_error_initialization_failed, restored.first_error);
        EXPECT_EQ(1u, restored.failed_records);
        EXPECT_EQ(head, restored.tail);
    }
}

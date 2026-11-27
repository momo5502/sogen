#include "std_include.hpp"

#include <devices/mountmgr.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                 emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                 emulator_object<LARGE_INTEGER> allocation_size, ULONG file_attributes, ULONG share_access,
                                 ULONG create_disposition, ULONG create_options, uint64_t ea_buffer, ULONG ea_length);
    NTSTATUS handle_NtSetInformationFile(const syscall_context& c, handle file_handle,
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t file_information,
                                         ULONG length, FILE_INFORMATION_CLASS info_class);
    NTSTATUS handle_NtDeviceIoControlFile(const syscall_context& c, handle file_handle, handle event, emulator_pointer apc_routine,
                                          emulator_pointer apc_context,
                                          emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG io_control_code,
                                          emulator_pointer input_buffer, ULONG input_buffer_length, emulator_pointer output_buffer,
                                          ULONG output_buffer_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint8_t untouched_byte = 0xCC;
        constexpr uint64_t untouched_information = 0xDEAD;

        struct volume_name_row
        {
            std::u16string_view path{};
            ULONG output_length{};
            uint64_t event{};
            bool null_output{};
            bool null_status_block{};
            NTSTATUS status{};
            uint64_t information{};
            bool name_length_written{};
            bool name_written{};
        };

        class VolumeDeviceNameTest : public testing::Test
        {
          protected:
            std::filesystem::path directory =
                std::filesystem::temp_directory_path() /
                ("sogen-volume-device-name-test-" + std::to_string(getpid()) + "-" + std::to_string(std::random_device{}()));
            windows_emulator emu = create_empty_emulator();
            uint64_t storage{};
            uint64_t page_before_hole{};
            emulator_thread* issuing_thread{};

            void SetUp() override
            {
                ASSERT_TRUE(std::filesystem::create_directories(this->directory / "c" / "Windows"));
                ASSERT_TRUE(std::filesystem::create_directories(this->directory / "d"));
                this->emu.file_sys.map(windows_path{'c', {}}, this->directory / "c");
                this->emu.file_sys.map(windows_path{'d', {}}, this->directory / "d");
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);

                const auto region = this->emu.memory.allocate_memory(0x2000, memory_permission::read_write);
                ASSERT_TRUE(this->emu.memory.release_memory(region + 0x1000, 0x1000));
                ASSERT_FALSE(this->emu.memory.get_region_info(region + 0x1000).is_committed);
                this->page_before_hole = region;

                auto& threads = this->emu.process.threads;
                this->issuing_thread =
                    threads.get(threads.store(emulator_thread{this->emu.memory, this->emu.process, 0, 0, 0x100000, 0, 8, true}));
                this->emu.vcpu(0).active_thread = this->issuing_thread;
            }

            void TearDown() override
            {
                std::error_code error{};
                std::filesystem::remove_all(this->directory, error);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            NTSTATUS query_name(const handle volume, const handle event, const uint64_t status_block, const uint64_t output,
                                const ULONG output_length, const uint64_t input = 0, const ULONG input_length = 0,
                                const uint64_t apc_routine = 0, const uint64_t apc_context = 0)
            {
                return syscalls::handle_NtDeviceIoControlFile(this->context(), volume, event, apc_routine, apc_context,
                                                              {this->emu.memory, status_block}, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, input,
                                                              input_length, output, output_length);
            }

            handle open(const std::u16string_view path, const ULONG create_options = FILE_SYNCHRONOUS_IO_NONALERT)
            {
                const auto name_bytes = static_cast<USHORT>(path.size() * sizeof(char16_t));
                this->emu.memory.write_memory(this->storage + 0x400, path.data(), name_bytes);
                this->emu.memory.write_memory(this->storage + 0x300, UNICODE_STRING<EmulatorTraits<Emu64>>{
                                                                         .Length = name_bytes,
                                                                         .MaximumLength = name_bytes,
                                                                         .Buffer = this->storage + 0x400,
                                                                     });
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> attributes{};
                attributes.Length = sizeof(attributes);
                attributes.ObjectName = this->storage + 0x300;
                this->emu.memory.write_memory(this->storage + 0x200, attributes);

                const emulator_object<handle> file_handle{this->emu.memory, this->storage};
                file_handle.write({});

                constexpr ACCESS_MASK access = FILE_READ_ATTRIBUTES | SYNCHRONIZE;
                constexpr ULONG share_all = 0x7;
                const auto status = syscalls::handle_NtCreateFile(
                    this->context(), file_handle, access, {this->emu.memory, this->storage + 0x200},
                    {this->emu.memory, this->storage + 0x10}, {this->emu.memory}, 0, share_all, FILE_OPEN, create_options, 0, 0);
                EXPECT_EQ(STATUS_SUCCESS, status);

                return file_handle.read();
            }
        };
    }

    TEST_F(VolumeDeviceNameTest, MountDeviceNameQueryOnVolumeHandles)
    {
        constexpr std::array<volume_name_row, 15> rows{{
            {u"\\??\\C:", 0x208, 0, false, false, STATUS_SUCCESS, 48, true, true},
            {u"\\??\\C:", 0x40, 0, false, false, STATUS_SUCCESS, 48, true, true},
            {u"\\??\\C:", 8, 0, false, false, STATUS_BUFFER_OVERFLOW, 2, true, false},
            {u"\\??\\C:", 4, 0, false, false, STATUS_BUFFER_OVERFLOW, 2, true, false},
            {u"\\??\\C:", 3, 0, false, false, STATUS_INVALID_PARAMETER, untouched_information, false, false},
            {u"\\??\\C:", 2, 0, false, false, STATUS_INVALID_PARAMETER, untouched_information, false, false},
            {u"\\??\\C:", 0, 0, false, false, STATUS_INVALID_PARAMETER, untouched_information, false, false},
            {u"\\??\\c:", 0x208, 0, false, false, STATUS_SUCCESS, 48, true, true},
            {u"\\??\\C:\\", 0x208, 0, false, false, STATUS_INVALID_PARAMETER, untouched_information, false, false},
            {u"\\??\\C:\\Windows", 0x208, 0, false, false, STATUS_INVALID_PARAMETER, untouched_information, false, false},
            {u"\\??\\D:", 0x208, 0, false, false, STATUS_SUCCESS, 48, true, true},
            {u"\\??\\C:", 8, 0, true, false, STATUS_INVALID_PARAMETER, untouched_information, false, false},
            {u"\\??\\C:", 0, 0, true, false, STATUS_INVALID_PARAMETER, untouched_information, false, false},
            {u"\\??\\C:", 0x208, 0, false, true, STATUS_ACCESS_VIOLATION, untouched_information, false, false},
            {u"\\??\\C:", 0x208, 0x1234, false, false, STATUS_INVALID_HANDLE, untouched_information, false, false},
        }};

        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status_block{this->emu.memory, this->storage + 0x40};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> null_status_block{this->emu.memory};
        const auto output = this->storage + 0x800;

        for (const auto& row : rows)
        {
            SCOPED_TRACE(u16_to_u8(std::u16string(row.path)) + " out=" + std::to_string(row.output_length));

            const auto volume = this->open(row.path);
            const auto device_name = windows_path{static_cast<char>(std::tolower(row.path[4])), {}}.volume_device_name();
            this->emu.memory.set_memory(output, untouched_byte, 0x400);
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial_block{};
            initial_block.Information = untouched_information;
            status_block.write(initial_block);

            handle event{};
            event.bits = row.event;

            const auto status_block_argument = row.null_status_block ? null_status_block : status_block;
            EXPECT_EQ(row.status, syscalls::handle_NtDeviceIoControlFile(this->context(), volume, event, 0, 0, status_block_argument,
                                                                         IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, 0, 0,
                                                                         row.null_output ? 0 : output, row.output_length));

            EXPECT_EQ(row.information, status_block.read().Information);

            std::array<uint8_t, 0x400> buffer{};
            this->emu.memory.read_memory(output, buffer.data(), buffer.size());

            USHORT name_length{};
            std::memcpy(&name_length, buffer.data(), sizeof(name_length));

            const auto untouched_from = row.name_written ? sizeof(USHORT) + name_length : row.name_length_written ? sizeof(USHORT) : 0;
            EXPECT_TRUE(
                std::ranges::all_of(std::span{buffer}.subspan(untouched_from), [](const uint8_t b) { return b == untouched_byte; }));

            if (row.name_length_written)
            {
                EXPECT_EQ(device_name.size() * sizeof(char16_t), name_length);
            }

            if (row.name_written)
            {
                EXPECT_EQ(device_name,
                          std::u16string(reinterpret_cast<const char16_t*>(buffer.data() + sizeof(USHORT)), device_name.size()));
            }
        }
    }

    TEST_F(VolumeDeviceNameTest, ProbesStatusBlockAndWholeOutputBeforeWriting)
    {
        const auto volume = this->open(u"\\??\\C:");
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status_block{this->emu.memory, this->storage + 0x40};
        const auto unmapped_page = this->page_before_hole + 0x1000;
        constexpr ULONG name_bytes_with_length = 48;
        const auto output_at_page_end = unmapped_page - name_bytes_with_length;

        const auto reset = [&] {
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial_block{};
            initial_block.Information = untouched_information;
            status_block.write(initial_block);
            this->emu.memory.set_memory(this->storage + 0x800, untouched_byte, 0x400);
            this->emu.memory.set_memory(output_at_page_end, untouched_byte, name_bytes_with_length);
        };

        const auto output_untouched = [&](const uint64_t output, const size_t size) {
            std::array<uint8_t, 0x400> buffer{};
            this->emu.memory.read_memory(output, buffer.data(), size);
            return std::ranges::all_of(std::span{buffer}.first(size), [](const uint8_t b) { return b == untouched_byte; });
        };

        reset();
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_name(volume, {}, unmapped_page, this->storage + 0x800, 0x208));
        EXPECT_TRUE(output_untouched(this->storage + 0x800, 0x400));

        reset();
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_name(volume, {}, this->storage + 0x40, output_at_page_end, 0x40));
        EXPECT_TRUE(output_untouched(output_at_page_end, name_bytes_with_length));
        EXPECT_EQ(untouched_information, status_block.read().Information);

        reset();
        EXPECT_EQ(STATUS_SUCCESS, this->query_name(volume, {}, this->storage + 0x40, output_at_page_end, name_bytes_with_length));
        EXPECT_EQ(name_bytes_with_length, status_block.read().Information);
    }

    TEST_F(VolumeDeviceNameTest, UnreadableInputFailsAfterEventResetBeforeAnythingIsWritten)
    {
        struct input_row
        {
            bool unmapped_input{};
            uint64_t input_offset{};
            ULONG input_length{};
            NTSTATUS status{};
        };

        constexpr std::array<input_row, 4> rows{{
            {true, 0, 1, STATUS_ACCESS_VIOLATION},
            {false, 0, 0, STATUS_SUCCESS},
            {false, 0x100, 1, STATUS_SUCCESS},
            {false, 0x100, 0, STATUS_SUCCESS},
        }};

        const auto volume = this->open(u"\\??\\C:");
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status_block{this->emu.memory, this->storage + 0x40};
        const auto unmapped_page = this->page_before_hole + 0x1000;

        for (const auto& row : rows)
        {
            SCOPED_TRACE("input_length=" + std::to_string(row.input_length) + " unmapped=" + std::to_string(row.unmapped_input));

            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial_block{};
            initial_block.Information = untouched_information;
            status_block.write(initial_block);
            this->emu.memory.set_memory(this->storage + 0x800, untouched_byte, 0x400);

            const auto event_handle = this->emu.process.events.store(event{});
            this->emu.process.events.get(event_handle)->set_signaled(true);

            const auto input = row.unmapped_input ? unmapped_page : row.input_offset ? this->storage + row.input_offset : 0;
            EXPECT_EQ(row.status,
                      this->query_name(volume, event_handle, this->storage + 0x40, this->storage + 0x800, 0x208, input, row.input_length));

            const auto succeeded = row.status == STATUS_SUCCESS;
            EXPECT_EQ(succeeded, this->emu.process.events.get(event_handle)->is_signaled());
            EXPECT_EQ(succeeded ? uint64_t{48} : untouched_information, status_block.read().Information);

            USHORT name_length{};
            this->emu.memory.read_memory(this->storage + 0x800, &name_length, sizeof(name_length));
            EXPECT_EQ(succeeded, name_length == 46);
        }
    }

    TEST_F(VolumeDeviceNameTest, CompletionEventIsResetOnSubmissionAndSignalledOnSuccess)
    {
        struct event_row
        {
            ULONG output_length{};
            bool initially_signalled{};
            NTSTATUS status{};
            bool signalled_after{};
        };

        constexpr std::array<event_row, 4> rows{{
            {0x208, false, STATUS_SUCCESS, true},
            {8, false, STATUS_BUFFER_OVERFLOW, true},
            {3, false, STATUS_INVALID_PARAMETER, false},
            {3, true, STATUS_INVALID_PARAMETER, false},
        }};

        const auto volume = this->open(u"\\??\\C:");
        for (const auto& row : rows)
        {
            SCOPED_TRACE("out=" + std::to_string(row.output_length) + " initially_signalled=" + std::to_string(row.initially_signalled));

            const auto event_handle = this->emu.process.events.store(event{});
            this->emu.process.events.get(event_handle)->set_signaled(row.initially_signalled);

            EXPECT_EQ(row.status, this->query_name(volume, event_handle, this->storage + 0x40, this->storage + 0x800, row.output_length));
            EXPECT_EQ(row.signalled_after, this->emu.process.events.get(event_handle)->is_signaled());
        }
    }

    TEST_F(VolumeDeviceNameTest, QueryOnAnAsynchronousHandleCompletesThroughThePort)
    {
        struct port_row
        {
            ULONG notification_flags{};
            ULONG output_length{};
            uint64_t apc_routine{};
            NTSTATUS result{};
            bool packet{};
            NTSTATUS packet_status{};
            uint64_t packet_information{};
            bool rejected{};
        };

        constexpr ULONG skip_completion_port_on_success = 0x1;
        constexpr uint64_t key = 0x3001;
        constexpr uint64_t overlapped = 0x7700;
        constexpr std::array<port_row, 6> rows{{
            {0, 0x208, 0, STATUS_SUCCESS, true, STATUS_SUCCESS, 48, false},
            {0, 0x208, 1, STATUS_SUCCESS, true, STATUS_SUCCESS, 48, false},
            {0, 0x208, 0x1234, STATUS_INVALID_PARAMETER, false, STATUS_SUCCESS, 0, true},
            {0, 8, 0, STATUS_PENDING, true, STATUS_BUFFER_OVERFLOW, 2, false},
            {skip_completion_port_on_success, 0x208, 0, STATUS_SUCCESS, false, STATUS_SUCCESS, 0, false},
            {skip_completion_port_on_success, 8, 0, STATUS_PENDING, true, STATUS_BUFFER_OVERFLOW, 2, false},
        }};

        const auto volume = this->open(u"\\??\\C:", 0);
        const auto port = this->emu.process.io_completions.store(io_completion{});
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> status_block{this->emu.memory, this->storage + 0x40};

        this->emu.memory.write_memory(this->storage + 0x100, port);
        this->emu.memory.write_memory(this->storage + 0x108, key);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetInformationFile(this->context(), volume, status_block, this->storage + 0x100, 16,
                                                                        FileCompletionInformation));

        ULONG applied_flags{};
        for (const auto& row : rows)
        {
            SCOPED_TRACE("flags=" + std::to_string(row.notification_flags) + " out=" + std::to_string(row.output_length));

            if (row.notification_flags != applied_flags)
            {
                this->emu.memory.write_memory(this->storage + 0x100, row.notification_flags);
                ASSERT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtSetInformationFile(this->context(), volume, status_block, this->storage + 0x100, sizeof(ULONG),
                                                                FileIoCompletionNotificationInformation));
                applied_flags = row.notification_flags;
            }

            const auto event_handle = this->emu.process.events.store(event{});
            this->emu.process.events.get(event_handle)->set_signaled(true);
            this->emu.memory.set_memory(this->storage + 0x800, untouched_byte, 0x100);
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> initial_block{};
            initial_block.Information = untouched_information;
            status_block.write(initial_block);

            EXPECT_EQ(row.result, this->query_name(volume, event_handle, this->storage + 0x40, this->storage + 0x800, row.output_length, 0,
                                                   0, row.apc_routine, overlapped));
            EXPECT_TRUE(this->issuing_thread->pending_apcs.empty());

            if (row.rejected)
            {
                EXPECT_TRUE(this->emu.process.events.get(event_handle)->is_signaled());
                EXPECT_EQ(untouched_information, status_block.read().Information);

                std::array<uint8_t, 0x100> output{};
                this->emu.memory.read_memory(this->storage + 0x800, output.data(), output.size());
                EXPECT_TRUE(std::ranges::all_of(output, [](const uint8_t b) { return b == untouched_byte; }));
            }

            io_completion_message message{};
            ASSERT_EQ(row.packet, this->emu.process.io_completions.get(port)->dequeue(message));
            if (row.packet)
            {
                EXPECT_EQ(key, message.key_context);
                EXPECT_EQ(overlapped, message.apc_context);
                EXPECT_EQ(row.packet_status, message.io_status_block.Status);
                EXPECT_EQ(row.packet_information, message.io_status_block.Information);
            }
        }
    }

    TEST_F(VolumeDeviceNameTest, QueryOnAnUnassociatedAsynchronousHandleQueuesTheApc)
    {
        struct apc_row
        {
            uint64_t apc_routine{};
            uint64_t queued_routine{};
        };

        constexpr std::array<apc_row, 3> rows{{
            {.apc_routine = 0x5678, .queued_routine = 0x5678},
            {.apc_routine = 0x5679, .queued_routine = 0x5678},
            {.apc_routine = 1, .queued_routine = 0},
        }};

        const auto volume = this->open(u"\\??\\C:", 0);
        for (const auto& row : rows)
        {
            SCOPED_TRACE("apc_routine=" + std::to_string(row.apc_routine));
            this->issuing_thread->pending_apcs.clear();

            EXPECT_EQ(STATUS_SUCCESS,
                      this->query_name(volume, {}, this->storage + 0x40, this->storage + 0x800, 0x208, 0, 0, row.apc_routine, 0x9A));

            if (row.queued_routine == 0)
            {
                EXPECT_TRUE(this->issuing_thread->pending_apcs.empty());
                continue;
            }

            ASSERT_EQ(1u, this->issuing_thread->pending_apcs.size());
            const auto& apc = this->issuing_thread->pending_apcs.front();
            EXPECT_EQ(row.queued_routine, apc.apc_routine);
            EXPECT_EQ(0x9Au, apc.apc_argument1);
            EXPECT_EQ(this->storage + 0x40, apc.apc_argument2);
            EXPECT_EQ(48u, apc.io_information);
        }
    }
}

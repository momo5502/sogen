#include "emulation_test_utils.hpp"
#include <io_device.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr ULONG ioctl_open_class_key = 0x470863;
        constexpr uint32_t installer_class = 2;
        constexpr uint32_t interface_class = 3;
        constexpr uint32_t open_always = 1;
        constexpr uint32_t open_existing = 2;

        // The request cfgmgr32's InitializeInputRegistryData builds for CM_Open_Class_Key.
        struct registry_input
        {
            uint32_t size{48};
            uint32_t reserved1{};
            uint32_t key_type{interface_class};
            uint32_t reserved2{};
            uint64_t class_name{};
            uint32_t class_name_bytes{};
            uint32_t desired_access{0x8};
            uint32_t disposition{open_existing};
            uint32_t reserved3{};
            uint64_t result_size{16};
        };

        struct handle_result
        {
            uint32_t size;
            NTSTATUS status;
            uint64_t handle;
        };

        constexpr ULONG ioctl_get_device_interface_list = 0x470807;
        constexpr std::array<uint8_t, 16> audio_interface_class{0x04, 0xad, 0x94, 0x69, 0xef, 0x93, 0xd0, 0x11,
                                                                0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96};

        // The request cfgmgr32 builds for CM_Get_Device_Interface_List, here for present KSCATEGORY_AUDIO interfaces.
        struct interface_list_input
        {
            uint32_t size{40};
            uint32_t flags{0x10000};
            std::array<uint8_t, 16> interface_class{audio_interface_class};
            uint64_t device_id{};
            uint32_t device_id_bytes{};
            uint32_t result_size{20};
        };

        struct buffer_result
        {
            uint32_t size;
            NTSTATUS status;
            uint32_t data_bytes;
            uint32_t reserved;
            uint16_t first_char;
        };

        class DeviceApiTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            NTSTATUS open_class_key(registry_input input, const std::u16string_view class_name = {}, const uint64_t output = 0,
                                    uint64_t request = 0)
            {
                if (!class_name.empty())
                {
                    const std::u16string terminated{class_name};
                    this->emu.memory.write_memory(this->storage + 0x100, terminated.c_str(), (terminated.size() + 1) * sizeof(char16_t));
                    input.class_name = this->storage + 0x100;
                    input.class_name_bytes = static_cast<uint32_t>((terminated.size() + 1) * sizeof(char16_t));
                }

                request = request ? request : this->storage;
                this->emu.memory.write_memory(request, &input, sizeof(input));

                this->context.io_control_code = ioctl_open_class_key;
                this->context.io_status_block.set_address(this->storage + 0x300);
                this->context.input_buffer = request;
                this->context.input_buffer_length = sizeof(input);
                this->context.output_buffer = output ? output : this->storage + 0x200;
                this->context.output_buffer_length = sizeof(handle_result);

                io_device_container device{u"DeviceApi\\CMApi", this->emu, {}};
                return device.execute_ioctl(this->emu, this->context);
            }

            handle_result result()
            {
                return this->emu.memory.read_memory<handle_result>(this->storage + 0x200);
            }

            NTSTATUS get_interface_list(const interface_list_input& input, const ULONG output_length, const uint64_t output = 0,
                                        uint64_t request = 0)
            {
                request = request ? request : this->storage;
                this->emu.memory.write_memory(request, &input, sizeof(input));
                this->emu.memory.set_memory(this->storage + 0x200, 0xCC, 0x40);

                this->context.io_control_code = ioctl_get_device_interface_list;
                this->context.io_status_block.set_address(this->storage + 0x300);
                this->context.input_buffer = request;
                this->context.input_buffer_length = sizeof(input);
                this->context.output_buffer = output ? output : this->storage + 0x200;
                this->context.output_buffer_length = output_length;

                io_device_container device{u"DeviceApi\\CMApi", this->emu, {}};
                return device.execute_ioctl(this->emu, this->context);
            }

            buffer_result list_result()
            {
                return this->emu.memory.read_memory<buffer_result>(this->storage + 0x200);
            }

            std::string opened_path(const handle_result& reply)
            {
                const auto* key = this->emu.process.registry_keys.get(handle{.bits = reply.handle});
                if (!key)
                {
                    return {};
                }

                // A key's path has the host's separators; the registry's own are backslashes.
                const auto path = (key->hive.get() / key->path.get()).generic_u8string();
                std::string lowered(path.begin(), path.end());
                std::ranges::transform(lowered, lowered.begin(), [](const char c) { return static_cast<char>(std::tolower(c)); });
                std::ranges::replace(lowered, '/', '\\');
                return lowered;
            }

            windows_emulator emu = create_empty_emulator();
            io_device_context context{this->emu.memory};
            uint64_t storage{};
        };
    }

    TEST_F(DeviceApiTest, InterfaceListSizeQueryReportsTheTerminator)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->get_interface_list({}, 20));

        const auto reply = this->list_result();
        EXPECT_EQ(20u, reply.size);
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, reply.status);
        EXPECT_EQ(2u, reply.data_bytes);
        EXPECT_EQ(20u, this->context.io_status_block.read().Information);
    }

    TEST_F(DeviceApiTest, InterfaceListIsEmpty)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->get_interface_list({}, 22));

        const auto reply = this->list_result();
        EXPECT_EQ(STATUS_SUCCESS, reply.status);
        EXPECT_EQ(2u, reply.data_bytes);
        EXPECT_EQ(0u, reply.first_char);
        EXPECT_EQ(22u, this->context.io_status_block.read().Information);
    }

    TEST_F(DeviceApiTest, InterfaceListRejectsBadRequests)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->get_interface_list({.flags = 1}, 22));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->list_result().status);
        EXPECT_EQ(0u, this->list_result().data_bytes);

        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->get_interface_list({.size = 36}, 22));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->get_interface_list({.result_size = 16}, 22));
    }

    // PiCMReturnBufferResultData writes the reply to the caller's buffer only after a probe of all of it for write,
    // ULONG-aligned, and that probe also faults on a missing buffer.
    TEST_F(DeviceApiTest, InterfaceListOutputBufferThatCannotTakeTheReplyFailsTheRequest)
    {
        const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->get_interface_list({}, 22, read_only));
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint64_t>(read_only));

        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->get_interface_list({}, 22, this->storage + 0x202));
        EXPECT_EQ(0xCCCCCCCCu, this->emu.memory.read_memory<uint32_t>(this->storage + 0x202));

        this->context.output_buffer = 0;
        io_device_container device{u"DeviceApi\\CMApi", this->emu, {}};
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, device.execute_ioctl(this->emu, this->context));
    }

    // The device ID is captured like a class name: its whole declared range is probed, aligned to a character, and it is
    // used up to its first NUL, so a length up to 4 GB is checked without a copy. An ID longer than RtlInitUnicodeStringEx
    // takes is reported in the reply.
    TEST_F(DeviceApiTest, InterfaceListDeviceIdIsCheckedAsTheKernelCopiesIt)
    {
        const std::u16string device_id = u"ROOT\\MEDIA\\0000";
        const auto id_bytes = static_cast<uint32_t>((device_id.size() + 1) * sizeof(char16_t));
        this->emu.memory.write_memory(this->storage + 0x100, device_id.c_str(), id_bytes);

        ASSERT_EQ(STATUS_SUCCESS, this->get_interface_list({.device_id = this->storage + 0x100, .device_id_bytes = id_bytes}, 22));
        EXPECT_EQ(STATUS_SUCCESS, this->list_result().status);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                  this->get_interface_list({.device_id = this->storage + 0x100, .device_id_bytes = 0xFFFFFFFE}, 22));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT,
                  this->get_interface_list({.device_id = this->storage + 0x101, .device_id_bytes = id_bytes}, 22));

        constexpr size_t too_long = 0x7FFF;
        const auto long_id = this->emu.memory.allocate_memory(0x11000, memory_permission::read_write);
        const std::u16string characters(too_long, u'a');
        this->emu.memory.write_memory(long_id, characters.data(), characters.size() * sizeof(char16_t));
        this->emu.memory.write_memory(long_id + (too_long * sizeof(char16_t)), char16_t{});
        ASSERT_EQ(STATUS_SUCCESS,
                  this->get_interface_list(
                      {.device_id = long_id, .device_id_bytes = static_cast<uint32_t>((too_long + 1) * sizeof(char16_t))}, 22));
        EXPECT_EQ(STATUS_NAME_TOO_LONG, this->list_result().status);
        EXPECT_EQ(0u, this->list_result().data_bytes);
    }

    // The interface list request is read like the class key request: one the caller cannot read, or a misaligned one,
    // fails with the probe's status before anything is written.
    TEST_F(DeviceApiTest, InterfaceListRequestThatCannotBeReadFailsTheRequest)
    {
        const auto no_access = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_TRUE(this->emu.memory.protect_memory(no_access, 0x1000, memory_permission::none));

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->get_interface_list({}, 22, 0, no_access));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->get_interface_list({}, 22, 0, this->storage + 0x404));
        EXPECT_EQ(0xCCCCCCCCu, this->list_result().size);
    }

    TEST_F(DeviceApiTest, OpensTheInterfaceClassRoot)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->open_class_key({}));

        const auto reply = this->result();
        EXPECT_EQ(16u, reply.size);
        ASSERT_EQ(STATUS_SUCCESS, reply.status);
        EXPECT_TRUE(this->opened_path(reply).ends_with("\\control\\deviceclasses"));
        EXPECT_EQ(16u, this->context.io_status_block.read().Information);
    }

    TEST_F(DeviceApiTest, OpensAnInstallerClassByGuid)
    {
        constexpr std::u16string_view display_class = u"{4d36e968-e325-11ce-bfc1-08002be10318}";
        ASSERT_EQ(STATUS_SUCCESS, this->open_class_key({.key_type = installer_class}, display_class));

        const auto reply = this->result();
        ASSERT_EQ(STATUS_SUCCESS, reply.status);
        EXPECT_TRUE(this->opened_path(reply).ends_with("\\control\\class\\{4d36e968-e325-11ce-bfc1-08002be10318}"));
    }

    TEST_F(DeviceApiTest, MissingClassDependsOnTheDisposition)
    {
        constexpr std::u16string_view missing_class = u"{00000000-0000-0000-0000-000000000000}";
        ASSERT_EQ(STATUS_SUCCESS, this->open_class_key({}, missing_class));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->result().status);

        ASSERT_EQ(STATUS_SUCCESS, this->open_class_key({.disposition = open_always}, missing_class));
        EXPECT_EQ(STATUS_ACCESS_DENIED, this->result().status);
        EXPECT_EQ(0u, this->result().handle);
    }

    TEST_F(DeviceApiTest, RejectsMalformedRequests)
    {
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->open_class_key({.size = 36}));

        ASSERT_EQ(STATUS_SUCCESS, this->open_class_key({.key_type = 1}));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->result().status);
        EXPECT_EQ(0u, this->result().handle);
    }

    // The class name is copied with all of its declared length, and that copy's probe fails the request when the range
    // cannot be read; a name longer than RtlInitUnicodeStringEx takes is reported in the reply.
    TEST_F(DeviceApiTest, ClassNameIsCheckedAsTheKernelCopiesIt)
    {
        const std::u16string short_name = u"{4d36e968-e325-11ce-bfc1-08002be10318}";
        this->emu.memory.write_memory(this->storage + 0x100, short_name.c_str(), (short_name.size() + 1) * sizeof(char16_t));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->open_class_key({.class_name = this->storage + 0x100, .class_name_bytes = 0xFFFFFFFE}));

        constexpr size_t too_long = 0x7FFF;
        const auto long_name = this->emu.memory.allocate_memory(0x11000, memory_permission::read_write);
        const std::u16string characters(too_long, u'a');
        this->emu.memory.write_memory(long_name, characters.data(), characters.size() * sizeof(char16_t));
        this->emu.memory.write_memory(long_name + (too_long * sizeof(char16_t)), char16_t{});
        const auto keys = this->emu.process.registry_keys.size();
        ASSERT_EQ(STATUS_SUCCESS, this->open_class_key({.key_type = installer_class,
                                                        .class_name = long_name,
                                                        .class_name_bytes = static_cast<uint32_t>((too_long + 1) * sizeof(char16_t))}));
        EXPECT_EQ(STATUS_NAME_TOO_LONG, this->result().status);
        EXPECT_EQ(keys, this->emu.process.registry_keys.size());
    }

    // The request is METHOD_NEITHER: the reply is written to the caller's buffer after a probe of all of it, and a
    // request whose reply cannot be written fails without leaving a key handle behind.
    TEST_F(DeviceApiTest, OutputBufferThatCannotTakeTheReplyFailsTheRequest)
    {
        const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
        const auto keys = this->emu.process.registry_keys.size();
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->open_class_key({}, {}, read_only));
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint64_t>(read_only));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->open_class_key({}, {}, this->storage + 0x202));
        EXPECT_EQ(keys, this->emu.process.registry_keys.size());
    }

    // The request is the caller's memory too: PiCMCaptureRegistryInputData probes it for read, aligned to its widest
    // field, and reads it in place, so a request the caller cannot read or a misaligned one fails before anything is
    // opened or written.
    TEST_F(DeviceApiTest, RequestThatCannotBeReadFailsTheRequest)
    {
        const auto no_access = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_TRUE(this->emu.memory.protect_memory(no_access, 0x1000, memory_permission::none));
        const auto keys = this->emu.process.registry_keys.size();

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->open_class_key({}, {}, 0, no_access));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->open_class_key({}, {}, 0, this->storage + 0x404));
        EXPECT_EQ(0u, this->result().size);
        EXPECT_EQ(keys, this->emu.process.registry_keys.size());
    }

    // A WoW64 request is read from its size: only once that says 36 bytes are the other fields read.
    TEST_F(DeviceApiTest, Wow64RequestIsReadFromItsSize)
    {
        this->emu.process.is_wow64_process = true;

        const auto pages = this->emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_TRUE(this->emu.memory.protect_memory(pages + 0x1000, 0x1000, memory_permission::none));
        const auto request = pages + 0x1000 - sizeof(uint32_t);

        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->open_class_key({.size = 0}, {}, 0, request));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->open_class_key({.size = 36}, {}, 0, request));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->open_class_key({.size = 36}, {}, 0, this->storage + 0x402));
        EXPECT_EQ(0u, this->result().size);
    }
}

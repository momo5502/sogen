#include "emulation_test_utils.hpp"
#include <io_device.hpp>
#include <token.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr ULONG ioctl_get_device_id_list = 0x470803;
        constexpr uint32_t list_by_enumerator = 0x1;
        constexpr uint32_t list_by_service = 0x2;
        constexpr uint32_t list_bus_relations = 0x20;
        constexpr uint32_t list_by_class = 0x80;
        constexpr uint32_t list_present_only = 0x10000;
        constexpr uint32_t list_unfiltered = 0x20000;

        // The request cfgmgr32's InitializeInputDeviceListData builds for CM_Get_Device_ID_List.
        struct device_list_input
        {
            uint32_t size{24};
            uint32_t flags{};
            uint64_t filter{};
            uint32_t filter_bytes{};
            uint32_t result_size{20};
        };

        struct device_list_reply
        {
            NTSTATUS ioctl_status{};
            NTSTATUS status{};
            uint32_t data_bytes{};
            uint64_t information{};
            std::u16string list{};
        };

        constexpr std::u16string_view enum_path = uR"(\Registry\Machine\System\CurrentControlSet\Enum\)";
        constexpr std::u16string_view installer_classes_path = uR"(\Registry\Machine\System\CurrentControlSet\Control\Class\)";
        constexpr std::u16string_view session_id_property = uR"(\Properties\{83da6326-97a6-4088-9453-a1923f573b29}\0006)";
        constexpr std::u16string_view restricted_sd_property = uR"(\Properties\{8c439ff0-9cf7-43cd-961e-9299a4c6c157}\0064)";
        constexpr uint32_t devprop_type_uint32 = 0xFFFF0007;
        constexpr uint32_t devprop_type_security_descriptor = 0xFFFF0013;
        constexpr ACCESS_MASK enumerate_device_access = 0x1;

        // What a device ID list request answers: the filter goes to storage + 0x100, the reply to storage + 0x400.
        device_list_reply query_device_ids(windows_emulator& emu, const uint64_t storage, const uint32_t flags,
                                           const std::optional<std::u16string_view> filter = {}, const ULONG output_length = 0x400,
                                           const uint32_t result_size = 20)
        {
            device_list_input input{.flags = flags, .result_size = result_size};
            if (filter)
            {
                const std::u16string terminated{*filter};
                emu.memory.write_memory(storage + 0x100, terminated.c_str(), (terminated.size() + 1) * sizeof(char16_t));
                input.filter = storage + 0x100;
                input.filter_bytes = static_cast<uint32_t>((terminated.size() + 1) * sizeof(char16_t));
            }

            emu.memory.write_memory(storage, &input, sizeof(input));
            emu.memory.set_memory(storage + 0x400, 0xCC, output_length);

            io_device_context context{emu.memory};
            context.io_control_code = ioctl_get_device_id_list;
            context.io_status_block.set_address(storage + 0x300);
            context.input_buffer = storage;
            context.input_buffer_length = sizeof(input);
            context.output_buffer = storage + 0x400;
            context.output_buffer_length = output_length;

            io_device_container device{u"DeviceApi\\CMApi", emu, {}};
            device_list_reply reply{.ioctl_status = device.execute_ioctl(emu, context)};
            reply.status = emu.memory.read_memory<NTSTATUS>(storage + 0x404);
            reply.data_bytes = emu.memory.read_memory<uint32_t>(storage + 0x408);
            reply.information = context.io_status_block.read().Information;
            if (reply.ioctl_status == STATUS_SUCCESS && reply.status == STATUS_SUCCESS)
            {
                reply.list.resize(reply.data_bytes / sizeof(char16_t));
                emu.memory.read_memory(storage + 0x410, reply.list.data(), reply.data_bytes);
            }

            return reply;
        }

        std::u16string multi_sz(const std::initializer_list<std::u16string_view> strings)
        {
            std::u16string list{};
            for (const auto string : strings)
            {
                list += string;
                list += u'\0';
            }

            list += u'\0';
            return list;
        }

        struct test_device
        {
            std::u16string id;
            std::u16string service{};
            std::u16string class_guid{};
            std::u16string upper_filter{};
            std::optional<uint32_t> session{};
            std::vector<std::byte> restricted_descriptor{};
        };

        void set_string_value(windows_emulator& emu, const std::u16string& key_path, const std::string& name, const uint32_t type,
                              std::u16string value)
        {
            const auto key = emu.registry.create_key(std::filesystem::path{key_path});
            ASSERT_TRUE(key.has_value());
            value.push_back(u'\0');
            if (type == REG_MULTI_SZ)
            {
                value.push_back(u'\0');
            }

            emu.registry.set_value(*key, name, type, std::as_bytes(std::span{value}));
        }

        void set_property(windows_emulator& emu, const std::u16string& key_path, const uint32_t type, const std::span<const std::byte> data)
        {
            const auto key = emu.registry.create_key(std::filesystem::path{key_path});
            ASSERT_TRUE(key.has_value());
            emu.registry.set_value(*key, "", type, data);
        }

        void add_device(windows_emulator& emu, const test_device& device)
        {
            const auto path = std::u16string{enum_path} + device.id;
            ASSERT_TRUE(emu.registry.create_key(std::filesystem::path{path}).has_value());
            if (!device.service.empty())
            {
                set_string_value(emu, path, "Service", REG_SZ, device.service);
            }

            if (!device.class_guid.empty())
            {
                set_string_value(emu, path, "ClassGUID", REG_SZ, device.class_guid);
            }

            if (!device.upper_filter.empty())
            {
                set_string_value(emu, path, "UpperFilters", REG_MULTI_SZ, device.upper_filter);
            }

            if (device.session)
            {
                set_property(emu, path + std::u16string{session_id_property}, devprop_type_uint32,
                             std::as_bytes(std::span{&*device.session, 1}));
            }

            if (!device.restricted_descriptor.empty())
            {
                set_property(emu, path + std::u16string{restricted_sd_property}, devprop_type_security_descriptor,
                             device.restricted_descriptor);
            }
        }

        // A self-relative security descriptor whose DACL holds one ACE.
        std::vector<std::byte> descriptor_with_ace(const uint8_t type, const uint8_t flags, const ACCESS_MASK mask,
                                                   const std::span<const uint8_t> sid)
        {
            constexpr auto sid_offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
            const auto ace_size = static_cast<WORD>(sid_offset + sid.size());

            SECURITY_DESCRIPTOR_RELATIVE header{};
            header.Revision = SECURITY_DESCRIPTOR_REVISION;
            header.Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
            header.Dacl = sizeof(header);

            ACL acl{};
            acl.AclRevision = ACL_REVISION;
            acl.AclSize = static_cast<WORD>(sizeof(acl) + ace_size);
            acl.AceCount = 1;

            ACCESS_ALLOWED_ACE ace{};
            ace.Header.AceType = type;
            ace.Header.AceFlags = flags;
            ace.Header.AceSize = ace_size;
            ace.Mask = mask;

            std::vector<std::byte> descriptor(sizeof(header) + acl.AclSize);
            const auto ace_offset = sizeof(header) + sizeof(acl);
            memcpy(descriptor.data(), &header, sizeof(header));
            memcpy(descriptor.data() + sizeof(header), &acl, sizeof(acl));
            memcpy(descriptor.data() + ace_offset, &ace, sid_offset);
            memcpy(descriptor.data() + ace_offset + sid_offset, sid.data(), sid.size());
            return descriptor;
        }

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

            device_list_reply device_ids(const uint32_t flags, const std::optional<std::u16string_view> filter = {},
                                         const ULONG output_length = 0x400, const uint32_t result_size = 20)
            {
                return query_device_ids(this->emu, this->storage, flags, filter, output_length, result_size);
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

    // An interface key "##?#<device path>" under the class with reference keys "#" and "#Ref", as the PnP manager writes
    // them: listing all interfaces names both links, a device ID filters on the key's DeviceInstance value, and a
    // present-only query names neither, as the emulator enables no interfaces.
    TEST_F(DeviceApiTest, InterfaceListNamesRegisteredInterfaces)
    {
        constexpr std::array<uint8_t, 16> test_class{0x4c, 0x3d, 0x2e, 0x1f, 0x6a, 0x5b, 0x78, 0x49,
                                                     0x86, 0x95, 0xa4, 0xb3, 0xc2, 0xd1, 0xe0, 0xf0};
        const std::u16string interface_name = u"##?#ROOT#SOGEN#0000#{1F2E3D4C-5B6A-4978-8695-A4B3C2D1E0F0}";
        const std::u16string instance = uR"(ROOT\SOGEN\0000)";
        const std::filesystem::path class_path{
            uR"(\Registry\Machine\System\CurrentControlSet\Control\DeviceClasses\{1F2E3D4C-5B6A-4978-8695-A4B3C2D1E0F0})"};

        const auto interface_key = this->emu.registry.create_key(class_path / interface_name);
        ASSERT_TRUE(interface_key.has_value());
        ASSERT_TRUE(this->emu.registry.create_key(class_path / interface_name / u"#").has_value());
        ASSERT_TRUE(this->emu.registry.create_key(class_path / interface_name / u"#Ref").has_value());
        this->emu.registry.set_value(*interface_key, "DeviceInstance", REG_SZ,
                                     std::as_bytes(std::span{instance.c_str(), instance.size() + 1}));

        const auto query = [&](const uint32_t flags, const std::u16string& device_id) {
            interface_list_input input{.flags = flags, .interface_class = test_class};
            if (!device_id.empty())
            {
                this->emu.memory.write_memory(this->storage + 0x100, device_id.c_str(), (device_id.size() + 1) * sizeof(char16_t));
                input.device_id = this->storage + 0x100;
                input.device_id_bytes = static_cast<uint32_t>((device_id.size() + 1) * sizeof(char16_t));
            }

            this->emu.memory.write_memory(this->storage, &input, sizeof(input));
            this->context.io_control_code = ioctl_get_device_interface_list;
            this->context.io_status_block.set_address(this->storage + 0x300);
            this->context.input_buffer = this->storage;
            this->context.input_buffer_length = sizeof(input);
            this->context.output_buffer = this->storage + 0x400;
            this->context.output_buffer_length = 0x400;

            io_device_container device{u"DeviceApi\\CMApi", this->emu, {}};
            EXPECT_EQ(STATUS_SUCCESS, device.execute_ioctl(this->emu, this->context));

            const auto reply = this->emu.memory.read_memory<buffer_result>(this->storage + 0x400);
            EXPECT_EQ(STATUS_SUCCESS, reply.status);
            std::u16string list(reply.data_bytes / sizeof(char16_t), u'\0');
            this->emu.memory.read_memory(this->storage + 0x400 + 16, list.data(), reply.data_bytes);
            return list;
        };

        const auto links = [](const std::u16string& list) {
            std::vector<std::u16string> names;
            for (size_t start = 0; start < list.size() && list[start] != u'\0'; start = list.find(u'\0', start) + 1)
            {
                names.emplace_back(list.substr(start, list.find(u'\0', start) - start));
            }

            std::ranges::sort(names);
            return names;
        };

        const std::u16string link = uR"(\\?\ROOT#SOGEN#0000#{1F2E3D4C-5B6A-4978-8695-A4B3C2D1E0F0})";
        const std::vector<std::u16string> both{link, link + uR"(\Ref)"};
        const std::u16string empty(1, u'\0');

        const auto all = query(0, {});
        EXPECT_EQ(both, links(all));
        EXPECT_TRUE(all.ends_with(std::u16string(2, u'\0')));
        EXPECT_EQ(both, links(query(0, instance)));
        EXPECT_EQ(empty, query(0, uR"(ROOT\OTHER\0000)"));
        EXPECT_EQ(empty, query(0x10000, {}));
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

    // The class name is copied with all of its declared length, aligned to a character, and that copy's probe fails the
    // request when the range cannot be read or is misaligned; a name longer than RtlInitUnicodeStringEx takes is reported
    // in the reply.
    TEST_F(DeviceApiTest, ClassNameIsCheckedAsTheKernelCopiesIt)
    {
        const std::u16string short_name = u"{4d36e968-e325-11ce-bfc1-08002be10318}";
        const auto name_bytes = static_cast<uint32_t>((short_name.size() + 1) * sizeof(char16_t));
        this->emu.memory.write_memory(this->storage + 0x100, short_name.c_str(), name_bytes);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->open_class_key({.class_name = this->storage + 0x100, .class_name_bytes = 0xFFFFFFFE}));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT,
                  this->open_class_key({.class_name = this->storage + 0x101, .class_name_bytes = name_bytes}));

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

    // The list names the Enum keys that are valid device instance IDs, in the order of the PnP manager's device cache
    // (PiDmCompareObjects: the ID's case-insensitive X65599 hash, then the ID), which is not alphabetical. An enumerator
    // filter matches the IDs it prefixes, ignoring case, and a size query reads the list's size from a
    // STATUS_BUFFER_TOO_SMALL reply.
    TEST_F(DeviceApiTest, DeviceIdListNamesRegisteredDevicesInDeviceCacheOrder)
    {
        for (const auto* device : {u"DISPLAY", u"MONITOR", u"AUDIO", u"KEYBOARD", u"MOUSE", u"GAMEPAD"})
        {
            add_device(this->emu, {.id = std::u16string{uR"(SOGENTEST\)"} + device + uR"(\0000)"});
        }

        add_device(this->emu, {.id = uR"(SOGENTEST\NOT,VALID\0000)"});

        const auto all = this->device_ids(list_by_enumerator, u"sogentest");
        ASSERT_EQ(STATUS_SUCCESS, all.ioctl_status);
        EXPECT_EQ(STATUS_SUCCESS, all.status);
        EXPECT_EQ(multi_sz({uR"(SOGENTEST\AUDIO\0000)", uR"(SOGENTEST\GAMEPAD\0000)", uR"(SOGENTEST\MONITOR\0000)",
                            uR"(SOGENTEST\MOUSE\0000)", uR"(SOGENTEST\KEYBOARD\0000)", uR"(SOGENTEST\DISPLAY\0000)"}),
                  all.list);
        EXPECT_EQ(20u + all.data_bytes, all.information);

        EXPECT_EQ(multi_sz({uR"(SOGENTEST\MOUSE\0000)"}), this->device_ids(list_by_enumerator, uR"(SOGENTEST\MOUSE)").list);

        const auto size_query = this->device_ids(list_by_enumerator, u"SOGENTEST", 20);
        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, size_query.status);
        EXPECT_EQ(all.data_bytes, size_query.data_bytes);
    }

    // A service filter lists the devices the service drives or filters: by their Service value, their UpperFilters or
    // LowerFilters, or their class's. A class filter lists the devices whose ClassGUID names it, ignoring case, and an
    // empty one names no class.
    TEST_F(DeviceApiTest, DeviceIdListFiltersByServiceAndClass)
    {
        constexpr std::u16string_view test_class = u"{5e0f1a2b-3c4d-4e5f-8a9b-0c1d2e3f4a5b}";
        constexpr std::u16string_view test_class_upper = u"{5E0F1A2B-3C4D-4E5F-8A9B-0C1D2E3F4A5B}";
        set_string_value(this->emu, std::u16string{installer_classes_path} + std::u16string{test_class}, "LowerFilters", REG_MULTI_SZ,
                         u"SogenClassFilter");
        add_device(this->emu, {.id = uR"(SOGENTEST\AUDIO\0000)", .service = u"SogenAudio"});
        add_device(this->emu, {.id = uR"(SOGENTEST\MOUSE\0000)",
                               .service = u"SogenMouse",
                               .class_guid = std::u16string{test_class},
                               .upper_filter = u"SogenDeviceFilter"});
        add_device(this->emu,
                   {.id = uR"(SOGENTEST\KEYBOARD\0000)", .service = u"SogenKeyboard", .class_guid = std::u16string{test_class_upper}});

        const auto class_members = multi_sz({uR"(SOGENTEST\MOUSE\0000)", uR"(SOGENTEST\KEYBOARD\0000)"});
        EXPECT_EQ(multi_sz({uR"(SOGENTEST\AUDIO\0000)"}), this->device_ids(list_by_service, u"sogenaudio").list);
        EXPECT_EQ(multi_sz({uR"(SOGENTEST\MOUSE\0000)"}), this->device_ids(list_by_service, u"SogenDeviceFilter").list);
        EXPECT_EQ(class_members, this->device_ids(list_by_service, u"SogenClassFilter").list);
        EXPECT_EQ(class_members, this->device_ids(list_by_class, test_class_upper).list);
        EXPECT_EQ(multi_sz({}), this->device_ids(list_by_class, u"").list);
    }

    // PiPnpRtlApplyMandatoryDeviceFilters leaves out a device stored with another session's DEVPKEY_Device_SessionId, and
    // one whose DEVPKEY_Device_RestrictedSD does not let the caller read it; an inherit-only ACE does not apply to the
    // device. The list without those filters needs PnP access the caller only has elevated.
    TEST_F(DeviceApiTest, DeviceIdListLeavesOutDevicesTheCallerMayNotSee)
    {
        // S-1-5-21-1-2-3-1001
        this->emu.process.sid = {0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x15, 0x00, 0x00, 0x00, 0x01, 0x00,
                                 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xE9, 0x03, 0x00, 0x00};
        const auto& user_sid = this->emu.process.sid;

        add_device(this->emu, {.id = uR"(SOGENTEST\AUDIO\0000)", .session = token_session_id + 1});
        add_device(this->emu, {.id = uR"(SOGENTEST\GAMEPAD\0000)", .session = token_session_id});
        add_device(this->emu,
                   {.id = uR"(SOGENTEST\MONITOR\0000)",
                    .restricted_descriptor = descriptor_with_ace(ACCESS_ALLOWED_ACE_TYPE, 0, enumerate_device_access, administrators_sid)});
        add_device(this->emu,
                   {.id = uR"(SOGENTEST\MOUSE\0000)",
                    .restricted_descriptor = descriptor_with_ace(ACCESS_ALLOWED_ACE_TYPE, 0, enumerate_device_access, user_sid)});
        add_device(this->emu, {.id = uR"(SOGENTEST\KEYBOARD\0000)",
                               .restricted_descriptor =
                                   descriptor_with_ace(ACCESS_ALLOWED_ACE_TYPE, INHERIT_ONLY_ACE, enumerate_device_access, user_sid)});

        EXPECT_EQ(multi_sz({uR"(SOGENTEST\GAMEPAD\0000)", uR"(SOGENTEST\MOUSE\0000)"}),
                  this->device_ids(list_by_enumerator, u"SOGENTEST").list);

        const auto unfiltered = this->device_ids(list_by_enumerator | list_unfiltered, u"SOGENTEST");
        EXPECT_EQ(STATUS_ACCESS_DENIED, unfiltered.status);
        EXPECT_EQ(0u, unfiltered.data_bytes);
    }

    // An elevated caller's token holds Administrators, which a restricted descriptor can grant, and the PnP access the list
    // without the mandatory filters needs.
    TEST(ElevatedDeviceApiTest, DeviceIdListWithoutTheMandatoryFiltersNamesEveryDevice)
    {
        emulator_interfaces interfaces{};
        interfaces.socket_factory = network::create_static_socket_factory();
        interfaces.dns_lookup = create_sample_dns_lookup();
        auto emu = create_emulator({.use_relative_time = true, .elevated = true}, {}, std::move(interfaces));
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        add_device(emu, {.id = uR"(SOGENTEST\AUDIO\0000)", .session = token_session_id + 1});
        add_device(emu,
                   {.id = uR"(SOGENTEST\MONITOR\0000)",
                    .restricted_descriptor = descriptor_with_ace(ACCESS_ALLOWED_ACE_TYPE, 0, enumerate_device_access, administrators_sid)});

        EXPECT_EQ(multi_sz({uR"(SOGENTEST\MONITOR\0000)"}), query_device_ids(emu, storage, list_by_enumerator, u"SOGENTEST").list);
        EXPECT_EQ(multi_sz({uR"(SOGENTEST\AUDIO\0000)", uR"(SOGENTEST\MONITOR\0000)"}),
                  query_device_ids(emu, storage, list_by_enumerator | list_unfiltered, u"SOGENTEST").list);
    }

    // An empty list is a lone terminator, so a reply under 22 bytes has no room for it. The emulator starts no devices:
    // present-only lists are empty and relations, which devnodes hold, fail. A request that fails once its flags are read
    // reports its reply's room for the list, rounded down to a character, as the list's size.
    TEST_F(DeviceApiTest, DeviceIdListEmptyListsAndFailedRequests)
    {
        add_device(this->emu, {.id = uR"(SOGENTEST\AUDIO\0000)"});

        for (const ULONG output_length : {20u, 21u})
        {
            const auto reply = this->device_ids(list_by_enumerator, u"SOGENNONE", output_length);
            EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, reply.status);
            EXPECT_EQ(2u, reply.data_bytes);
            EXPECT_EQ(20u, reply.information);
        }

        const auto empty = this->device_ids(list_by_enumerator, u"SOGENNONE", 22);
        EXPECT_EQ(STATUS_SUCCESS, empty.status);
        EXPECT_EQ(multi_sz({}), empty.list);
        EXPECT_EQ(22u, empty.information);

        EXPECT_EQ(multi_sz({}), this->device_ids(list_by_enumerator | list_present_only, u"SOGENTEST").list);

        using failed_reply = std::pair<NTSTATUS, uint32_t>;
        const auto failure = [&](const uint32_t flags, const std::optional<std::u16string_view> filter) {
            const auto reply = this->device_ids(flags, filter, 0x101);
            return failed_reply{reply.status, reply.data_bytes};
        };

        // The 0x101-byte reply has 237 bytes of room after its header, 236 of them whole characters.
        const auto with_room = [](const NTSTATUS status) { return failed_reply{status, 236}; };
        const std::u16string long_enumerator(205, u'E');
        EXPECT_EQ(with_room(STATUS_INVALID_PARAMETER), failure(0x3, std::nullopt));
        EXPECT_EQ(with_room(STATUS_OBJECT_NAME_NOT_FOUND), failure(list_bus_relations, uR"(SOGENTEST\AUDIO\0000)"));
        EXPECT_EQ(with_room(STATUS_INVALID_PARAMETER), failure(list_bus_relations, std::nullopt));
        EXPECT_EQ(with_room(STATUS_INVALID_PARAMETER), failure(list_by_enumerator, std::nullopt));
        EXPECT_EQ(with_room(STATUS_INVALID_PARAMETER), failure(list_by_enumerator, uR"(SOGENTEST\AUDIO\0000)"));
        EXPECT_EQ(with_room(STATUS_BUFFER_OVERFLOW), failure(list_by_enumerator, long_enumerator));

        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->device_ids(list_by_enumerator, u"SOGENTEST", 0x400, 16).ioctl_status);
    }
}

#include "rpc_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryWnfStateData(const syscall_context&, emulator_object<uint64_t> state_name, emulator_pointer type_id,
                                        emulator_pointer explicit_scope, emulator_object<ULONG> change_stamp, emulator_pointer buffer,
                                        emulator_object<ULONG> buffer_size);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 16> svcctl_interface{0x81, 0xbb, 0x7a, 0x36, 0x44, 0x98, 0xf1, 0x35,
                                                           0xad, 0x32, 0x98, 0xf0, 0x38, 0x00, 0x10, 0x03};
        constexpr uint32_t open_service_procedure = 16;
        constexpr uint32_t query_service_config_procedure = 17;
        constexpr uint32_t query_notification_name_procedure = 55;
        constexpr size_t reply_header_size = 0x18;
        constexpr std::u16string_view test_service = u"SogenTestService";
        constexpr uint32_t service_running = 4;

        using context_handle = std::array<uint8_t, 20>;

        struct service_config_reply
        {
            uint32_t service_type{};
            uint32_t start_type{};
            uint32_t error_control{};
            std::array<uint32_t, 5> string_referents{};
            std::vector<std::u16string> strings{};
            uint32_t bytes_needed{};
            uint32_t status{};
        };

        class ServiceControlTest : public RpcProtocolTest
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu =
                    std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                       emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root()},
                                                       emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x20000, memory_permission::read_write);
                this->wnf_storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->bind(u"\\RPC Control\\ntsvcs", svcctl_interface, utils::aligned_binary_writer::pointer_size_32);
            }

            void create_test_service()
            {
                const auto key = this->emu->registry.create_key(
                    std::filesystem::path{uR"(\Registry\Machine\System\CurrentControlSet\Services)"} / test_service);
                ASSERT_TRUE(key.has_value());
                this->set_dword(*key, "Type", 0x20);
                this->set_dword(*key, "Start", 2);
                this->set_dword(*key, "ErrorControl", 1);
                this->set_strings(*key, "DependOnService", REG_MULTI_SZ, {u"RpcSs", u"Tcpip"});
                this->set_strings(*key, "DependOnGroup", REG_MULTI_SZ, {u"TDI"});
                this->set_strings(*key, "ObjectName", REG_SZ, {u"NT Authority\\NetworkService"});
            }

            context_handle open_service(const std::u16string_view name, uint32_t& status)
            {
                std::vector<uint8_t> request{};
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(context_handle{});
                writer.write_ndr_u16string(name);
                writer.write<uint32_t>(0x5);
                const auto reply = this->call(open_service_procedure, request);
                EXPECT_EQ(STATUS_SUCCESS, reply.status);

                context_handle handle{};
                if (!reply.payload || reply.payload->size() < reply_header_size + handle.size() + sizeof(uint32_t))
                {
                    ADD_FAILURE() << "Truncated OpenServiceW reply";
                    return handle;
                }

                std::memcpy(handle.data(), reply.payload->data() + reply_header_size, handle.size());
                status = read<uint32_t>(*reply.payload, reply_header_size + handle.size());
                return handle;
            }

            context_handle open_test_service()
            {
                uint32_t status{~0u};
                const auto handle = this->open_service(test_service, status);
                EXPECT_EQ(0u, status);
                return handle;
            }

            service_config_reply query_service_config(const context_handle& service, const uint32_t buffer_size)
            {
                std::vector<uint8_t> request{};
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(service);
                writer.write<uint32_t>(buffer_size);
                const auto reply = this->call(query_service_config_procedure, request);
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                if (!reply.payload)
                {
                    return {};
                }

                const auto& payload = *reply.payload;
                service_config_reply config{
                    .service_type = read<uint32_t>(payload, reply_header_size),
                    .start_type = read<uint32_t>(payload, reply_header_size + 4),
                    .error_control = read<uint32_t>(payload, reply_header_size + 8),
                    .string_referents =
                        {
                            read<uint32_t>(payload, reply_header_size + 12),
                            read<uint32_t>(payload, reply_header_size + 16),
                            read<uint32_t>(payload, reply_header_size + 24),
                            read<uint32_t>(payload, reply_header_size + 28),
                            read<uint32_t>(payload, reply_header_size + 32),
                        },
                };

                auto offset = reply_header_size + 36;
                for (const auto referent : config.string_referents)
                {
                    if (referent != 0)
                    {
                        config.strings.push_back(read_ndr_string(payload, offset));
                    }
                }

                config.bytes_needed = read<uint32_t>(payload, offset);
                config.status = read<uint32_t>(payload, offset + 4);
                EXPECT_EQ(offset + 8, payload.size());
                return config;
            }

            std::vector<uint8_t> query_notification_name(const context_handle& service, const uint32_t info_level)
            {
                std::vector<uint8_t> request{};
                utils::aligned_binary_writer writer(request, this->pointer_size);
                writer.write(service);
                writer.write<uint32_t>(info_level);
                const auto reply = this->call(query_notification_name_procedure, request);
                EXPECT_EQ(STATUS_SUCCESS, reply.status);
                return reply.payload.value_or(std::vector<uint8_t>{});
            }

            NTSTATUS query_wnf_state(const uint64_t state_name, ULONG& change_stamp, ULONG& size, uint32_t& data)
            {
                const auto base = this->wnf_storage;
                const emulator_object<uint64_t> name{this->emu->memory, base};
                const emulator_object<ULONG> stamp{this->emu->memory, base + 0x10};
                const emulator_object<ULONG> buffer_size{this->emu->memory, base + 0x20};
                const emulator_object<uint32_t> buffer{this->emu->memory, base + 0x100};
                name.write(state_name);
                buffer_size.write(0xF00); // the rest of the page
                buffer.write(0);

                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                const auto status = syscalls::handle_NtQueryWnfStateData(context, name, 0, 0, stamp, buffer.value(), buffer_size);
                change_stamp = stamp.read();
                size = buffer_size.read();
                data = buffer.read();
                return status;
            }

            uint64_t wnf_storage{};

          private:
            void set_dword(const registry_key& key, const std::string name, const uint32_t value) const
            {
                const auto* bytes = reinterpret_cast<const std::byte*>(&value);
                this->emu->registry.set_value(key, name, REG_DWORD, std::span(bytes, sizeof(value)));
            }

            void set_strings(const registry_key& key, const std::string name, const uint32_t type,
                             const std::initializer_list<std::u16string_view> values) const
            {
                std::u16string data{};
                for (const auto value : values)
                {
                    data += value;
                    data.push_back(u'\0');
                }

                if (type == REG_MULTI_SZ)
                {
                    data.push_back(u'\0');
                }

                const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
                this->emu->registry.set_value(key, name, type, std::span(bytes, data.size() * sizeof(char16_t)));
            }

            std::u16string read_ndr_string(const std::vector<uint8_t>& payload, size_t& offset) const
            {
                offset = (offset + 3) & ~size_t{3};
                const auto count = read<uint32_t>(payload, offset + 8);
                offset += 12;
                if (count == 0 || offset + count * sizeof(char16_t) > payload.size())
                {
                    ADD_FAILURE() << "Malformed NDR string at " << offset;
                    return {};
                }

                std::u16string value(count - 1, u'\0');
                std::memcpy(value.data(), payload.data() + offset, value.size() * sizeof(char16_t));
                offset += count * sizeof(char16_t);
                offset = (offset + 3) & ~size_t{3};
                return value;
            }
        };
    }

    TEST_F(ServiceControlTest, QueryServiceConfigReportsTheServiceKey)
    {
        this->create_test_service();
        const auto config = this->query_service_config(this->open_test_service(), 0x2000);

        EXPECT_EQ(0u, config.status);
        EXPECT_EQ(0x20u, config.service_type);
        EXPECT_EQ(2u, config.start_type);
        EXPECT_EQ(1u, config.error_control);
        ASSERT_EQ(5u, config.strings.size());
        EXPECT_EQ(u"", config.strings[1]);
        EXPECT_EQ(u"RpcSs/Tcpip/+TDI/", config.strings[2]);
        EXPECT_EQ(u"NT Authority\\NetworkService", config.strings[3]);
        EXPECT_EQ(test_service, config.strings[4]);
    }

    TEST_F(ServiceControlTest, ShortBufferReportsTheNeededSizeWithoutStrings)
    {
        this->create_test_service();
        const auto service = this->open_test_service();
        const auto needed = this->query_service_config(service, 0x2000).bytes_needed;
        ASSERT_GT(needed, 0u);

        const auto truncated = this->query_service_config(service, needed - 1);
        EXPECT_EQ(122u, truncated.status);
        EXPECT_EQ(needed, truncated.bytes_needed);
        EXPECT_EQ(2u, truncated.start_type);
        EXPECT_EQ((std::array<uint32_t, 5>{}), truncated.string_referents);

        const auto exact = this->query_service_config(service, needed);
        EXPECT_EQ(0u, exact.status);
        EXPECT_EQ(5u, exact.strings.size());
    }

    TEST_F(ServiceControlTest, OpenServiceRejectsUnknownServices)
    {
        uint32_t status{};
        const auto handle = this->open_service(u"SogenServiceThatDoesNotExist", status);
        EXPECT_EQ(1060u, status);
        EXPECT_EQ(context_handle{}, handle);
    }

    TEST_F(ServiceControlTest, StatusNotificationNameReadsAsRunning)
    {
        this->create_test_service();
        const auto service = this->open_test_service();
        const auto reply = this->query_notification_name(service, 3);

        EXPECT_EQ(3u, read<uint32_t>(reply, reply_header_size));
        EXPECT_NE(0u, read<uint32_t>(reply, reply_header_size + 4));
        const auto state_name = read<uint64_t>(reply, reply_header_size + 8);
        EXPECT_EQ(0u, read<uint32_t>(reply, reply_header_size + 16));
        EXPECT_EQ(reply_header_size + 20, reply.size());
        EXPECT_EQ(state_name, read<uint64_t>(this->query_notification_name(service, 3), reply_header_size + 8));

        ULONG change_stamp{};
        ULONG size{};
        uint32_t state{};
        ASSERT_EQ(STATUS_SUCCESS, this->query_wnf_state(state_name, change_stamp, size, state));
        EXPECT_EQ(1u, change_stamp);
        EXPECT_EQ(sizeof(uint32_t), size);
        EXPECT_EQ(1u << (service_running - 1), state);
    }

    TEST_F(ServiceControlTest, OtherNotificationLevelsAreDenied)
    {
        this->create_test_service();
        const auto reply = this->query_notification_name(this->open_test_service(), 2);

        EXPECT_EQ(2u, read<uint32_t>(reply, reply_header_size));
        EXPECT_EQ(0u, read<uint32_t>(reply, reply_header_size + 4));
        EXPECT_EQ(5u, read<uint32_t>(reply, reply_header_size + 8));
        EXPECT_EQ(reply_header_size + 12, reply.size());
    }
}

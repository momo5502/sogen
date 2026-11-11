#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtOpenFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                               emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                               ULONG open_options);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtDuplicateObject(const syscall_context& c, handle source_process_handle, handle source_handle,
                                      handle target_process_handle, emulator_object<handle> target_handle, ACCESS_MASK desired_access,
                                      ULONG handle_attributes, ULONG options);
}

namespace sogen::test
{
    namespace
    {
        constexpr ULONG echo_ioctl = 0x222004;
        std::vector<int> destruction_order;

        struct echo_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& emu, const io_device_context& context) override
            {
                if (context.io_control_code != echo_ioctl)
                {
                    return STATUS_NOT_SUPPORTED;
                }
                if (context.input_buffer_length != sizeof(uint32_t) || context.output_buffer_length < sizeof(uint32_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                const auto value = emu.memory.read_memory<uint32_t>(context.input_buffer);
                emu.memory.write_memory(context.output_buffer, value);
                context.io_status_block.access([](auto& status) { status.Information = sizeof(uint32_t); });
                return STATUS_SUCCESS;
            }
        };

        std::unique_ptr<io_device> create_echo(const device_creation_context&)
        {
            return std::make_unique<echo_device>();
        }

        struct provider_state
        {
            uint32_t reply{};
            bool allowed{true};
        };

        struct configured_echo_device : echo_device
        {
            explicit configured_echo_device(std::shared_ptr<provider_state> state)
                : state_(std::move(state))
            {
            }

            NTSTATUS io_control(windows_emulator& emu, const io_device_context& request) override
            {
                const auto status = echo_device::io_control(emu, request);
                if (status == STATUS_SUCCESS)
                {
                    emu.memory.write_memory(request.output_buffer, this->state_->reply);
                }
                return status;
            }

          private:
            std::shared_ptr<provider_state> state_;
        };

        std::unique_ptr<io_device> create_configured_echo(const device_creation_context& context)
        {
            auto state = std::static_pointer_cast<provider_state>(context.provider);
            if (!state)
            {
                throw std::runtime_error("Missing provider state");
            }
            return std::make_unique<configured_echo_device>(std::move(state));
        }

        NTSTATUS validate_configured_echo(const device_creation_context& context)
        {
            const auto state = std::static_pointer_cast<provider_state>(context.provider);
            return state && state->allowed ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        }

        struct tracked_device : echo_device
        {
            ~tracked_device() override
            {
                destruction_order.push_back(1);
            }
        };

        std::unique_ptr<io_device> create_tracked(const device_creation_context&)
        {
            return std::make_unique<tracked_device>();
        }

        std::unique_ptr<io_device> create_nothing(const device_creation_context&)
        {
            return {};
        }

        NTSTATUS reject_32_bit(const device_creation_context& context)
        {
            return context.is_32_bit ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
        }

        windows_emulator make_provider_emulator(std::vector<device_registration> registrations = {})
        {
            emulator_interfaces interfaces{};
            interfaces.ui = std::make_unique<null_ui_backend>();
            interfaces.audio = std::make_unique<null_audio_backend>();
            interfaces.console = std::make_unique<null_console_backend>();
            interfaces.devices = std::move(registrations);
            return windows_emulator{
                create_x86_64_emulator_from_environment(),
                emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                {},
                std::move(interfaces)};
        }

        syscall_context context(windows_emulator& emu)
        {
            return {.win_emu = emu, .emu = emu.vcpu(0).cpu, .vcpu = emu.vcpu(0), .proc = emu.process};
        }

        struct open_result
        {
            NTSTATUS status{};
            handle file{};
        };

        open_result open(windows_emulator& emu, const std::u16string_view path)
        {
            const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            emulator_allocator allocator{emu.memory, storage, 0x1000};
            const auto name = allocator.make_unicode_string(path);
            const auto attributes = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
            OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> value{};
            value.Length = sizeof(value);
            value.ObjectName = name.value();
            attributes.write(value);
            const auto output = allocator.reserve<handle>();
            output.write({.bits = 0xDEADBEEF});
            const auto status = allocator.reserve<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>();
            const auto result = syscalls::handle_NtOpenFile(context(emu), output, GENERIC_READ | GENERIC_WRITE, attributes, status, 0, 0);
            return {result, output.read()};
        }

        void expect_echo(windows_emulator& emu, const handle file, const uint32_t expected = 0xFEDCBA98)
        {
            auto* device = emu.process.devices.get(file);
            ASSERT_NE(nullptr, device);
            const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            constexpr uint32_t payload = 0xFEDCBA98;
            emu.memory.write_memory(storage, payload);
            emu.memory.write_memory<uint32_t>(storage + 8, 0);
            io_device_context request{emu.memory};
            request.io_control_code = echo_ioctl;
            request.input_buffer = storage;
            request.input_buffer_length = sizeof(payload);
            request.output_buffer = storage + 8;
            request.output_buffer_length = sizeof(payload);
            request.io_status_block = {emu.memory, storage + 16};
            const auto event = emu.process.events.store(sogen::event{});
            request.event = event;
            EXPECT_EQ(STATUS_SUCCESS, device->execute_ioctl(emu, request));
            EXPECT_EQ(expected, emu.memory.read_memory<uint32_t>(storage + 8));
            EXPECT_EQ(STATUS_SUCCESS, request.io_status_block.read().Status);
            EXPECT_EQ(sizeof(payload), request.io_status_block.read().Information);
            EXPECT_TRUE(emu.process.events.get(event)->signaled);
        }
    }

    TEST(DeviceProviderTest, RegisteredDeviceOpensOnlyInItsOwningEmulator)
    {
        auto first = make_provider_emulator({{.name = u"Echo", .factory = create_echo}});
        auto second = make_provider_emulator();
        const auto supported = open(first, u"\\Device\\Echo");
        ASSERT_EQ(STATUS_SUCCESS, supported.status);
        expect_echo(first, supported.file);
        const auto unsupported = open(second, u"\\Device\\Echo");
        EXPECT_EQ(STATUS_NO_SUCH_DEVICE, unsupported.status);
        EXPECT_EQ(0xDEADBEEFu, unsupported.file.bits);
    }

    TEST(DeviceProviderTest, FactoryAndValidatorUseTheirOwnEmulatorsProviderState)
    {
        auto first_state = std::make_shared<provider_state>(provider_state{.reply = 0x13579BDF});
        auto second_state = std::make_shared<provider_state>(provider_state{.reply = 0x2468ACE0, .allowed = false});
        auto first = make_provider_emulator({{.name = u"Configured",
                                              .factory = create_configured_echo,
                                              .validate_open = validate_configured_echo,
                                              .lifetime = first_state}});
        auto second = make_provider_emulator({{.name = u"Configured",
                                               .factory = create_configured_echo,
                                               .validate_open = validate_configured_echo,
                                               .lifetime = second_state}});
        const auto allowed = open(first, u"\\Device\\Configured");
        ASSERT_EQ(STATUS_SUCCESS, allowed.status);
        expect_echo(first, allowed.file, first_state->reply);
        const auto denied = open(second, u"\\Device\\Configured");
        EXPECT_EQ(STATUS_ACCESS_DENIED, denied.status);
        EXPECT_EQ(0xDEADBEEFu, denied.file.bits);
        second_state->allowed = true;
        const auto enabled = open(second, u"\\Device\\Configured");
        ASSERT_EQ(STATUS_SUCCESS, enabled.status);
        expect_echo(second, enabled.file, second_state->reply);
    }

    TEST(DeviceProviderTest, DosAliasIsExplicit)
    {
        auto emu = make_provider_emulator({{.name = u"Echo", .factory = create_echo, .dos_alias = true}});
        const auto result = open(emu, u"\\??\\Echo");
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        expect_echo(emu, result.file);
        auto no_alias = make_provider_emulator({{.name = u"Echo", .factory = create_echo}});
        const auto fallback = open(no_alias, u"\\??\\Echo");
        EXPECT_NE(STATUS_SUCCESS, fallback.status);
        EXPECT_NE(STATUS_NO_SUCH_DEVICE, fallback.status);
        EXPECT_EQ(0xDEADBEEFu, fallback.file.bits);
    }

    TEST(DeviceProviderTest, RejectsAmbiguousAndInvalidRegistrations)
    {
        for (const auto& name : {u"", u"Nsi", u"Console"})
        {
            EXPECT_THROW((device_registry{{{.name = name, .factory = create_echo}}}), std::invalid_argument);
        }
        EXPECT_THROW((device_registry{{{.name = u"Echo"}}}), std::invalid_argument);
        EXPECT_THROW((device_registry{{{.name = u"Echo", .factory = create_echo}, {.name = u"Echo", .factory = create_echo}}}),
                     std::invalid_argument);
        for (const auto& name : {u"con", u"NUL", u"ConIn$", u"ConOut$", u"NamedPipe", u"NamedPipe\\Echo"})
        {
            EXPECT_THROW((device_registry{{{.name = name, .factory = create_echo, .dos_alias = true}}}), std::invalid_argument);
        }
    }

    TEST(DeviceProviderTest, ValidationRejectsAnUnsupportedArchitectureBeforeConstruction)
    {
        auto emu = make_provider_emulator({{.name = u"NativeOnly", .factory = create_nothing, .validate_open = reject_32_bit}});
        emu.process.is_wow64_process = true;
        const auto result = open(emu, u"\\Device\\NativeOnly");
        EXPECT_EQ(STATUS_NOT_SUPPORTED, result.status);
        EXPECT_EQ(0xDEADBEEFu, result.file.bits);
    }

    TEST(DeviceProviderTest, EmptyFactoryResultCannotBecomeADeviceHandle)
    {
        auto emu = make_provider_emulator({{.name = u"Empty", .factory = create_nothing}});
        EXPECT_THROW(open(emu, u"\\Device\\Empty"), std::runtime_error);
    }

    TEST(DeviceProviderTest, DuplicatedHandleKeepsTheDeviceUsableAfterOriginalClose)
    {
        auto emu = make_provider_emulator({{.name = u"Echo", .factory = create_echo}});
        const auto original = open(emu, u"\\Device\\Echo");
        ASSERT_EQ(STATUS_SUCCESS, original.status);
        const auto storage = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<handle> duplicate{emu.memory, storage};
        ASSERT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtDuplicateObject(context(emu), CURRENT_PROCESS, original.file, CURRENT_PROCESS, duplicate, 0, 0, 0x2));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(context(emu), original.file));
        expect_echo(emu, duplicate.read());
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(context(emu), duplicate.read()));
    }

    TEST(DeviceProviderTest, SharedFileStateRetainsProviderUntilAfterDeviceDestruction)
    {
        destruction_order.clear();
        std::optional<io_device_container> escaped;
        {
            auto owner = std::shared_ptr<void>(new int{}, [](void* value) {
                destruction_order.push_back(2);
                delete static_cast<int*>(value);
            });
            auto emu = make_provider_emulator({{.name = u"Tracked", .factory = create_tracked, .lifetime = owner}});
            owner.reset();
            io_device_container original{u"Tracked", emu, {}};
            escaped.emplace(original.duplicate(GENERIC_ALL, false));
        }
        EXPECT_TRUE(destruction_order.empty());
        escaped.reset();
        EXPECT_EQ((std::vector<int>{1, 2}), destruction_order);
    }

    TEST(DeviceProviderTest, ExternalDevicesCannotWriteSnapshotStateEvenWhenStateless)
    {
        auto emu = make_provider_emulator({{.name = u"Echo", .factory = create_echo}});
        const auto result = open(emu, u"\\Device\\Echo");
        ASSERT_EQ(STATUS_SUCCESS, result.status);
        auto* container = emu.process.devices.get(result.file);
        ASSERT_NE(nullptr, container);
        EXPECT_TRUE(container->get_internal_device()->supports_serialization());
        EXPECT_FALSE(container->supports_serialization());
        utils::buffer_serializer state;
        EXPECT_THROW(emu.serialize(state), std::runtime_error);
        EXPECT_TRUE(state.get_buffer().empty());
        EXPECT_THROW(container->serialize_object(state), std::runtime_error);
        EXPECT_TRUE(state.get_buffer().empty());
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(context(emu), result.file));
        EXPECT_NO_THROW(emu.serialize(state));
        EXPECT_FALSE(state.get_buffer().empty());
    }

    TEST(DeviceProviderTest, BuiltinRestorePreservesSharedDevicesAndSnapshotBytes)
    {
        auto emu = make_provider_emulator();
        io_device_container original{u"PcwDrv", emu, {}};
        auto alias = original.duplicate(GENERIC_READ, true);
        utils::buffer_serializer saved;
        saved.write(original);
        saved.write(alias);
        utils::buffer_deserializer reader{saved};
        reader.register_factory<windows_emulator_wrapper>([&] { return windows_emulator_wrapper{emu}; });
        io_device_container restored;
        io_device_container restored_alias;
        reader.read(restored);
        reader.read(restored_alias);
        EXPECT_EQ(restored.get_internal_device(), restored_alias.get_internal_device());
        EXPECT_EQ(u"PcwDrv", restored.get_device_name());
        EXPECT_TRUE(restored.supports_serialization());
        utils::buffer_serializer round_trip;
        round_trip.write(restored);
        round_trip.write(restored_alias);
        EXPECT_EQ(saved.get_buffer(), round_trip.get_buffer());
    }

    TEST(DeviceProviderTest, ExistingDosSpecialDevicesKeepTheirRoutes)
    {
        auto emu = make_provider_emulator();
        for (const auto path : {u"\\??\\NUL", u"\\??\\CONIN$", u"\\??\\CONOUT$"})
        {
            const auto result = open(emu, path);
            EXPECT_EQ(STATUS_SUCCESS, result.status);
            EXPECT_NE(handle_types::device, result.file.value.type);
        }
        const auto builtin = open(emu, u"\\??\\Nsi");
        EXPECT_EQ(STATUS_SUCCESS, builtin.status);
        EXPECT_EQ(handle_types::device, builtin.file.value.type);
    }
}

#include <windows_emulator.hpp>
#include <backend_selection.hpp>
#include <syscall_utils.hpp>
#include <iostream>

using sogen::NTSTATUS;

namespace
{
    constexpr uint32_t echo_ioctl = 0x222004;

    class echo_device final : public sogen::stateless_device
    {
        NTSTATUS io_control(sogen::windows_emulator& emulator, const sogen::io_device_context& request) override
        {
            if (request.io_control_code != echo_ioctl)
            {
                return STATUS_NOT_SUPPORTED;
            }
            if (request.input_buffer_length != sizeof(uint32_t) || request.output_buffer_length < sizeof(uint32_t))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }
            uint32_t value{};
            if (!sogen::is_readable_range(emulator.memory, request.input_buffer, sizeof(value)) ||
                !sogen::is_writable_range(emulator.memory, request.output_buffer, sizeof(value)) ||
                !emulator.emu().try_read_memory(request.input_buffer, &value, sizeof(value)) ||
                !emulator.emu().try_write_memory(request.output_buffer, &value, sizeof(value)))
            {
                return STATUS_ACCESS_VIOLATION;
            }
            if (request.io_status_block)
            {
                request.io_status_block.access([](auto& status) { status.Information = sizeof(uint32_t); });
            }
            return STATUS_SUCCESS;
        }
    };

    std::unique_ptr<sogen::io_device> create_echo(const sogen::device_creation_context&)
    {
        return std::make_unique<echo_device>();
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2)
        {
            throw std::invalid_argument("Usage: device-provider-example <root containing filesys/>");
        }
        sogen::emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<sogen::null_ui_backend>();
        interfaces.audio = std::make_unique<sogen::null_audio_backend>();
        interfaces.console = std::make_unique<sogen::null_console_backend>();
        interfaces.devices.push_back({.name = u"Echo", .factory = create_echo, .dos_alias = true});
        sogen::windows_emulator emulator{
            sogen::create_x86_64_emulator(sogen::backend_type::unicorn),
            sogen::emulator_settings{.disable_logging = true, .emulation_root = argv[1], .load_registry = false},
            {},
            std::move(interfaces)};
        sogen::io_device_container device{u"Echo", emulator, {}};
        const auto storage = emulator.memory.allocate_memory(0x1000, sogen::memory_permission::read_write);
        constexpr uint32_t payload = 0x12345678;
        emulator.memory.write_memory(storage, payload);
        sogen::io_device_context request{emulator.memory};
        request.io_control_code = echo_ioctl;
        request.input_buffer = storage;
        request.input_buffer_length = sizeof(payload);
        request.output_buffer = storage + 8;
        request.output_buffer_length = sizeof(payload);
        request.io_status_block = {emulator.memory, storage + 16};
        if (device.execute_ioctl(emulator, request) != STATUS_SUCCESS || emulator.memory.read_memory<uint32_t>(storage + 8) != payload ||
            request.io_status_block.read().Information != sizeof(payload))
        {
            throw std::runtime_error("Device exchange failed");
        }
        std::cout << "Device exchange succeeded\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

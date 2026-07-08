#include "std_include.hpp"
#include "io_device.hpp"
#include "windows_emulator.hpp"
#include "devices/afd_endpoint.hpp"
#include "devices/mount_point_manager.hpp"
#include "devices/security_support_provider.hpp"
#include "devices/named_pipe.hpp"
#include "devices/network_store_interface.hpp"
#include "devices/gpu_bridge.hpp"
#include "devices/console.hpp"
#include <iostream>

namespace sogen
{

    namespace
    {
        struct dummy_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator&, const io_device_context&) override
            {
                return STATUS_SUCCESS;
            }
        };

        struct transport_stub_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
            {
                if (context.output_buffer && context.output_buffer_length)
                {
                    std::vector<std::byte> output(context.output_buffer_length, std::byte{0});
                    win_emu.emu().write_memory(context.output_buffer, output.data(), output.size());
                }

                if (context.io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = context.output_buffer_length;
                    context.io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }
        };

        // Uniform factory signature (bool is_32_bit) so every device fits one table entry; nullary
        // factories just ignore the flag.
        std::unique_ptr<io_device> make_dummy(bool)
        {
            return std::make_unique<dummy_device>();
        }
        std::unique_ptr<io_device> make_transport_stub(bool)
        {
            return std::make_unique<transport_stub_device>();
        }
        std::unique_ptr<io_device> make_named_pipe(bool)
        {
            return std::make_unique<named_pipe>();
        }
        std::unique_ptr<io_device> make_console(bool)
        {
            return create_console_device();
        }
        std::unique_ptr<io_device> make_nsi(bool)
        {
            return create_network_store_interface();
        }
        std::unique_ptr<io_device> make_afd_endpoint(bool is_32_bit)
        {
            return create_afd_endpoint(is_32_bit);
        }
        std::unique_ptr<io_device> make_afd_async_connect_hlp(bool is_32_bit)
        {
            return create_afd_async_connect_hlp(is_32_bit);
        }
        std::unique_ptr<io_device> make_mount_point_manager(bool)
        {
            return create_mount_point_manager();
        }
        std::unique_ptr<io_device> make_ksecdd(bool)
        {
            return create_security_support_provider();
        }
        std::unique_ptr<io_device> make_gpu(bool)
        {
            return create_gpu_bridge();
        }

        using namespace std::string_view_literals;

        constexpr std::u16string_view dummy_names[] = {
            u"CNG"sv, u"RasAcd"sv, u"PcwDrv"sv, u"DeviceApi\\CMApi"sv, u"DeviceApi\\CMNotify"sv, u"ConDrv\\Server"sv};
        constexpr std::u16string_view console_names[] = {u"Console"sv};
        constexpr std::u16string_view nsi_names[] = {u"Nsi"sv};
        constexpr std::u16string_view afd_endpoint_names[] = {u"Afd\\Endpoint"sv};
        constexpr std::u16string_view afd_hlp_names[] = {u"Afd\\AsyncConnectHlp"sv};
        constexpr std::u16string_view mount_names[] = {u"MountPointManager"sv};
        constexpr std::u16string_view ksecdd_names[] = {u"KsecDD"sv};
        constexpr std::u16string_view named_pipe_names[] = {u"NamedPipe"sv};
        constexpr std::u16string_view gpu_names[] = {u"SogenGpu"sv};
        constexpr std::u16string_view transport_names[] = {u"Tcp"sv, u"Tcp6"sv, u"Udp"sv, u"RawIp"sv};

        constexpr device_registration registrations[] = {
            {dummy_names, make_dummy},
            {console_names, make_console},
            {nsi_names, make_nsi},
            {afd_endpoint_names, make_afd_endpoint},
            {afd_hlp_names, make_afd_async_connect_hlp},
            {mount_names, make_mount_point_manager},
            {ksecdd_names, make_ksecdd},
            {named_pipe_names, make_named_pipe},
            {gpu_names, make_gpu},
            {transport_names, make_transport_stub},
        };
    }

    std::span<const device_registration> get_device_registrations()
    {
        return registrations;
    }

    bool needs_32_bit_devices(const windows_emulator& win_emu)
    {
        return win_emu.process.is_wow64_process;
    }

    std::unique_ptr<io_device> create_device(const std::u16string_view device, const bool is_32_bit)
    {
        for (const auto& registration : registrations)
        {
            for (const auto name : registration.names)
            {
                if (name == device)
                {
                    return registration.create(is_32_bit);
                }
            }
        }

        throw std::runtime_error("Unsupported device: " + u16_to_u8(device));
    }

    emulator_thread& io_device_context::thread() const
    {
        if (!this->vcpu)
        {
            throw std::runtime_error("I/O request has no issuing vCPU");
        }

        return this->vcpu->thread();
    }

    NTSTATUS io_device::execute_ioctl(windows_emulator& win_emu, const io_device_context& c)
    {
        if (c.io_status_block)
        {
            c.io_status_block.write({});
        }

        const auto result = this->io_control(win_emu, c);
        write_io_status(c.io_status_block, result);

        // A synchronously-completing IOCTL must signal the optional completion event the caller passed, so a
        // thread that issues the request and then waits on the event is released. Asynchronous devices return
        // STATUS_PENDING and signal the event themselves once the delayed operation completes.
        if (result != STATUS_PENDING && c.event.bits)
        {
            if (auto* e = win_emu.process.events.get(c.event); e)
            {
                e->signaled = true;
            }
        }

        return result;
    }

    NTSTATUS io_device_container::io_control(windows_emulator& win_emu, const io_device_context& context)
    {
        this->assert_validity();
        win_emu.callbacks.on_ioctrl(*this->device_, this->device_name_, context.io_control_code);
        return this->device_->io_control(win_emu, context);
    }

    void io_device_container::work(windows_emulator& win_emu)
    {
        this->assert_validity();
        this->device_->work(win_emu);
    }

    void io_device_container::serialize_object(utils::buffer_serializer& buffer) const
    {
        this->assert_validity();

        buffer.write(this->is_32_bit_);
        buffer.write_string(this->device_name_);
        this->device_->serialize(buffer);
    }

    void io_device_container::deserialize_object(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->is_32_bit_);
        buffer.read_string(this->device_name_);

        this->setup();
        this->device_->deserialize(buffer);
    }

} // namespace sogen

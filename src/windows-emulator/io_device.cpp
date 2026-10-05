#include "std_include.hpp"
#include "io_device.hpp"
#include "io_completion_wait.hpp"
#include "windows_emulator.hpp"
#include "devices/afd_endpoint.hpp"
#include "devices/mount_point_manager.hpp"
#include "devices/security_support_provider.hpp"
#include "devices/named_pipe.hpp"
#include "devices/network_store_interface.hpp"
#include "devices/gpu_bridge.hpp"
#include "devices/steam_bridge.hpp"
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

        // A device whose handle opens but that fails every ioctl. Used for the PnP DevQuery interface that
        // audioses probes for device properties: a clean error makes the caller fall back to its defaults,
        // whereas returning success with no data leads it to read uninitialized output.
        struct unsupported_io_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator&, const io_device_context&) override
            {
                return STATUS_NOT_SUPPORTED;
            }
        };

        struct transport_stub_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
            {
                if (context.output_buffer && context.output_buffer_length)
                {
                    win_emu.emu().set_memory(context.output_buffer, 0, context.output_buffer_length);
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

        void write_wow64_io_status(const io_device_context& context)
        {
            if (!context.wow64_x86_io_status_block || !context.io_status_block)
            {
                return;
            }

            const auto native_status = context.io_status_block.read();
            const auto status32 = static_cast<uint32_t>(native_status.Status);
            const auto information32 = static_cast<uint32_t>(native_status.Information);
            auto* memory = context.io_status_block.get_memory_interface();
            memory->write_memory(context.wow64_x86_io_status_block, &status32, sizeof(status32));
            memory->write_memory(context.wow64_x86_io_status_block + sizeof(status32), &information32, sizeof(information32));
        }

        // Factories for the devices defined locally in this file, matching the shared create_*
        // (device_creation_context) signature so they slot straight into the registry.
        std::unique_ptr<io_device> create_dummy_device(const device_creation_context&)
        {
            return std::make_unique<dummy_device>();
        }

        std::unique_ptr<io_device> create_transport_stub_device(const device_creation_context&)
        {
            return std::make_unique<transport_stub_device>();
        }

        std::unique_ptr<io_device> create_unsupported_io_device(const device_creation_context&)
        {
            return std::make_unique<unsupported_io_device>();
        }

        std::unique_ptr<io_device> create_named_pipe_device(const device_creation_context&)
        {
            return std::make_unique<named_pipe>();
        }

    }

    const std::map<std::u16string_view, device_factory>& get_device_registry()
    {
        using namespace std::string_view_literals;

        static const std::map<std::u16string_view, device_factory> registry = {
            // Dummy
            {u"CNG"sv, create_dummy_device},
            {u"RasAcd"sv, create_dummy_device},
            {u"PcwDrv"sv, create_dummy_device},
            {u"DeviceApi\\CMApi"sv, create_dummy_device},
            {u"DeviceApi\\CMNotify"sv, create_dummy_device},
            {u"ConDrv\\Server"sv, create_dummy_device},
            {u"DeviceApi\\Dev\\Query"sv, create_unsupported_io_device},
            // Generic
            {u"Console"sv, create_console_device},
            // Multimedia Class Scheduler. avrt!AvSetMmThreadCharacteristics opens this to raise the audio
            // render worker's scheduling priority; the emulator has no priority classes, so accepting the open
            // and succeeding its ioctls is enough for the worker to proceed.
            {u"MMCSS\\MmThread"sv, create_dummy_device},
            {u"Nsi"sv, create_network_store_interface},
            {u"MountPointManager"sv, create_mount_point_manager},
            {u"KsecDD"sv, create_security_support_provider},
            {u"NamedPipe"sv, create_named_pipe_device},
            {u"SogenGpu"sv, create_gpu_bridge},
            {u"SogenSteam"sv, create_steam_bridge},
            {u"Afd\\Mio"sv, create_afd_mio},
            // AFD
            {u"Afd\\Endpoint"sv, create_afd_endpoint},
            {u"Afd\\AsyncConnectHlp"sv, create_afd_async_connect_hlp},
            // Transport
            {u"Tcp"sv, create_transport_stub_device},
            {u"Tcp6"sv, create_transport_stub_device},
            {u"Udp"sv, create_transport_stub_device},
            {u"RawIp"sv, create_transport_stub_device},
        };
        return registry;
    }

    bool needs_32_bit_devices(const windows_emulator& win_emu)
    {
        return win_emu.process.is_wow64_process;
    }

    std::unique_ptr<io_device> create_device(const std::u16string_view device, const device_creation_context& context)
    {
        const auto& registry = get_device_registry();
        const auto it = registry.find(device);
        if (it == registry.end())
        {
            throw std::runtime_error("Unsupported device: " + u16_to_u8(device));
        }

        return it->second(context);
    }

    NTSTATUS io_device_container::set_completion_association(process_context& process, const emulator_thread* active_thread,
                                                             const handle completion_port, const uint64_t key)
    {
        const auto resolved_completion_port = process.resolve_object_pseudo_handle(completion_port, active_thread);
        if (resolved_completion_port.value.type != handle_types::io_completion || !process.io_completions.get(resolved_completion_port))
        {
            return STATUS_INVALID_HANDLE;
        }

        handle retained_completion_port{};
        if (!io_completion_wait::retain_handle_reference(process, active_thread, resolved_completion_port, retained_completion_port))
        {
            return STATUS_INVALID_HANDLE;
        }

        if (this->completion_association_)
        {
            io_completion_wait::release_handle_reference(process, this->completion_association_->completion_port);
        }

        this->completion_association_ = device_completion_association{
            .completion_port = retained_completion_port,
            .key = key,
        };
        return STATUS_SUCCESS;
    }

    void io_device_container::set_completion_notification_flags(const uint32_t flags)
    {
        this->completion_notification_flags_ = flags;
    }

    void io_device_container::release_references(process_context& process)
    {
        this->assert_validity();
        this->device_->release_references(process);

        if (this->completion_association_)
        {
            io_completion_wait::release_handle_reference(process, this->completion_association_->completion_port);
            this->completion_association_ = {};
        }
    }

    NTSTATUS io_device_container::io_control(windows_emulator& win_emu, const io_device_context& context)
    {
        this->assert_validity();
        win_emu.callbacks.on_ioctrl(*this->device_, this->device_name_, context.io_control_code);

        auto request = context;
        request.completion_port = this->completion_association_ ? this->completion_association_->completion_port : handle{};
        request.completion_key = this->completion_association_ ? this->completion_association_->key : 0;
        request.completion_notification_flags = this->completion_notification_flags_;
        return this->device_->io_control(win_emu, request);
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
        write_wow64_io_status(c);

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

    void io_device_container::work(windows_emulator& win_emu)
    {
        this->assert_validity();
        this->device_->work(win_emu);
    }

    void io_device_container::restore_after_state_restore(windows_emulator& win_emu)
    {
        this->assert_validity();
        this->device_->restore_after_state_restore(win_emu);
    }

    void io_device_container::serialize_object(utils::buffer_serializer& buffer) const
    {
        this->assert_validity();

        buffer.write(this->is_32_bit_);
        buffer.write_string(this->device_name_);
        buffer.write_optional(this->completion_association_);
        buffer.write(this->completion_notification_flags_);
        this->device_->serialize(buffer);
    }

    void io_device_container::deserialize_object(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->is_32_bit_);
        buffer.read_string(this->device_name_);
        buffer.read_optional(this->completion_association_);
        buffer.read(this->completion_notification_flags_);

        this->setup();
        this->device_->deserialize(buffer);
    }

} // namespace sogen

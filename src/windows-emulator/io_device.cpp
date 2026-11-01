#include "std_include.hpp"
#include "io_device.hpp"
#include "io_completion_wait.hpp"
#include "windows_emulator.hpp"
#include "devices/afd_endpoint.hpp"
#include "devices/mount_point_manager.hpp"
#include "devices/security_support_provider.hpp"
#include "devices/cryptography.hpp"
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

        // \Device\DeviceApi\CMApi, which cfgmgr32 asks for device lists and properties. Device ID lists come back
        // empty, i.e. no device matches the filter. Succeeding without writing the reply made
        // CM_Get_Device_ID_List_Size fail, so SetupDiGetClassDevs returned INVALID_HANDLE_VALUE, on which Unreal
        // Engine 4 enumerates monitors forever. Other requests keep succeeding without data.
        struct device_api_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                constexpr ULONG ioctl_get_device_id_list = 0x470803;
                if (c.io_control_code != ioctl_get_device_id_list)
                {
                    return STATUS_SUCCESS;
                }

                // cfgmgr32 reads the request's status and the list size from this header and the list right after it,
                // but sizes its buffer as 20 bytes plus the list, and asks for the size with a 20-byte buffer.
                struct device_id_list_header
                {
                    uint32_t reserved1;
                    NTSTATUS status;
                    uint32_t list_bytes;
                    uint32_t reserved2;
                };

                static_assert(sizeof(device_id_list_header) == 16);

                constexpr ULONG minimum_reply = 20;
                constexpr char16_t empty_list = u'\0';
                if (!c.output_buffer || c.output_buffer_length < minimum_reply)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const bool fits = c.output_buffer_length >= minimum_reply + sizeof(empty_list);
                const device_id_list_header header{
                    .reserved1 = 0,
                    .status = fits ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL,
                    .list_bytes = sizeof(empty_list),
                    .reserved2 = 0,
                };
                win_emu.memory.write_memory(c.output_buffer, &header, sizeof(header));
                if (fits)
                {
                    win_emu.memory.write_memory(c.output_buffer + sizeof(header), &empty_list, sizeof(empty_list));
                }

                return STATUS_SUCCESS;
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

        // Factories for the devices defined locally in this file, matching the shared create_*
        // (device_creation_context) signature so they slot straight into the registry.
        std::unique_ptr<io_device> create_dummy_device(const device_creation_context&)
        {
            return std::make_unique<dummy_device>();
        }

        std::unique_ptr<io_device> create_device_api_device(const device_creation_context&)
        {
            return std::make_unique<device_api_device>();
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
            {u"RasAcd"sv, create_dummy_device},
            {u"PcwDrv"sv, create_dummy_device},
            {u"DeviceApi\\CMApi"sv, create_device_api_device},
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
            {u"CNG"sv, create_cryptography_device},
            {u"NamedPipe"sv, create_named_pipe_device},
            {u"SogenGpu"sv, create_gpu_bridge},
            {u"SogenSteam"sv, create_steam_bridge},
            // AFD
            {u"Afd\\Endpoint"sv, create_afd_endpoint},
            {u"Afd\\Mio"sv, create_afd_mio_endpoint},
            {u"Afd\\AsyncConnectHlp"sv, create_afd_async_connect_hlp},
            // Transport
            {u"Tcp"sv, create_transport_stub_device},
            {u"Tcp6"sv, create_transport_stub_device},
            {u"Udp"sv, create_transport_stub_device},
            {u"RawIp"sv, create_transport_stub_device},
        };
        return registry;
    }

    bool is_device_supported(std::u16string_view device)
    {
        const auto& registry = get_device_registry();
        return registry.contains(device);
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
        auto request = this->prepare_io_context(c);
        if (request.io_status_block)
        {
            request.io_status_block.write({});
        }

        const auto result = this->io_control(win_emu, request);
        write_io_status(request, result);

        // A synchronously-completing IOCTL must signal the optional completion event the caller passed, so a
        // thread that issues the request and then waits on the event is released. Asynchronous devices return
        // STATUS_PENDING and signal the event themselves once the delayed operation completes.
        if (result != STATUS_PENDING && request.event.bits)
        {
            if (auto* e = win_emu.process.events.get(request.event); e)
            {
                e->signaled = true;
                win_emu.wake_idle_vcpu();
            }
        }

        constexpr ULONG file_skip_completion_port_on_success = 0x1;
        if (result != STATUS_PENDING && NT_SUCCESS(result) &&
            !(request.completion_notification_flags & file_skip_completion_port_on_success))
        {
            queue_io_completion(win_emu, request);
        }

        return result;
    }

    void io_device::queue_io_completion(windows_emulator& win_emu, const io_device_context& context)
    {
        if (!context.completion_port.bits || !context.io_status_block || context.suppresses_completion_packet())
        {
            return;
        }

        auto* completion = win_emu.process.io_completions.get(context.completion_port);
        if (!completion)
        {
            return;
        }

        io_completion_message message{};
        message.key_context = context.completion_key;
        message.apc_context = context.apc_context;
        message.io_status_block = context.io_status_block.read();
        completion->enqueue(message);
        win_emu.wake_idle_vcpu();
    }

    NTSTATUS io_device_container::io_control(windows_emulator& win_emu, const io_device_context& context)
    {
        this->assert_validity();
        win_emu.callbacks.on_ioctrl(*this->file_->device, this->device_name_, context.io_control_code);
        return this->file_->device->io_control(win_emu, context);
    }

    io_device_context io_device_container::prepare_io_context(const io_device_context& context) const
    {
        auto request = context;
        if (this->file_->completion_association)
        {
            request.completion_port = this->file_->completion_association->completion_port;
            request.completion_key = this->file_->completion_association->key;
        }
        request.completion_notification_flags = this->file_->completion_notification_flags;
        request.granted_access = this->granted_access;
        if (request.vcpu && request.vcpu->active_thread)
        {
            request.issuing_thread_id = request.thread().id;
        }
        return request;
    }

    bool io_device_container::cancel_io(windows_emulator& win_emu, const uint64_t io_status_block,
                                        const std::optional<uint32_t> issuing_thread_id)
    {
        this->assert_validity();
        return this->file_->device->cancel_io(win_emu, io_status_block, issuing_thread_id);
    }

    bool io_device_container::has_pending_io(const uint32_t issuing_thread_id) const
    {
        this->assert_validity();
        return this->file_->device->has_pending_io(issuing_thread_id);
    }

    NTSTATUS io_device_container::set_completion_association(process_context& process, const emulator_thread* active_thread,
                                                             const handle completion_port, const uint64_t key)
    {
        this->assert_validity();
        if (this->file_->completion_association)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto resolved_port = process.resolve_object_pseudo_handle(completion_port, active_thread);
        if (resolved_port.value.type != handle_types::io_completion || !process.io_completions.get(resolved_port))
        {
            return STATUS_INVALID_HANDLE;
        }

        handle retained_port{};
        if (!io_completion_wait::retain_handle_reference(process, active_thread, resolved_port, retained_port))
        {
            return STATUS_INVALID_HANDLE;
        }

        this->file_->completion_association = device_completion_association{.completion_port = retained_port, .key = key};
        return STATUS_SUCCESS;
    }

    void io_device_container::set_completion_notification_flags(const ULONG flags)
    {
        this->assert_validity();
        this->file_->completion_notification_flags = flags;
    }

    void io_device_container::release_references(process_context& process)
    {
        this->assert_validity();
        if (!this->is_last_file_reference())
        {
            return;
        }
        this->file_->device->release_references(process);
        if (this->file_->completion_association)
        {
            io_completion_wait::release_handle_reference(process, this->file_->completion_association->completion_port);
            this->file_->completion_association.reset();
        }
    }

    void io_device_container::work(windows_emulator& win_emu)
    {
        this->assert_validity();
        this->file_->device->work(win_emu);
    }

    void io_device_container::restore_after_state_restore(windows_emulator& win_emu)
    {
        this->assert_validity();
        this->file_->device->restore_after_state_restore(win_emu);
    }

    bool io_device_container::supports_serialization() const
    {
        this->assert_validity();
        return this->file_->device->supports_serialization();
    }

    void io_device_container::prepare_serialization(windows_emulator& win_emu)
    {
        this->assert_validity();
        this->file_->device->prepare_serialization(win_emu);
    }

    bool io_device_container::suspends_at_frame_end() const
    {
        this->assert_validity();
        return this->file_->device->suspends_at_frame_end();
    }

    void io_device_container::serialize_object(utils::buffer_serializer& buffer) const
    {
        this->assert_validity();

        buffer.write(this->is_32_bit_);
        buffer.write_string(this->device_name_);
        buffer.write(this->granted_access);
        buffer.write_shared(this->file_, [&](const file_state& file) {
            buffer.write_optional(file.completion_association);
            buffer.write(file.completion_notification_flags);
            file.device->serialize(buffer);
        });
    }

    void io_device_container::deserialize_object(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->is_32_bit_);
        buffer.read_string(this->device_name_);
        buffer.read(this->granted_access);
        buffer.read_shared(this->file_, [&](file_state& file) {
            buffer.read_optional(file.completion_association);
            buffer.read(file.completion_notification_flags);
            file.device = create_device(this->device_name_, {.is_32_bit = this->is_32_bit_});
            file.device->deserialize(buffer);
        });
    }

    io_device_container io_device_container::duplicate(const ACCESS_MASK access, const bool inheritable) const
    {
        this->assert_validity();
        io_device_container result;
        result.is_32_bit_ = this->is_32_bit_;
        result.device_name_ = this->device_name_;
        result.file_ = this->file_;
        result.granted_access = access;
        result.inherit = inheritable;
        return result;
    }

} // namespace sogen

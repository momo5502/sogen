#include "../std_include.hpp"
#include "device_api.hpp"

#include "../windows_emulator.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{
    namespace
    {
        // \Device\DeviceApi\CMApi, which cfgmgr32 asks for device and interface lists, class keys and properties. Device ID
        // lists come back empty, i.e. no device matches the filter. Succeeding without writing the reply made
        // CM_Get_Device_ID_List_Size fail, so SetupDiGetClassDevs returned INVALID_HANDLE_VALUE, on which Unreal
        // Engine 4 enumerates monitors forever. Other requests keep succeeding without data.
        struct device_api_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                constexpr ULONG ioctl_get_device_id_list = 0x470803;
                constexpr ULONG ioctl_get_device_interface_list = 0x470807;
                constexpr ULONG ioctl_open_class_key = 0x470863;

                switch (c.io_control_code)
                {
                case ioctl_get_device_id_list:
                    return get_device_id_list(win_emu, c);
                case ioctl_get_device_interface_list:
                    return get_device_interface_list(win_emu, c);
                case ioctl_open_class_key:
                    return open_class_key(win_emu, c);
                default:
                    return STATUS_SUCCESS;
                }
            }

          private:
            // The longest name RtlInitUnicodeStringEx takes (0xFFFC bytes). PiCMOpenClassKey fails a longer class name, and
            // PiCMGetDeviceInterfaceList a longer device ID, with its STATUS_NAME_TOO_LONG.
            static constexpr size_t max_name_length = 0x7FFE;

            // How PiCMCaptureInterfaceListInputData and PiCMCaptureRegistryInputData read a request. The ioctls are
            // METHOD_NEITHER, so the request is the caller's own memory: the kernel probes its declared length for read,
            // aligned to the request's widest field, and reads the fields in place, a WoW64 request only once its size says
            // it is one. Those faults fail the request.
            template <typename Input, typename Input32>
            static NTSTATUS read_request(windows_emulator& win_emu, const io_device_context& c, Input& input, Input32& input32)
            {
                if (!c.input_buffer || c.input_buffer_length == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const bool wow64 = win_emu.process.is_wow64_process;
                const uint64_t input_end = c.input_buffer + c.input_buffer_length;
                if (c.input_buffer % (wow64 ? sizeof(uint32_t) : sizeof(uint64_t)) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }
                if (input_end > MAX_ALLOCATION_END_EXCL || input_end < c.input_buffer)
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                if (wow64)
                {
                    // Only the size is read before it is checked.
                    if (c.input_buffer_length < sizeof(input32))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    if (!is_readable_range(win_emu.memory, c.input_buffer, sizeof(input32.size)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    if (win_emu.memory.read_memory<uint32_t>(c.input_buffer) != sizeof(input32))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    if (!is_readable_range(win_emu.memory, c.input_buffer, sizeof(input32)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }

                    win_emu.memory.read_memory(c.input_buffer, &input32, sizeof(input32));
                    return STATUS_SUCCESS;
                }

                if (c.input_buffer_length < sizeof(input))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (!is_readable_range(win_emu.memory, c.input_buffer, sizeof(input)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                win_emu.memory.read_memory(c.input_buffer, &input, sizeof(input));
                return input.size == sizeof(input) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
            }

            // PiControlMakeUserModeCallersCopy for a name, as both captures call it: the probe of all of its bytes for read,
            // aligned to a character, fails the request. The kernel then terminates its copy at the last character and uses
            // the name up to the first NUL, so only that much is read here: a guest-chosen length up to 4 GB does not size a
            // host allocation. Past max_name_length the name fails anyway (RtlInitUnicodeStringEx), and the read stops one
            // character beyond it.
            static NTSTATUS capture_name(windows_emulator& win_emu, const uint64_t address, const uint32_t bytes, std::u16string& name)
            {
                if (address % sizeof(char16_t) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }
                if (!is_readable_range(win_emu.memory, address, bytes))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                const uint64_t usable = (bytes / sizeof(char16_t)) - 1;
                name.clear();
                std::array<char16_t, 0x100> piece{};
                for (uint64_t offset = 0; offset < usable && name.size() <= max_name_length; offset += piece.size())
                {
                    const auto count = static_cast<size_t>(std::min<uint64_t>(piece.size(), usable - offset));
                    win_emu.memory.read_memory(address + (offset * sizeof(char16_t)), piece.data(), count * sizeof(char16_t));
                    const char16_t* const begin = piece.data();
                    const auto* const end = std::ranges::find(begin, begin + count, u'\0');
                    name.append(begin, end);
                    if (end != begin + count)
                    {
                        break;
                    }
                }

                return STATUS_SUCCESS;
            }

            // The probe PiCMReturnHandleResultData and PiCMReturnBufferResultData make before they write a reply: the
            // output buffer is the caller's own too, and all of it must be writable, ULONG-aligned.
            static NTSTATUS probe_output_buffer(windows_emulator& win_emu, const io_device_context& c)
            {
                if (c.output_buffer % sizeof(ULONG) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }

                return is_writable_range(win_emu.memory, c.output_buffer, c.output_buffer_length) ? STATUS_SUCCESS
                                                                                                  : STATUS_ACCESS_VIOLATION;
            }

            // The interface list request CM_Get_Device_Interface_List and its size query send. WoW64 callers send a 36-byte
            // layout with a 32-bit device ID pointer, which PiCMCaptureInterfaceListInputData widens to this one.
            struct interface_list_input
            {
                uint32_t size;
                uint32_t flags;
                std::array<uint8_t, 16> interface_class;
                uint64_t device_id;
                uint32_t device_id_bytes;
                uint32_t result_size;
            };

            static_assert(sizeof(interface_list_input) == 40);

            struct interface_list_input32
            {
                uint32_t size;
                uint32_t flags;
                std::array<uint8_t, 16> interface_class;
                uint32_t device_id;
                uint32_t device_id_bytes;
                uint32_t result_size;
            };

            static_assert(sizeof(interface_list_input32) == 36);

            // The reply PiCMReturnBufferResultData writes: this header, then the data, which starts over its last field.
            struct buffer_result
            {
                uint32_t size;
                NTSTATUS status;
                uint32_t data_bytes;
                uint32_t reserved;
                uint32_t data;
            };

            static_assert(sizeof(buffer_result) == 20);

            // PiCMCaptureInterfaceListInputData. The device ID is captured like a class name, aligned to a character.
            static NTSTATUS capture_interface_list_input(windows_emulator& win_emu, const io_device_context& c, interface_list_input& input,
                                                         std::u16string& device_id)
            {
                interface_list_input32 input32{};
                if (const auto status = read_request(win_emu, c, input, input32); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (win_emu.process.is_wow64_process)
                {
                    input = {
                        .size = sizeof(input),
                        .flags = input32.flags,
                        .interface_class = input32.interface_class,
                        .device_id = input32.device_id,
                        .device_id_bytes = input32.device_id_bytes,
                        .result_size = input32.result_size,
                    };
                }

                if (input.device_id ? input.device_id_bytes < sizeof(char16_t) : input.device_id_bytes != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                return input.device_id ? capture_name(win_emu, input.device_id, input.device_id_bytes, device_id) : STATUS_SUCCESS;
            }

            // PiCMReturnBufferResultData: the header with the request's status and the data's size, then the data, which is
            // only copied on success. A reply that does not fit, or an output buffer the probe rejects, fails the ioctl
            // itself.
            static NTSTATUS return_buffer_result(windows_emulator& win_emu, const io_device_context& c, const NTSTATUS status,
                                                 const uint32_t data_bytes, const std::span<const uint8_t> data, const uint32_t result_size)
            {
                if (result_size != sizeof(buffer_result) || c.output_buffer_length < data.size() + sizeof(buffer_result))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (const auto probe = probe_output_buffer(win_emu, c); probe != STATUS_SUCCESS)
                {
                    return probe;
                }

                const buffer_result header{
                    .size = sizeof(buffer_result), .status = status, .data_bytes = data_bytes, .reserved = 0, .data = 0};
                win_emu.memory.write_memory(c.output_buffer, &header, sizeof(header));
                win_emu.memory.write_memory(c.output_buffer + offsetof(buffer_result, data), data.data(), data.size());
                if (c.io_status_block)
                {
                    c.io_status_block.access(
                        [&](IO_STATUS_BLOCK<EmulatorTraits<Emu64>>& block) { block.Information = data.size() + sizeof(buffer_result); });
                }

                return STATUS_SUCCESS;
            }

            // PiCMGetDeviceInterfaceList. The emulator has no device interfaces, just as it has no devices, so every class
            // gets the list IopGetDeviceInterfaces builds when nothing matches, a lone terminator, for any device ID
            // RtlInitUnicodeStringEx takes. A size query's 20-byte buffer has no room for it, so it learns the size from a
            // STATUS_BUFFER_TOO_SMALL reply.
            static NTSTATUS get_device_interface_list(windows_emulator& win_emu, const io_device_context& c)
            {
                constexpr uint32_t present_only = 0x10000;
                constexpr std::array<uint8_t, sizeof(char16_t)> empty_list{};
                constexpr auto empty_list_bytes = static_cast<uint32_t>(empty_list.size());

                interface_list_input input{};
                std::u16string device_id{};
                if (const auto status = capture_interface_list_input(win_emu, c, input, device_id); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (!c.output_buffer || c.output_buffer_length < sizeof(buffer_result) || (input.flags & ~present_only) != 0)
                {
                    return return_buffer_result(win_emu, c, STATUS_INVALID_PARAMETER, 0, {}, input.result_size);
                }

                if (device_id.size() > max_name_length)
                {
                    return return_buffer_result(win_emu, c, STATUS_NAME_TOO_LONG, 0, {}, input.result_size);
                }

                if (c.output_buffer_length - sizeof(buffer_result) < empty_list_bytes)
                {
                    return return_buffer_result(win_emu, c, STATUS_BUFFER_TOO_SMALL, empty_list_bytes, {}, input.result_size);
                }

                return return_buffer_result(win_emu, c, STATUS_SUCCESS, empty_list_bytes, empty_list, input.result_size);
            }

            // The class key request cfgmgr32's InitializeInputRegistryData builds. WoW64 callers send a 36-byte layout
            // with 32-bit fields, which PiCMCaptureRegistryInputData widens to this one.
            struct registry_input
            {
                uint32_t size;
                uint32_t reserved1;
                uint32_t key_type;
                uint32_t reserved2;
                uint64_t class_name;
                uint32_t class_name_bytes;
                uint32_t desired_access;
                uint32_t disposition;
                uint32_t reserved3;
                uint64_t result_size;
            };

            static_assert(sizeof(registry_input) == 48);

            struct registry_input32
            {
                uint32_t size;
                uint32_t reserved1;
                uint32_t key_type;
                uint32_t class_name;
                uint32_t class_name_bytes;
                uint32_t desired_access;
                uint32_t disposition;
                uint32_t reserved3;
                uint32_t result_size;
            };

            static_assert(sizeof(registry_input32) == 36);

            struct handle_result
            {
                uint32_t size;
                NTSTATUS status;
                uint64_t handle;
            };

            static_assert(sizeof(handle_result) == 16);

            struct class_key_request
            {
                registry_input input{};
                std::optional<std::u16string> class_name{};
            };

            // PiCMCaptureRegistryInputData
            static NTSTATUS capture_registry_input(windows_emulator& win_emu, const io_device_context& c, class_key_request& request)
            {
                auto& input = request.input;
                registry_input32 input32{};
                if (const auto status = read_request(win_emu, c, input, input32); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (win_emu.process.is_wow64_process)
                {
                    input = {
                        .size = sizeof(input),
                        .reserved1 = input32.reserved1,
                        .key_type = input32.key_type,
                        .reserved2 = 0,
                        .class_name = input32.class_name,
                        .class_name_bytes = input32.class_name_bytes,
                        .desired_access = input32.desired_access,
                        .disposition = input32.disposition,
                        .reserved3 = input32.reserved3,
                        .result_size = input32.result_size,
                    };
                }

                if (input.class_name ? input.class_name_bytes < sizeof(char16_t) : input.class_name_bytes != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (input.class_name)
                {
                    if (const auto status = capture_name(win_emu, input.class_name, input.class_name_bytes, request.class_name.emplace());
                        status != STATUS_SUCCESS)
                    {
                        return status;
                    }
                }

                return STATUS_SUCCESS;
            }

            // PiCMOpenClassKey: opens Control\Class for installer classes or Control\DeviceClasses for interface classes,
            // or the class's {GUID} key under it, and hands the caller its own handle. The reply carries the open's status,
            // so the ioctl itself succeeds whenever the request is well formed and the reply fits.
            static NTSTATUS open_class_key(windows_emulator& win_emu, const io_device_context& c)
            {
                constexpr uint32_t installer_class = 2;
                constexpr uint32_t interface_class = 3;
                constexpr uint32_t open_always = 1;

                class_key_request request{};
                if (const auto status = capture_registry_input(win_emu, c, request); status != STATUS_SUCCESS)
                {
                    return status;
                }

                // PiCMReturnHandleResultData
                const auto& input = request.input;
                if (!c.output_buffer || c.output_buffer_length < sizeof(handle_result) || input.result_size != sizeof(handle_result))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                // When PiCMReturnHandleResultData's probe fails, the request fails and PiCMOpenClassKey closes the handle it
                // made, so the probe comes before any key is opened here.
                if (const auto probe = probe_output_buffer(win_emu, c); probe != STATUS_SUCCESS)
                {
                    return probe;
                }

                handle_result result{.size = sizeof(handle_result), .status = STATUS_INVALID_PARAMETER, .handle = 0};
                const bool well_formed = input.reserved1 == 0 && input.reserved3 == 0 &&
                                         (input.key_type == installer_class || input.key_type == interface_class);
                if (well_formed && request.class_name && request.class_name->size() > max_name_length)
                {
                    result.status = STATUS_NAME_TOO_LONG;
                }
                else if (well_formed)
                {
                    std::u16string path = input.key_type == interface_class
                                              ? u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\DeviceClasses"
                                              : u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class";
                    if (request.class_name)
                    {
                        path += u"\\" + *request.class_name;
                    }

                    if (auto key = win_emu.registry.get_key({path}))
                    {
                        result.status = STATUS_SUCCESS;
                        result.handle = win_emu.process.registry_keys.store(std::move(*key)).bits;
                    }
                    else
                    {
                        // Creating a missing class needs PnP write access (PiAuDoesClientHaveAccess), which the
                        // non-elevated emulated user lacks.
                        result.status = input.disposition == open_always ? STATUS_ACCESS_DENIED : STATUS_OBJECT_NAME_NOT_FOUND;
                    }
                }

                win_emu.memory.write_memory(c.output_buffer, &result, sizeof(result));
                if (c.io_status_block)
                {
                    c.io_status_block.access(
                        [](IO_STATUS_BLOCK<EmulatorTraits<Emu64>>& block) { block.Information = sizeof(handle_result); });
                }

                return STATUS_SUCCESS;
            }

            static NTSTATUS get_device_id_list(windows_emulator& win_emu, const io_device_context& c)
            {
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
    }

    std::unique_ptr<io_device> create_device_api_device(const device_creation_context&)
    {
        return std::make_unique<device_api_device>();
    }
}

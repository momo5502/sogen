#include "../std_include.hpp"
#include "device_api.hpp"

#include "../windows_emulator.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{
    namespace
    {
        // \Device\DeviceApi\CMApi, which cfgmgr32 asks for device lists, class keys and properties. Device ID lists
        // come back empty, i.e. no device matches the filter. Succeeding without writing the reply made
        // CM_Get_Device_ID_List_Size fail, so SetupDiGetClassDevs returned INVALID_HANDLE_VALUE, on which Unreal
        // Engine 4 enumerates monitors forever. Other requests keep succeeding without data.
        struct device_api_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                constexpr ULONG ioctl_get_device_id_list = 0x470803;
                constexpr ULONG ioctl_open_class_key = 0x470863;

                switch (c.io_control_code)
                {
                case ioctl_get_device_id_list:
                    return get_device_id_list(win_emu, c);
                case ioctl_open_class_key:
                    return open_class_key(win_emu, c);
                default:
                    return STATUS_SUCCESS;
                }
            }

          private:
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

            // The longest name RtlInitUnicodeStringEx takes (0xFFFC bytes); PiCMOpenClassKey fails a longer one with its
            // STATUS_NAME_TOO_LONG.
            static constexpr size_t max_class_name_length = 0x7FFE;

            struct class_key_request
            {
                registry_input input{};
                std::optional<std::u16string> class_name{};
            };

            // PiCMCaptureRegistryInputData
            static NTSTATUS capture_registry_input(windows_emulator& win_emu, const io_device_context& c, class_key_request& request)
            {
                if (!c.input_buffer || c.input_buffer_length == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                // The ioctl is METHOD_NEITHER, so the request is the caller's own memory: the kernel probes its declared
                // length for read, aligned to the request's widest field, and reads the fields in place. Those faults fail
                // the request here.
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

                auto& input = request.input;
                if (wow64)
                {
                    // Only the size is read before it is checked.
                    registry_input32 input32{};
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
                else
                {
                    if (c.input_buffer_length < sizeof(input))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    if (!is_readable_range(win_emu.memory, c.input_buffer, sizeof(input)))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }

                    win_emu.memory.read_memory(c.input_buffer, &input, sizeof(input));
                    if (input.size != sizeof(input))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                }

                if (input.class_name ? input.class_name_bytes < sizeof(char16_t) : input.class_name_bytes != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (input.class_name)
                {
                    // PiControlMakeUserModeCallersCopy copies all class_name_bytes, and the probe of its copy fails the
                    // request with STATUS_ACCESS_VIOLATION when the range cannot be read. The kernel then terminates its copy
                    // at the last character and opens the name up to the first NUL, so only that much is read here: a
                    // guest-chosen length up to 4 GB does not size a host allocation. Past max_class_name_length the open
                    // fails anyway (RtlInitUnicodeStringEx), and the read stops one character beyond it.
                    if (!is_readable_range(win_emu.memory, input.class_name, input.class_name_bytes))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }

                    const uint64_t usable = (input.class_name_bytes / sizeof(char16_t)) - 1;
                    std::u16string name{};
                    std::array<char16_t, 0x100> piece{};
                    for (uint64_t offset = 0; offset < usable && name.size() <= max_class_name_length; offset += piece.size())
                    {
                        const auto count = static_cast<size_t>(std::min<uint64_t>(piece.size(), usable - offset));
                        win_emu.memory.read_memory(input.class_name + (offset * sizeof(char16_t)), piece.data(), count * sizeof(char16_t));
                        const char16_t* const begin = piece.data();
                        const auto* const end = std::ranges::find(begin, begin + count, u'\0');
                        name.append(begin, end);
                        if (end != begin + count)
                        {
                            break;
                        }
                    }

                    request.class_name = std::move(name);
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

                // 0x470863 is METHOD_NEITHER, so the output buffer is the caller's own: PiCMReturnHandleResultData probes all
                // of it for write, ULONG-aligned, before it writes the result. When that fails, the request fails and
                // PiCMOpenClassKey closes the handle it made, so it is checked before any key is opened here.
                if (c.output_buffer % sizeof(ULONG) != 0)
                {
                    return STATUS_DATATYPE_MISALIGNMENT;
                }
                if (!is_writable_range(win_emu.memory, c.output_buffer, c.output_buffer_length))
                {
                    return STATUS_ACCESS_VIOLATION;
                }

                handle_result result{.size = sizeof(handle_result), .status = STATUS_INVALID_PARAMETER, .handle = 0};
                const bool well_formed = input.reserved1 == 0 && input.reserved3 == 0 &&
                                         (input.key_type == installer_class || input.key_type == interface_class);
                if (well_formed && request.class_name && request.class_name->size() > max_class_name_length)
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

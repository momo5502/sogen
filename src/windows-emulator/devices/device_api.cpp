#include "../std_include.hpp"
#include "device_api.hpp"

#include "../windows_emulator.hpp"
#include "../syscall_utils.hpp"
#include "../token.hpp"

namespace sogen
{
    namespace
    {
        // \Device\DeviceApi\CMApi, which cfgmgr32 asks for device and interface lists, class keys and properties. Requests
        // it does not answer yet succeed without data.
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

            // PnpStringFromGuid: the class's key name under Control\DeviceClasses.
            static std::u16string device_class_key_name(const std::array<uint8_t, 16>& guid)
            {
                uint32_t data1{};
                uint16_t data2{};
                uint16_t data3{};
                memcpy(&data1, guid.data(), sizeof(data1));
                memcpy(&data2, guid.data() + 4, sizeof(data2));
                memcpy(&data3, guid.data() + 6, sizeof(data3));

                std::array<char, 39> name{};
                snprintf(name.data(), name.size(), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", data1, data2, data3, guid[8],
                         guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
                return u8_to_u16(name.data());
            }

            // CmDeviceClassesSubkeyCallback: each "##?#<device path>" key under the class key is an interface key, and each
            // of its "#<reference string>" subkeys an interface. Its link swaps "##?#" for "\\?\" and appends
            // "\<reference string>" when that is not empty.
            static std::optional<std::u16string> device_interface_link(const std::u16string_view interface_key,
                                                                       const std::u16string_view reference_key)
            {
                if (interface_key.size() <= 4 || !interface_key.starts_with(u"##?#") || !reference_key.starts_with(u"#"))
                {
                    return std::nullopt;
                }

                std::u16string link{u"\\\\?\\"};
                link += interface_key.substr(4);
                if (reference_key.size() > 1)
                {
                    link += u'\\';
                    link += reference_key.substr(1);
                }

                return link;
            }

            // DEVPKEY_DeviceInterfaceClass_DefaultInterface in the class key's property store, when the class names one.
            static std::optional<std::u16string> default_device_interface(windows_emulator& win_emu, const std::u16string& class_path)
            {
                constexpr uint32_t devprop_type_string = 0x12;
                const auto key = win_emu.registry.get_key({class_path + u"\\Properties\\{14C83A99-0B3F-44B7-BE4C-A178D3990564}\\0002"});
                const auto value = key ? win_emu.registry.get_value(*key, "") : std::nullopt;
                if (!value || (value->type & 0xFFFF) != devprop_type_string || value->data.size() < sizeof(char16_t))
                {
                    return std::nullopt;
                }

                std::u16string link(value->data.size() / sizeof(char16_t), u'\0');
                memcpy(link.data(), value->data.data(), link.size() * sizeof(char16_t));
                if (const auto end = link.find(u'\0'); end != std::u16string::npos)
                {
                    link.resize(end);
                }

                return link.empty() ? std::nullopt : std::optional{std::move(link)};
            }

            // IopGetDeviceInterfaces for a user-mode caller: every interface of the class that matches, as NUL-terminated
            // links with the class's default interface first, ending in an empty string. A device ID keeps the interfaces
            // whose DeviceInstance names it. A present-only query keeps the interfaces the PnP manager has enabled
            // (PiControlGetDeviceInterfaceEnabled); the emulator starts no devices, so it has enabled none.
            static std::u16string device_interface_list(windows_emulator& win_emu, const std::array<uint8_t, 16>& interface_class,
                                                        const std::u16string_view device_id, const bool present_only)
            {
                std::vector<std::u16string> links;
                const auto class_path =
                    u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\DeviceClasses\\" + device_class_key_name(interface_class);
                const auto class_key = present_only ? std::nullopt : win_emu.registry.get_key({class_path});
                for (size_t index = 0; class_key && index < win_emu.registry.get_sub_key_count(*class_key); ++index)
                {
                    const auto interface_name = win_emu.registry.get_sub_key_name(*class_key, index);
                    if (!interface_name || !interface_name->starts_with("##?#"))
                    {
                        continue;
                    }

                    const auto interface_key_name = u8_to_u16(*interface_name);
                    const auto interface_key = win_emu.registry.get_sub_key(*class_key, interface_key_name);
                    if (!interface_key)
                    {
                        continue;
                    }

                    if (!device_id.empty())
                    {
                        const auto instance = win_emu.registry.get_value(*interface_key, "DeviceInstance");
                        const auto instance_id = instance ? instance->as_string() : std::nullopt;
                        if (!instance_id || !utils::string::equals_ignore_case(std::u16string_view{*instance_id}, device_id))
                        {
                            continue;
                        }
                    }

                    for (size_t reference = 0; reference < win_emu.registry.get_sub_key_count(*interface_key); ++reference)
                    {
                        const auto reference_name = win_emu.registry.get_sub_key_name(*interface_key, reference);
                        if (auto link =
                                reference_name ? device_interface_link(interface_key_name, u8_to_u16(*reference_name)) : std::nullopt)
                        {
                            links.push_back(std::move(*link));
                        }
                    }
                }

                if (const auto default_link = class_key ? default_device_interface(win_emu, class_path) : std::nullopt)
                {
                    const auto match = std::ranges::find_if(
                        links, [&](const std::u16string& link) { return utils::string::equals_ignore_case(link, *default_link); });
                    if (match != links.end())
                    {
                        std::rotate(links.begin(), match, match + 1);
                    }
                }

                std::u16string list;
                for (const auto& link : links)
                {
                    list += link;
                    list += u'\0';
                }

                list += u'\0';
                return list;
            }

            // PiCMGetDeviceInterfaceList. A size query's 20-byte buffer has no room for the list, so it learns the size from
            // a STATUS_BUFFER_TOO_SMALL reply.
            static NTSTATUS get_device_interface_list(windows_emulator& win_emu, const io_device_context& c)
            {
                constexpr uint32_t present_only = 0x10000;

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

                const auto list = device_interface_list(win_emu, input.interface_class, device_id, (input.flags & present_only) != 0);
                const std::span list_bytes{reinterpret_cast<const uint8_t*>(list.data()), list.size() * sizeof(char16_t)};
                const auto list_size = static_cast<uint32_t>(list_bytes.size());
                if (c.output_buffer_length - sizeof(buffer_result) < list_size)
                {
                    return return_buffer_result(win_emu, c, STATUS_BUFFER_TOO_SMALL, list_size, {}, input.result_size);
                }

                return return_buffer_result(win_emu, c, STATUS_SUCCESS, list_size, list_bytes, input.result_size);
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

            // PnpIsValidGuidString, which CmValidateInstallerClassName applies to every class name: the 38 characters
            // RtlGUIDFromString reads, hex digits in either case.
            static bool is_guid_string(const std::u16string_view name)
            {
                constexpr std::u16string_view layout = u"{00000000-0000-0000-0000-000000000000}";
                const auto is_hex_digit = [](const char16_t c) {
                    return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
                };

                return name.size() == layout.size() && std::ranges::equal(name, layout, [&](const char16_t c, const char16_t expected) {
                           return expected == u'0' ? is_hex_digit(c) : c == expected;
                       });
            }

            // PiCMOpenClassKey: opens Control\Class for installer classes or Control\DeviceClasses for interface classes,
            // or the class's {GUID} key under it, which it creates for a caller with PnP write access that opens a missing
            // class always, and hands the caller its own handle. The reply carries the open's status, so the ioctl itself
            // succeeds whenever the request is well formed and the reply fits.
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
                else if (well_formed && request.class_name && !is_guid_string(*request.class_name))
                {
                    result.status = STATUS_OBJECT_NAME_INVALID;
                }
                else if (well_formed)
                {
                    std::u16string path = input.key_type == interface_class
                                              ? u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\DeviceClasses"
                                              : u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class";
                    if (request.class_name)
                    {
                        // PiCMOpenClassKey upcases the name, so a class key it creates has an upper-case name.
                        path += u'\\';
                        std::ranges::transform(*request.class_name, std::back_inserter(path), [](const char16_t ch) {
                            return ch >= u'a' && ch <= u'z' ? static_cast<char16_t>(ch - u'a' + u'A') : ch;
                        });
                    }

                    auto key = win_emu.registry.get_key({path});
                    const auto creates = !key && request.class_name && input.disposition == open_always;
                    if (creates && has_pnp_write_access(win_emu, caller_token(c)))
                    {
                        // The class comes from ZwCreateKey (_RegRtlCreateKeyTransacted), which tells the watchers of its
                        // parent as NtCreateKey does.
                        key = win_emu.registry.create_key(path);
                        auto& vcpu = c.vcpu ? *c.vcpu : win_emu.vcpu(0);
                        const syscall_context call{.win_emu = win_emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = win_emu.process};
                        if (key && registry_notification::has_watchers(call))
                        {
                            registry_notification::report_sub_key_change(call, *key);
                        }
                    }

                    if (key)
                    {
                        result.status = STATUS_SUCCESS;
                        result.handle = win_emu.process.registry_keys.store(std::move(*key)).bits;
                    }
                    else
                    {
                        result.status = creates ? STATUS_ACCESS_DENIED : STATUS_OBJECT_NAME_NOT_FOUND;
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

            // The device ID list request CM_Get_Device_ID_List and its size query send. WoW64 callers send a 20-byte layout
            // with a 32-bit filter pointer, which PiCMCaptureDeviceListInputData widens to this one.
            struct device_list_input
            {
                uint32_t size;
                uint32_t flags;
                uint64_t filter;
                uint32_t filter_bytes;
                uint32_t result_size;
            };

            static_assert(sizeof(device_list_input) == 24);

            struct device_list_input32
            {
                uint32_t size;
                uint32_t flags;
                uint32_t filter;
                uint32_t filter_bytes;
                uint32_t result_size;
            };

            static_assert(sizeof(device_list_input32) == 20);

            // The flags PiCMConvertDeviceListFilters takes: at most one filter in the low word, which says what the filter
            // string names.
            static constexpr uint32_t device_list_enumerator = 0x1;
            static constexpr uint32_t device_list_service = 0x2;
            static constexpr uint32_t device_list_relations = 0x4 | 0x8 | 0x10 | 0x20 | 0x40; // ejection, removal, power, bus, transport
            static constexpr uint32_t device_list_class = 0x80;
            static constexpr uint32_t device_list_filters = 0xFFFF;
            static constexpr uint32_t device_list_present_only = 0x10000;
            // CMP_Get_Device_ID_List's request for the list without the mandatory filters.
            static constexpr uint32_t device_list_unfiltered = 0x20000;

            static constexpr std::u16string_view enum_key = u"\\Registry\\Machine\\System\\CurrentControlSet\\Enum";
            static constexpr std::u16string_view installer_classes_key = u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class";

            // MAX_DEVICE_ID_LEN: the characters a device ID, and the enumerator filter's copy, may take with a terminator.
            static constexpr size_t max_device_id_length = 200;

            // CmGetDeviceRegProp reads CM_DRP_CLASSGUID into a 78-byte buffer: a braced GUID and its terminator.
            static constexpr size_t class_guid_length = 38;

            // DEVPKEY_Device_SessionId and DEVPKEY_Device_RestrictedSD as keys of the device's property store, whose values
            // keep 0xFFFF0000 | DEVPROPTYPE as their registry type.
            static constexpr std::u16string_view session_id_property = u"{83da6326-97a6-4088-9453-a1923f573b29}\\0006";
            static constexpr std::u16string_view restricted_sd_property = u"{8c439ff0-9cf7-43cd-961e-9299a4c6c157}\\0064";
            static constexpr uint32_t devprop_type_mask = 0xFFFF;
            static constexpr uint32_t devprop_type_uint32 = 0x7;
            static constexpr uint32_t devprop_type_security_descriptor = 0x13;

            // The access PiPnpRtlIsDeviceEnumerableForUser asks of DEVPKEY_Device_RestrictedSD.
            static constexpr ACCESS_MASK enumerate_device_access = 0x1;

            static_assert(token_session_id != 0, "PiPnpRtlApplyMandatoryFilters checks session 0 callers another way");

            // PiCMCaptureDeviceListInputData. The filter is captured like a class name, aligned to a character.
            static NTSTATUS capture_device_list_input(windows_emulator& win_emu, const io_device_context& c, device_list_input& input,
                                                      std::optional<std::u16string>& filter)
            {
                device_list_input32 input32{};
                if (const auto status = read_request(win_emu, c, input, input32); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (win_emu.process.is_wow64_process)
                {
                    input = {
                        .size = sizeof(input),
                        .flags = input32.flags,
                        .filter = input32.filter,
                        .filter_bytes = input32.filter_bytes,
                        .result_size = input32.result_size,
                    };
                }

                if (input.filter ? input.filter_bytes < sizeof(char16_t) : input.filter_bytes != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                return input.filter ? capture_name(win_emu, input.filter, input.filter_bytes, filter.emplace()) : STATUS_SUCCESS;
            }

            static bool is_device_list_request(const uint32_t flags)
            {
                const auto filter = flags & device_list_filters;
                return (flags & ~(device_list_filters | device_list_present_only | device_list_unfiltered)) == 0 &&
                       (filter == 0 || (std::has_single_bit(filter) && filter <= device_list_class));
            }

            // SeCaptureSubjectContext: the issuing thread's impersonation token, else the process token.
            static handle caller_token(const io_device_context& c)
            {
                const auto* thread = c.vcpu ? c.vcpu->active_thread : nullptr;
                return thread && thread->impersonation_token.bits != 0 ? thread->impersonation_token : CURRENT_PROCESS_TOKEN;
            }

            // PiAuDoesClientHaveAccess(2), PnP write access: PiAuSecurityObject allows that access to Administrators, then
            // denies NETWORK all access, then allows LocalSystem all access.
            static bool has_pnp_write_access(const windows_emulator& win_emu, const handle token)
            {
                return token_holds_sid(win_emu, token, administrators_sid) ||
                       (!token_holds_sid(win_emu, token, network_sid) && token_holds_sid(win_emu, token, local_system_sid));
            }

            static std::u16string sub_key_path(const std::u16string_view parent, const std::u16string_view name)
            {
                std::u16string path{parent};
                path += u'\\';
                path += name;
                return path;
            }

            static std::vector<std::u16string> sub_key_names(windows_emulator& win_emu, const registry_key& key)
            {
                std::vector<std::u16string> names{};
                for (size_t index = 0; index < win_emu.registry.get_sub_key_count(key); ++index)
                {
                    if (const auto name = win_emu.registry.get_sub_key_name(key, index))
                    {
                        names.push_back(u8_to_u16(*name));
                    }
                }

                return names;
            }

            // CmValidateDeviceName allows the characters from 0x21 to 0x7F but ',' in each part of a device ID.
            static bool is_device_id_part(const std::u16string_view part)
            {
                return !part.empty() &&
                       std::ranges::all_of(part, [](const char16_t c) { return c >= u'!' && c <= u'\x7F' && c != u',' && c != u'\\'; });
            }

            struct registered_device
            {
                std::u16string id;
                std::optional<registry_key> key;
            };

            // The devices the PnP manager knows whose IDs start with the prefix, ignoring case: each
            // Enum\<enumerator>\<device>\<instance> key that names a valid device ID.
            static std::vector<registered_device> registered_devices(windows_emulator& win_emu, const std::u16string& prefix)
            {
                const auto may_lead_to_prefix = [&](const std::u16string& id_start) {
                    return id_start.size() < prefix.size() ? utils::string::starts_with_ignore_case(prefix, id_start)
                                                           : utils::string::starts_with_ignore_case(id_start, prefix);
                };

                std::vector<registered_device> devices{};
                const auto enum_root = win_emu.registry.get_key({std::u16string{enum_key}});
                if (!enum_root)
                {
                    return devices;
                }

                for (const auto& enumerator : sub_key_names(win_emu, *enum_root))
                {
                    if (!may_lead_to_prefix(sub_key_path(enumerator, {})))
                    {
                        continue;
                    }

                    const auto enumerator_key = win_emu.registry.get_sub_key(*enum_root, enumerator);
                    if (!enumerator_key)
                    {
                        continue;
                    }

                    for (const auto& device : sub_key_names(win_emu, *enumerator_key))
                    {
                        const auto device_id = sub_key_path(enumerator, device);
                        if (!may_lead_to_prefix(sub_key_path(device_id, {})))
                        {
                            continue;
                        }

                        const auto device_key = win_emu.registry.get_sub_key(*enumerator_key, device);
                        if (!device_key)
                        {
                            continue;
                        }

                        for (const auto& instance : sub_key_names(win_emu, *device_key))
                        {
                            auto id = sub_key_path(device_id, instance);
                            if (id.size() < max_device_id_length && is_device_id_part(enumerator) && is_device_id_part(device) &&
                                is_device_id_part(instance) && utils::string::starts_with_ignore_case(id, prefix))
                            {
                                devices.push_back({.id = std::move(id), .key = win_emu.registry.get_sub_key(*device_key, instance)});
                            }
                        }
                    }
                }

                return devices;
            }

            static std::u16string_view value_characters(const registry_value& value)
            {
                return {reinterpret_cast<const char16_t*>(value.data.data()), value.data.size() / sizeof(char16_t)};
            }

            // CmGetDeviceRegProp hands out CM_DRP_SERVICE with its last character made a terminator.
            static bool names_service(const std::optional<registry_value>& value, const std::u16string_view service)
            {
                if (!value || value->data.size() < sizeof(char16_t))
                {
                    return false;
                }

                auto name = value_characters(*value);
                name.remove_suffix(1);
                return utils::string::equals_ignore_case(name.substr(0, name.find(u'\0')), service);
            }

            // A REG_MULTI_SZ filter list as CmGetDeviceRegProp hands it out, with its last two characters made terminators.
            static bool lists_service(const std::optional<registry_value>& value, const std::u16string_view service)
            {
                if (!value || value->type != REG_MULTI_SZ || value->data.size() < sizeof(char16_t))
                {
                    return false;
                }

                std::u16string list{value_characters(*value)};
                if (value->data.size() > sizeof(char16_t))
                {
                    list[list.size() - 1] = u'\0';
                }

                if (value->data.size() > 2 * sizeof(char16_t))
                {
                    list[list.size() - 2] = u'\0';
                }

                for (size_t start = 0; start < list.size() && list[start] != u'\0';)
                {
                    const auto end = std::min(list.find(u'\0', start), list.size());
                    if (utils::string::equals_ignore_case(std::u16string_view{list}.substr(start, end - start), service))
                    {
                        return true;
                    }

                    start = end + 1;
                }

                return false;
            }

            static std::optional<std::u16string> device_class_guid(windows_emulator& win_emu, const registry_key& device_key)
            {
                const auto value = win_emu.registry.get_value(device_key, "ClassGUID");
                if (!value || value->type != REG_SZ || value->data.size() < sizeof(char16_t) ||
                    value->data.size() > (class_guid_length + 1) * sizeof(char16_t))
                {
                    return std::nullopt;
                }

                const auto guid = value_characters(*value).substr(0, class_guid_length);
                return std::u16string{guid.substr(0, guid.find(u'\0'))};
            }

            // PiPnpRtlServiceFilterCallback: the device's function driver, or a filter driver of the device or of its class.
            // The compound filter lists it reads also merge filters declared with INF AddFilter directives, which are not
            // modelled.
            static bool uses_service(windows_emulator& win_emu, const registry_key& device_key,
                                     const std::optional<registry_key>& installer_classes, const std::u16string_view service)
            {
                if (names_service(win_emu.registry.get_value(device_key, "Service"), service) ||
                    lists_service(win_emu.registry.get_value(device_key, "UpperFilters"), service) ||
                    lists_service(win_emu.registry.get_value(device_key, "LowerFilters"), service))
                {
                    return true;
                }

                const auto class_guid = device_class_guid(win_emu, device_key);
                const auto installer_class =
                    class_guid && installer_classes ? win_emu.registry.get_sub_key(*installer_classes, *class_guid) : std::nullopt;
                return installer_class && (lists_service(win_emu.registry.get_value(*installer_class, "UpperFilters"), service) ||
                                           lists_service(win_emu.registry.get_value(*installer_class, "LowerFilters"), service));
            }

            static std::optional<registry_value> device_property(windows_emulator& win_emu, const std::optional<registry_key>& device_key,
                                                                 const std::u16string_view property)
            {
                const auto key =
                    device_key ? win_emu.registry.get_sub_key(*device_key, sub_key_path(u"Properties", property)) : std::nullopt;
                return key ? win_emu.registry.get_value(*key, "") : std::nullopt;
            }

            // PiPnpRtlApplyMandatoryDeviceFilters: a device stored with another session's DEVPKEY_Device_SessionId, or with a
            // DEVPKEY_Device_RestrictedSD that does not let the caller read it, is left out. PnpGetObjectProperty reads the
            // session into four bytes and PnpValidatePropertyData wants exactly four for a UINT32, and a property that
            // cannot be read leaves the device out too.
            static bool is_enumerable_for_caller(windows_emulator& win_emu, const std::optional<registry_key>& device_key,
                                                 const handle token)
            {
                if (const auto session = device_property(win_emu, device_key, session_id_property))
                {
                    if (session->data.size() > sizeof(uint32_t))
                    {
                        return false;
                    }

                    if ((session->type & devprop_type_mask) == devprop_type_uint32)
                    {
                        uint32_t session_id{};
                        if (session->data.size() != sizeof(session_id))
                        {
                            return false;
                        }

                        memcpy(&session_id, session->data.data(), sizeof(session_id));
                        if (session_id != token_session_id)
                        {
                            return false;
                        }
                    }
                }

                const auto restricted = device_property(win_emu, device_key, restricted_sd_property);
                return !restricted || (restricted->type & devprop_type_mask) != devprop_type_security_descriptor ||
                       token_grants_access(win_emu, token, restricted->data, enumerate_device_access);
            }

            // PiPnpRtlGetFilteredDeviceList's enumerator filter: an enumerator, optionally with a device, which it copies
            // with a separator into a MAX_DEVICE_ID_LEN buffer to match device IDs by prefix.
            static NTSTATUS enumerator_prefix(const std::u16string& filter, std::u16string& prefix)
            {
                if (filter.empty() || std::ranges::count(filter, u'\\') > 1 || filter.front() == u'\\' || filter.back() == u'\\')
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (filter.size() >= max_device_id_length)
                {
                    return STATUS_BUFFER_OVERFLOW;
                }

                if (filter.size() + 1 >= max_device_id_length)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                prefix = filter + u'\\';
                return STATUS_SUCCESS;
            }

            // RtlHashUnicodeString(id, TRUE, HASH_STRING_ALGORITHM_X65599). Device IDs hold ASCII characters only.
            static uint32_t device_id_hash(const std::u16string_view id)
            {
                constexpr uint32_t x65599_multiplier = 65599;
                uint32_t hash = 0;
                for (const auto character : id)
                {
                    const auto upper = character >= u'a' && character <= u'z' ? character - (u'a' - u'A') : character;
                    hash = (hash * x65599_multiplier) + static_cast<uint32_t>(upper);
                }

                return hash;
            }

            // The lists come from the PnP manager's device cache, which PiDmCompareObjects orders by device_id_hash, then by
            // _wcsicmp.
            static void sort_as_device_cache(std::vector<std::u16string>& ids)
            {
                std::vector<std::pair<uint32_t, std::u16string>> keyed{};
                keyed.reserve(ids.size());
                for (auto& id : ids)
                {
                    keyed.emplace_back(device_id_hash(id), std::move(id));
                }

                std::ranges::sort(keyed, [](const auto& left, const auto& right) {
                    return left.first != right.first ? left.first < right.first
                                                     : utils::string::compare_ignore_case(left.second, right.second) < 0;
                });

                ids.clear();
                for (auto& [hash, id] : keyed)
                {
                    ids.push_back(std::move(id));
                }
            }

            // PiPnpRtlGetFilteredDeviceList and the mandatory filter PiCMMandatoryFilterCallback adds, or the status that
            // fails the request. Relations and presence come from devnodes, and the emulator starts no devices.
            static NTSTATUS list_device_ids(windows_emulator& win_emu, const io_device_context& c, const uint32_t flags,
                                            const std::optional<std::u16string>& filter, std::vector<std::u16string>& ids)
            {
                const auto kind = flags & device_list_filters;
                if (kind != 0 && !filter)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if ((kind & device_list_relations) != 0)
                {
                    return filter->size() > max_name_length ? STATUS_NAME_TOO_LONG : STATUS_OBJECT_NAME_NOT_FOUND;
                }

                std::u16string prefix{};
                if (kind == device_list_enumerator)
                {
                    if (const auto status = enumerator_prefix(*filter, prefix); status != STATUS_SUCCESS)
                    {
                        return status;
                    }
                }
                else if (kind == device_list_service && filter->size() > max_name_length)
                {
                    return STATUS_NAME_TOO_LONG;
                }

                if ((flags & device_list_present_only) != 0)
                {
                    return STATUS_SUCCESS;
                }

                const auto installer_classes =
                    kind == device_list_service ? win_emu.registry.get_key({std::u16string{installer_classes_key}}) : std::nullopt;
                const auto matches = [&](const std::optional<registry_key>& device_key) {
                    switch (kind)
                    {
                    case device_list_service:
                        return filter->empty() || (device_key && uses_service(win_emu, *device_key, installer_classes, *filter));
                    case device_list_class: {
                        if (filter->empty())
                        {
                            return false;
                        }

                        const auto class_guid = device_key ? device_class_guid(win_emu, *device_key) : std::nullopt;
                        return class_guid && utils::string::equals_ignore_case(*class_guid, *filter);
                    }
                    default:
                        return true;
                    }
                };

                const auto token = caller_token(c);
                const auto unfiltered = (flags & device_list_unfiltered) != 0;
                for (auto& device : registered_devices(win_emu, prefix))
                {
                    if (matches(device.key) && (unfiltered || is_enumerable_for_caller(win_emu, device.key, token)))
                    {
                        ids.push_back(std::move(device.id));
                    }
                }

                sort_as_device_cache(ids);
                return STATUS_SUCCESS;
            }

            // PiCMGetDeviceIdList. A request that fails once its flags are read reports the room its reply had for the list
            // as the list's size. An empty list is a lone terminator, which a size query's 20-byte buffer has no room for.
            static NTSTATUS get_device_id_list(windows_emulator& win_emu, const io_device_context& c)
            {
                device_list_input input{};
                std::optional<std::u16string> filter{};
                if (const auto status = capture_device_list_input(win_emu, c, input, filter); status != STATUS_SUCCESS)
                {
                    return status;
                }

                if (!c.output_buffer || c.output_buffer_length < sizeof(buffer_result))
                {
                    return return_buffer_result(win_emu, c, STATUS_INVALID_PARAMETER, 0, {}, input.result_size);
                }

                if ((input.flags & device_list_unfiltered) != 0 && !has_pnp_write_access(win_emu, caller_token(c)))
                {
                    return return_buffer_result(win_emu, c, STATUS_ACCESS_DENIED, 0, {}, input.result_size);
                }

                const auto room = c.output_buffer_length - static_cast<uint32_t>(sizeof(buffer_result));
                const auto room_bytes = static_cast<uint32_t>(room < sizeof(char16_t) ? 0 : room - (room % sizeof(char16_t)));

                std::vector<std::u16string> ids{};
                const auto status =
                    is_device_list_request(input.flags) ? list_device_ids(win_emu, c, input.flags, filter, ids) : STATUS_INVALID_PARAMETER;
                if (status != STATUS_SUCCESS)
                {
                    return return_buffer_result(win_emu, c, status, room_bytes, {}, input.result_size);
                }

                std::u16string list{};
                for (const auto& id : ids)
                {
                    list += id;
                    list += u'\0';
                }

                list += u'\0';
                const std::span list_bytes{reinterpret_cast<const uint8_t*>(list.data()), list.size() * sizeof(char16_t)};
                const auto list_size = static_cast<uint32_t>(list_bytes.size());
                if (list_size > room_bytes)
                {
                    return return_buffer_result(win_emu, c, STATUS_BUFFER_TOO_SMALL, list_size, {}, input.result_size);
                }

                return return_buffer_result(win_emu, c, STATUS_SUCCESS, list_size, list_bytes, input.result_size);
            }
        };
    }

    std::unique_ptr<io_device> create_device_api_device(const device_creation_context&)
    {
        return std::make_unique<device_api_device>();
    }
}

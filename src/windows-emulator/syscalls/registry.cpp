#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../cpu_context.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtOpenKey(const syscall_context& c, const emulator_object<handle> key_handle, const ACCESS_MASK /*desired_access*/,
                              const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();
        auto key = read_unicode_string(c.emu, attributes.ObjectName);

        if (attributes.RootDirectory)
        {
            const auto* parent_handle = c.proc.registry_keys.get(attributes.RootDirectory);
            if (!parent_handle)
            {
                return STATUS_INVALID_HANDLE;
            }

            const std::filesystem::path full_path = parent_handle->hive.get() / parent_handle->path.get() / key;
            key = full_path.u16string();
        }

        c.win_emu.callbacks.on_generic_access("Registry key", key);

        auto entry = c.win_emu.registry.get_key({key});
        if (!entry.has_value())
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        const auto handle = c.proc.registry_keys.store(std::move(entry.value()));
        key_handle.write(handle);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenKeyEx(const syscall_context& c, const emulator_object<handle> key_handle, const ACCESS_MASK desired_access,
                                const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG /*open_options*/)
    {
        return handle_NtOpenKey(c, key_handle, desired_access, object_attributes);
    }

    NTSTATUS handle_NtQueryKey(const syscall_context& c, const handle key_handle, const KEY_INFORMATION_CLASS key_information_class,
                               const uint64_t key_information, const ULONG length, const emulator_object<ULONG> result_length)
    {
        const auto* key = c.proc.registry_keys.get(key_handle);
        if (!key)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (key_information_class == KeyNameInformation)
        {
            auto key_name = (key->hive.get() / key->path.get()).u16string();
            while (key_name.ends_with(u'/') || key_name.ends_with(u'\\'))
            {
                key_name.pop_back();
            }

            std::ranges::transform(key_name, key_name.begin(), std::towupper);

            const auto required_size = sizeof(KEY_NAME_INFORMATION) + (key_name.size() * 2) - 1;
            result_length.write(static_cast<ULONG>(required_size));

            if (required_size > length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            KEY_NAME_INFORMATION info{};
            info.NameLength = static_cast<ULONG>(key_name.size() * 2);

            const emulator_object<KEY_NAME_INFORMATION> info_obj{c.emu, key_information};
            info_obj.write(info);

            c.emu.write_memory(key_information + offsetof(KEY_NAME_INFORMATION, Name), key_name.data(), info.NameLength);

            return STATUS_SUCCESS;
        }

        if (key_information_class == KeyFullInformation)
        {
            c.win_emu.log.warn("Unsupported registry class: %X\n", key_information_class);
            return STATUS_NOT_SUPPORTED;
        }

        if (key_information_class == KeyCachedInformation)
        {
            auto key_name = (key->hive.get() / key->path.get()).u16string();
            while (key_name.ends_with(u'/') || key_name.ends_with(u'\\'))
            {
                key_name.pop_back();
            }

            const auto hive_key = c.win_emu.registry.get_hive_key(*key);
            if (!hive_key.has_value())
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            constexpr auto required_size = sizeof(KEY_CACHED_INFORMATION);
            result_length.write(required_size);

            if (required_size > length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            KEY_CACHED_INFORMATION info{};
            info.SubKeys = static_cast<ULONG>(hive_key->key.get_sub_key_count(hive_key->file));
            info.Values = static_cast<ULONG>(hive_key->key.get_value_count(hive_key->file));
            info.NameLength = static_cast<ULONG>(key_name.size() * 2);
            info.MaxValueDataLen = 0x1000;
            info.MaxValueNameLen = 0x1000;
            info.MaxNameLen = 0x1000;

            c.emu.write_memory(key_information, info);
            return STATUS_SUCCESS;
        }

        if (key_information_class == KeyHandleTagsInformation)
        {
            constexpr auto required_size = sizeof(KEY_HANDLE_TAGS_INFORMATION);
            result_length.write(required_size);

            if (required_size > length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            KEY_HANDLE_TAGS_INFORMATION info{};
            info.HandleTags = 0; // ?

            const emulator_object<KEY_HANDLE_TAGS_INFORMATION> info_obj{c.emu, key_information};
            info_obj.write(info);

            return STATUS_SUCCESS;
        }

        c.win_emu.log.warn("Unsupported registry class: %X\n", key_information_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryValueKey(const syscall_context& c, const handle key_handle,
                                    const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name,
                                    const KEY_VALUE_INFORMATION_CLASS key_value_information_class, const uint64_t key_value_information,
                                    const ULONG length, const emulator_object<ULONG> result_length)
    {
        const auto* key = c.proc.registry_keys.get(key_handle);
        if (!key)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto query_name = read_unicode_string(c.emu, value_name);

        if (c.win_emu.callbacks.on_generic_access)
        {
            // TODO: Find a better way to log this
            c.win_emu.callbacks.on_generic_access("Querying value key", query_name + u" (" + key->to_string() + u")");
        }

        const auto value = c.win_emu.registry.get_value(*key, u16_to_u8(query_name));
        if (!value)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        const std::u16string original_name(value->name.begin(), value->name.end());

        if (key_value_information_class == KeyValueBasicInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_BASIC_INFORMATION, Name);
            const auto required_size = base_size + (original_name.size() * 2) - 1;
            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_BASIC_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.NameLength = static_cast<ULONG>(original_name.size() * 2);

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, original_name.data(), info.NameLength);

            return STATUS_SUCCESS;
        }

        if (key_value_information_class == KeyValuePartialInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data);
            const auto required_size = base_size + value->data.size();
            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_PARTIAL_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.DataLength = static_cast<ULONG>(value->data.size());

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, value->data.data(), value->data.size());

            return STATUS_SUCCESS;
        }

        if (key_value_information_class == KeyValueFullInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_FULL_INFORMATION, Name);
            const auto name_size = original_name.size() * 2;
            const auto value_size = value->data.size();
            const auto required_size = base_size + name_size + value_size + -1;
            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_FULL_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.DataLength = static_cast<ULONG>(value->data.size());
            info.NameLength = static_cast<ULONG>(original_name.size() * 2);

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, original_name.data(), info.NameLength);

            c.emu.write_memory(key_value_information + base_size + info.NameLength, value->data.data(), value->data.size());

            return STATUS_SUCCESS;
        }

        c.win_emu.log.warn("Unsupported registry value class: %X\n", key_value_information_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryMultipleValueKey(const syscall_context& c, const handle key_handle,
                                            const emulator_object<KEY_VALUE_ENTRY> value_entries, const ULONG entry_count,
                                            const uint64_t value_buffer, const emulator_object<ULONG> buffer_length,
                                            const emulator_object<ULONG> required_buffer_length)
    {
        if (entry_count > 0x10000)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        const auto* key = c.proc.registry_keys.get(key_handle);
        if (!key)
        {
            return STATUS_INVALID_HANDLE;
        }

        NTSTATUS status = STATUS_SUCCESS;
        auto remaining_length = buffer_length.read();
        ULONG required_length = 0;
        ULONG written_bytes = 0;

        for (ULONG i = 0; i < entry_count; i++)
        {
            auto entry = value_entries.read(i);
            if (!entry.ValueName)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            const auto query_name = read_unicode_string(c.emu, entry.ValueName);

            if (c.win_emu.callbacks.on_generic_access)
            {
                // TODO: Find a better way to log this
                c.win_emu.callbacks.on_generic_access("Querying multiple value key ", query_name + u" (" + key->to_string() + u")");
            }

            const auto value = c.win_emu.registry.get_value(*key, u16_to_u8(query_name));
            if (!value)
            {
                status = STATUS_OBJECT_NAME_NOT_FOUND;
                break;
            }

            const auto data_length = static_cast<ULONG>(value->data.size());

            if (status == STATUS_SUCCESS)
            {
                if (remaining_length >= data_length)
                {
                    entry.DataOffset = written_bytes;
                    entry.DataLength = data_length;
                    entry.Type = value->type;

                    c.emu.write_memory(value_buffer + entry.DataOffset, value->data.data(), entry.DataLength);
                    value_entries.write(entry, i);

                    remaining_length -= data_length;
                    written_bytes += data_length;
                }
                else
                {
                    status = STATUS_BUFFER_OVERFLOW;
                }
            }

            required_length += data_length;
        }

        buffer_length.write(written_bytes);

        if (required_buffer_length.value())
        {
            required_buffer_length.write(required_length);
        }

        return status;
    }

    NTSTATUS handle_NtCreateKey(const syscall_context& c, const emulator_object<handle> key_handle, const ACCESS_MASK desired_access,
                                const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                const ULONG /*title_index*/, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*class*/,
                                const ULONG /*create_options*/, const emulator_object<ULONG> /*disposition*/)
    {
        const auto result = handle_NtOpenKey(c, key_handle, desired_access, object_attributes);

        if (result == STATUS_OBJECT_NAME_NOT_FOUND)
        {
            return STATUS_NOT_SUPPORTED;
        }

        return result;
    }

    NTSTATUS handle_NtSetValueKey(const syscall_context& c, const handle key_handle,
                                  const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name, const ULONG /*title_index*/,
                                  const ULONG type, const uint64_t data, const ULONG data_size)
    {
        const auto* key = c.proc.registry_keys.get(key_handle);
        if (!key)
        {
            return STATUS_INVALID_HANDLE;
        }

        std::vector<std::byte> data_buffer{};
        data_buffer.resize(data_size);

        if (data_size > 0)
        {
            if (!data || !c.emu.try_read_memory(data, data_buffer.data(), data_size))
            {
                return STATUS_ACCESS_VIOLATION;
            }
        }

        std::string name{};
        if (value_name.value())
        {
            name = u16_to_u8(read_unicode_string(c.emu, value_name));
        }

        c.win_emu.registry.set_value(*key, std::move(name), type, std::span<const std::byte>{data_buffer});

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtNotifyChangeKey()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetInformationKey()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtEnumerateKey(const syscall_context& c, const handle key_handle, const ULONG index,
                                   const KEY_INFORMATION_CLASS key_information_class, const uint64_t key_information, const ULONG length,
                                   const emulator_object<ULONG> result_length)
    {
        const auto* key = c.proc.registry_keys.get(key_handle);
        if (!key)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto subkey_name = c.win_emu.registry.get_sub_key_name(*key, index);
        if (!subkey_name)
        {
            return STATUS_NO_MORE_ENTRIES;
        }

        const std::u16string subkey_name_u16(subkey_name->begin(), subkey_name->end());

        if (key_information_class == KeyBasicInformation)
        {
            constexpr auto base_size = offsetof(KEY_BASIC_INFORMATION, Name);
            const auto name_size = subkey_name_u16.size() * 2;
            const auto required_size = base_size + name_size;
            result_length.write(static_cast<ULONG>(required_size));

            KEY_BASIC_INFORMATION info{};
            info.LastWriteTime.QuadPart = 0;
            info.TitleIndex = 0;
            info.NameLength = static_cast<ULONG>(name_size);

            if (base_size <= length)
            {
                c.emu.write_memory(key_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_information + base_size, subkey_name_u16.data(), name_size);

            return STATUS_SUCCESS;
        }

        if (key_information_class == KeyNodeInformation)
        {
            constexpr auto base_size = offsetof(KEY_NODE_INFORMATION, Name);
            const auto name_size = subkey_name_u16.size() * 2;
            constexpr auto class_size = 0; // TODO: Class Name
            const auto required_size = base_size + name_size + class_size;
            result_length.write(static_cast<ULONG>(required_size));

            KEY_NODE_INFORMATION info{};
            info.LastWriteTime.QuadPart = 0;
            info.TitleIndex = 0;
            info.ClassOffset = static_cast<ULONG>(base_size + name_size);
            info.ClassLength = static_cast<ULONG>(class_size);
            info.NameLength = static_cast<ULONG>(name_size);

            if (base_size <= length)
            {
                c.emu.write_memory(key_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_information + base_size, subkey_name_u16.data(), name_size);
            // TODO: Write Class Name

            return STATUS_SUCCESS;
        }

        c.win_emu.log.warn("Unsupported registry enumeration class: %X\n", key_information_class);
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtEnumerateValueKey(const syscall_context& c, const handle key_handle, const ULONG index,
                                        const KEY_VALUE_INFORMATION_CLASS key_value_information_class, const uint64_t key_value_information,
                                        const ULONG length, const emulator_object<ULONG> result_length)
    {
        const auto* key = c.proc.registry_keys.get(key_handle);
        if (!key)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto value = c.win_emu.registry.get_value(*key, index);
        if (!value)
        {
            return STATUS_NO_MORE_ENTRIES;
        }

        const std::u16string value_name_u16(value->name.begin(), value->name.end());

        if (key_value_information_class == KeyValueBasicInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_BASIC_INFORMATION, Name);
            const auto name_size = value_name_u16.size() * 2;
            const auto required_size = base_size + name_size;
            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_BASIC_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.NameLength = static_cast<ULONG>(name_size);

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, value_name_u16.data(), name_size);

            return STATUS_SUCCESS;
        }

        if (key_value_information_class == KeyValuePartialInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data);
            const auto data_size = value->data.size();
            const auto required_size = base_size + data_size;
            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_PARTIAL_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.DataLength = static_cast<ULONG>(data_size);

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, value->data.data(), data_size);

            return STATUS_SUCCESS;
        }

        if (key_value_information_class == KeyValueFullInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_FULL_INFORMATION, Name);
            const auto name_size = value_name_u16.size() * 2;
            const auto data_size = value->data.size();
            const auto data_offset = static_cast<ULONG>(base_size + name_size);
            const auto required_size = data_offset + data_size;

            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_FULL_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.DataOffset = data_offset;
            info.DataLength = static_cast<ULONG>(data_size);
            info.NameLength = static_cast<ULONG>(name_size);

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, value_name_u16.data(), name_size);

            c.emu.write_memory(key_value_information + data_offset, value->data.data(), data_size);

            return STATUS_SUCCESS;
        }

        c.win_emu.log.warn("Unsupported registry value enumeration class: %X\n", key_value_information_class);
        return STATUS_NOT_SUPPORTED;
    }
}

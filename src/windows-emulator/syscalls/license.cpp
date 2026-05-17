#include "../std_include.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    namespace
    {
        constexpr std::u16string_view k_product_options_key = u"\\registry\\machine\\system\\CurrentControlSet\\Control\\ProductOptions";
        constexpr std::string_view k_product_policy_value_name = "ProductPolicy";

        struct product_policy_header
        {
            uint32_t total_size;
            uint32_t data_size;
            uint32_t end_marker_size;
            uint32_t touched;
            uint32_t reserved;
        };

        struct product_policy_entry
        {
            uint16_t size;
            uint16_t name_size;
            uint16_t type;
            uint16_t data_size;
            uint32_t flags;
            uint32_t reserved;
        };

        struct product_policy_match
        {
            uint16_t type{};
            std::span<const std::byte> data{};
        };

        template <typename T>
        bool read_struct(const std::span<const std::byte> blob, const size_t offset, T& out)
        {
            if (offset + sizeof(T) > blob.size())
            {
                return false;
            }

            std::memcpy(&out, blob.data() + offset, sizeof(T));
            return true;
        }

        NTSTATUS find_product_policy_value(const syscall_context& c, const std::u16string_view value_name, product_policy_match& out)
        {
            const auto key = c.win_emu.registry.get_key({std::u16string{k_product_options_key}});
            if (!key)
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            const auto product_policy = c.win_emu.registry.get_value(*key, k_product_policy_value_name);
            if (!product_policy || product_policy->type != REG_BINARY)
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            const auto blob = product_policy->data;
            if (blob.size() < sizeof(product_policy_header))
            {
                return STATUS_UNSUCCESSFUL;
            }

            product_policy_header header{};
            if (!read_struct(blob, 0, header))
            {
                return STATUS_UNSUCCESSFUL;
            }

            constexpr size_t header_size = sizeof(product_policy_header);
            if (header.data_size > blob.size() - header_size)
            {
                return STATUS_UNSUCCESSFUL;
            }

            size_t offset = header_size;
            const size_t end = header_size + header.data_size;
            while (offset + sizeof(product_policy_entry) <= end)
            {
                product_policy_entry entry{};
                if (!read_struct(blob, offset, entry))
                {
                    return STATUS_UNSUCCESSFUL;
                }

                if (entry.size < sizeof(product_policy_entry) || offset + entry.size > end)
                {
                    return STATUS_UNSUCCESSFUL;
                }

                if ((entry.name_size % sizeof(char16_t)) != 0)
                {
                    return STATUS_UNSUCCESSFUL;
                }

                const size_t name_offset = offset + sizeof(product_policy_entry);
                const size_t data_offset = name_offset + entry.name_size;
                if (data_offset + entry.data_size > offset + entry.size)
                {
                    return STATUS_UNSUCCESSFUL;
                }

                std::u16string entry_name(entry.name_size / sizeof(char16_t), u'\0');
                if (!entry_name.empty())
                {
                    std::memcpy(entry_name.data(), blob.data() + name_offset, entry.name_size);
                }

                if (!entry_name.empty() && entry_name.back() == u'\0')
                {
                    entry_name.pop_back();
                }

                if (entry_name == value_name)
                {
                    out.type = entry.type;
                    out.data = blob.subspan(data_offset, entry.data_size);
                    return STATUS_SUCCESS;
                }

                offset += entry.size;
            }

            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    NTSTATUS handle_NtQueryLicenseValue(const syscall_context& c, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name,
                                        emulator_object<uint32_t> type, uint64_t data, uint64_t data_size,
                                        emulator_object<uint32_t> result_data_size)
    {
        const auto name = read_unicode_string(c.emu, value_name);

        if (name == u"Kernel-VMDetection-Private")
        {
            c.win_emu.callbacks.on_suspicious_activity("Anti-vm check with NtQueryLicenseValue Kernel-VMDetection-Private");
        }

        if (name == u"TerminalServices-RemoteConnectionManager-AllowAppServerMode")
        {
            c.win_emu.callbacks.on_generic_activity(
                "Env check with NtQueryLicenseValue TerminalServices-RemoteConnectionManager-AllowAppServerMode");
        }

        product_policy_match policy_value{};
        const auto status = find_product_policy_value(c, name, policy_value);
        if (status != STATUS_SUCCESS)
        {
            return status;
        }

        if (type)
        {
            type.write(policy_value.type);
        }

        if (result_data_size)
        {
            result_data_size.write(static_cast<uint32_t>(policy_value.data.size()));
        }

        if (data_size < policy_value.data.size())
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        if (!policy_value.data.empty())
        {
            if (data == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            c.emu.write_memory(data, policy_value.data.data(), policy_value.data.size());
        }

        return STATUS_SUCCESS;
    }
}

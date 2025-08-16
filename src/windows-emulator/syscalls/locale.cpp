#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <utils/io.hpp>

namespace syscalls
{
    NTSTATUS handle_NtInitializeNlsFiles(const syscall_context& c, const emulator_object<uint64_t> base_address,
                                         const emulator_object<LCID> default_locale_id,
                                         const emulator_object<LARGE_INTEGER> /*default_casing_table_size*/)
    {
        const auto locale_file = utils::io::read_file(c.win_emu.file_sys.translate(R"(C:\Windows\System32\locale.nls)"));
        if (locale_file.empty())
        {
            return STATUS_FILE_INVALID;
        }

        const auto size = static_cast<size_t>(page_align_up(locale_file.size()));
        const auto base = c.win_emu.memory.allocate_memory(size, memory_permission::read);
        c.emu.write_memory(base, locale_file.data(), locale_file.size());

        base_address.write(base);
        default_locale_id.write(0x407);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryDefaultLocale(const syscall_context&, BOOLEAN /*user_profile*/, const emulator_object<LCID> default_locale_id)
    {
        default_locale_id.write(0x407);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtGetNlsSectionPtr(const syscall_context& c, const ULONG section_type, const ULONG section_data,
                                       emulator_pointer /*context_data*/, const emulator_object<uint64_t> section_pointer,
                                       const emulator_object<ULONG> section_size)
    {
        if (section_type == 11)
        {
            const auto file_path = R"(C:\Windows\System32\C_)" + std::to_string(section_data) + ".NLS";
            const auto locale_file = utils::io::read_file(c.win_emu.file_sys.translate(file_path));
            if (locale_file.empty())
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            const auto size = static_cast<size_t>(page_align_up(locale_file.size()));
            const auto section_memory = c.win_emu.memory.allocate_memory(size, memory_permission::read);
            c.emu.write_memory(section_memory, locale_file.data(), locale_file.size());

            section_pointer.write_if_valid(section_memory);
            section_size.write_if_valid(static_cast<ULONG>(size));

            return STATUS_SUCCESS;
        }

        c.win_emu.log.warn("Unsupported section type: %X\n", static_cast<uint32_t>(section_type));
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtGetMUIRegistryInfo()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtIsUILanguageComitted()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserGetKeyboardLayout()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryDefaultUILanguage(const syscall_context&, const emulator_object<LANGID> language_id)
    {
        language_id.write(0x407);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryInstallUILanguage(const syscall_context&, const emulator_object<LANGID> language_id)
    {
        language_id.write(0x407);
        return STATUS_SUCCESS;
    }
}

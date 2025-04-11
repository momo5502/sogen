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
        const auto locale_file =
            utils::io::read_file(c.win_emu.file_sys.translate(R"(C:\Windows\System32\locale.nls)"));
        if (locale_file.empty())
        {
            return STATUS_FILE_INVALID;
        }

        const auto size = page_align_up(locale_file.size());
        const auto base = c.win_emu.memory.allocate_memory(size, memory_permission::read);
        c.emu.write_memory(base, locale_file.data(), locale_file.size());

        base_address.write(base);
        default_locale_id.write(0x407);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryDefaultLocale(const syscall_context&, BOOLEAN /*user_profile*/,
                                         const emulator_object<LCID> default_locale_id)
    {
        default_locale_id.write(0x407);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtGetNlsSectionPtr()
    {
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

    NTSTATUS handle_NtQueryInstallUILanguage()
    {
        return STATUS_NOT_SUPPORTED;
    }
}
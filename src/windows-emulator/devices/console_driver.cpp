#include "console_driver.hpp"
#include "../emulator_utils.hpp"
#include "../windows_emulator.hpp"

#include <utils/object.hpp>

#include <utils/string.hpp>

std::optional<console_file_type> console_driver::parse_path(std::u16string_view path)
{

    constexpr std::u16string_view unc_prefix = u"\\??\\";
    if (utils::string::starts_with_ignore_case(path, unc_prefix))
    {
        auto sub_path = path.substr(unc_prefix.size());
        if (utils::string::starts_with_ignore_case(sub_path, std::u16string_view(u"CON")))
        {
            return console_file_type::SERVER;
        }
    }
    
    if (utils::string::equals_ignore_case(path, std::u16string_view(u"CONOUT$")))
    {
        return console_file_type::OUTPUT;
    }

    if (utils::string::equals_ignore_case(path, std::u16string_view(u"CONIN$")))
    {
        return console_file_type::INPUT;
    }

    constexpr std::u16string_view device_prefix = u"\\Device\\ConDrv";
    if (utils::string::starts_with_ignore_case(path, device_prefix))
    {
        return console_file_type::SERVER;
    }

    if (utils::string::equals_ignore_case(path, std::u16string_view(u"Reference")))
    {
        return console_file_type::REFERENCE;
    }
    if (utils::string::equals_ignore_case(path, std::u16string_view(u"Connect")))
    {
        return console_file_type::CONNECT;
    }

    return std::nullopt;
}

NTSTATUS console_driver::io_control(windows_emulator& win_emu, const io_device_context& context)
{
    win_emu.log.info("Console driver received IOCTL: %X\n", context.io_control_code);

    return STATUS_SUCCESS;
}

std::unique_ptr<file> console_driver::open(windows_emulator& win_emu, std::u16string_view path)
{
    (void)win_emu;

    auto f = std::make_unique<file>();

    if (const auto last_slash = path.find_last_of(u"/\\"); last_slash != std::u16string_view::npos)
    {
        path = path.substr(last_slash + 1);
    }
    
    f->name = u"\\Device\\ConDrv\\";
    f->name += path;

    return f;
}

std::unique_ptr<io_device> create_console_driver()
{
    return std::make_unique<console_driver>();
}
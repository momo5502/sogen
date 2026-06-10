#include "linux_syscall_info.hpp"

#include "linux_emulator_utils.hpp"

namespace sogen
{

    linux_syscall_info make_linux_syscall_info(x86_64_emulator& emu, const uint64_t number, const std::string_view name)
    {
        linux_syscall_info info{};
        info.number = number;
        info.name = name;
        info.emu = &emu;

        for (size_t i = 0; i < info.args.size(); ++i)
        {
            info.args[i] = get_linux_syscall_argument(emu, i);
        }

        return info;
    }

    std::optional<std::string> linux_syscall_info::read_c_string(const size_t arg_index, const size_t max_len) const
    {
        if (this->emu == nullptr)
        {
            return std::nullopt;
        }

        const auto address = this->arg(arg_index);
        if (address == 0)
        {
            return std::nullopt;
        }

        try
        {
            return read_string<char>(*this->emu, address, max_len);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

} // namespace sogen

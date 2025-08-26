#pragma once

#include "../io_device.hpp"

#include <optional>
#include <string_view>


class windows_emulator;

enum class console_file_type
{
    SERVER,
    REFERENCE,
    CONNECT,
    INPUT,
    OUTPUT,
};

class console_driver final : public stateless_device
{
  public:
    static std::optional<console_file_type> parse_path(std::u16string_view path);
    NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override;
    std::unique_ptr<file> open(windows_emulator& win_emu, std::u16string_view path) override;
};

std::unique_ptr<io_device> create_console_driver();
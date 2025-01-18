#pragma once
#include "x64_gdb_stub_handler.hpp"

#include "../windows_emulator.hpp"

class win_x64_gdb_stub_handler : public x64_gdb_stub_handler
{
  public:
    win_x64_gdb_stub_handler(windows_emulator& win_emu)
        : x64_gdb_stub_handler(win_emu.emu()),
          win_emu_(&win_emu)
    {
    }

    gdb_stub::gdb_action run() override
    {
        try
        {
            this->win_emu_->start();
        }
        catch (const std::exception& e)
        {
            this->win_emu_->log.error("%s\n", e.what());
        }

        return gdb_stub::gdb_action::resume;
    }

    gdb_stub::gdb_action singlestep() override
    {
        try
        {
            this->win_emu_->start({}, 1);
        }
        catch (const std::exception& e)
        {
            this->win_emu_->log.error("%s\n", e.what());
        }

        return gdb_stub::gdb_action::resume;
    }

    std::string get_target_description() override
    {
        return "i386:x86-64";
    }

  private:
    windows_emulator* win_emu_{};
};

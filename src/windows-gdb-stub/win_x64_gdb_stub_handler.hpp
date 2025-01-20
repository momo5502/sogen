#pragma once
#include "x64_gdb_stub_handler.hpp"

#include <windows_emulator.hpp>

class win_x64_gdb_stub_handler : public x64_gdb_stub_handler
{
  public:
    win_x64_gdb_stub_handler(windows_emulator& win_emu)
        : x64_gdb_stub_handler(win_emu.emu()),
          win_emu_(&win_emu)
    {
    }

    gdb_stub::action run() override
    {
        try
        {
            this->win_emu_->start();
        }
        catch (const std::exception& e)
        {
            this->win_emu_->log.error("%s\n", e.what());
        }

        return gdb_stub::action::resume;
    }

    gdb_stub::action singlestep() override
    {
        try
        {
            this->win_emu_->start({}, 1);
        }
        catch (const std::exception& e)
        {
            this->win_emu_->log.error("%s\n", e.what());
        }

        return gdb_stub::action::resume;
    }

    uint32_t get_current_thread_id() override
    {
        return this->win_emu_->current_thread().id;
    }

    std::vector<uint32_t> get_thread_ids() override
    {
        const auto& threads = this->win_emu_->process().threads;

        std::vector<uint32_t> ids{};
        ids.reserve(threads.size());

        for (const auto& t : threads | std::views::values)
        {
            if (!t.is_terminated())
            {
                ids.push_back(t.id);
            }
        }

        return ids;
    }

    bool switch_to_thread(const uint32_t thread_id) override
    {
        return this->win_emu_->activate_thread(thread_id);
    }

    std::optional<uint32_t> get_exit_code() override
    {
        const auto status = this->win_emu_->process().exit_status;
        if (!status)
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(*status);
    }

  private:
    windows_emulator* win_emu_{};
};

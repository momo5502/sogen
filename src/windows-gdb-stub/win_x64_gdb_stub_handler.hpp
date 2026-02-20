#pragma once
#include "x64_gdb_stub_handler.hpp"

#include <atomic>
#include <windows_emulator.hpp>
#include <utils/function.hpp>
#include <utils/string.hpp>

class win_x64_gdb_stub_handler : public x64_gdb_stub_handler
{
  public:
    win_x64_gdb_stub_handler(windows_emulator& win_emu, utils::optional_function<bool()> should_stop = {})
        : x64_gdb_stub_handler(win_emu.emu()),
          win_emu_(&win_emu),
          should_stop_(std::move(should_stop))
    {
        auto hook = [this](mapped_module&) {
            library_stop_pending_ = true;
            win_emu_->stop();
        };

        mod_load_id = win_emu_->callbacks.on_module_load.add(hook);
        mod_unload_id = win_emu_->callbacks.on_module_unload.add(hook);
    }

    ~win_x64_gdb_stub_handler() override
    {
        win_emu_->callbacks.on_module_load.remove(mod_load_id);
        win_emu_->callbacks.on_module_unload.remove(mod_unload_id);
    }

    void on_interrupt() override
    {
        this->win_emu_->stop();
    }

    bool should_stop() override
    {
        return this->should_stop_();
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
            this->win_emu_->start(1);
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
        const auto& threads = this->win_emu_->process.threads;

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
        const auto status = this->win_emu_->process.exit_status;
        if (!status)
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(*status);
    }

    std::vector<gdb_stub::library_info> get_libraries() override
    {
        std::vector<gdb_stub::library_info> libs{};
        const auto& mod_manager = this->win_emu_->mod_manager;
        libs.reserve(this->win_emu_->mod_manager.modules().size());
        for (const auto& [base_addr, mod] : mod_manager.modules())
        {
            if (!mod.module_path.empty())
            {
                libs.push_back({.name = mod.module_path.string(), .segment_address = base_addr + 0x1000});
            }
        }

        return libs;
    }

    std::string get_executable_path() override
    {
        const auto& mod_manager = this->win_emu_->mod_manager;
        return mod_manager.executable->module_path.string();
    }

    void reset_library_stop() override
    {
        library_stop_pending_ = false;
    }

    bool should_signal_library() override
    {
        return library_stop_pending_;
    }

  private:
    windows_emulator* win_emu_{};
    utils::optional_function<bool()> should_stop_{};

    // Track library stop events
    std::atomic<bool> library_stop_pending_{true};
    utils::callback_id_type mod_load_id{};
    utils::callback_id_type mod_unload_id{};
};

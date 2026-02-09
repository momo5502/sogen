#pragma once
#include "x64_gdb_stub_handler.hpp"

#include <linux_emulator.hpp>
#include <utils/function.hpp>

class linux_x64_gdb_stub_handler : public x64_gdb_stub_handler
{
  public:
    linux_x64_gdb_stub_handler(linux_emulator& linux_emu, utils::optional_function<bool()> should_stop = {})
        : x64_gdb_stub_handler(linux_emu.emu()),
          linux_emu_(&linux_emu),
          should_stop_(std::move(should_stop))
    {
    }

    ~linux_x64_gdb_stub_handler() override = default;

    void on_interrupt() override
    {
        this->linux_emu_->stop();
    }

    bool should_stop() override
    {
        return this->should_stop_();
    }

    gdb_stub::action run() override
    {
        try
        {
            this->linux_emu_->start();
        }
        catch (const std::exception& e)
        {
            this->linux_emu_->log.error("GDB run: %s\n", e.what());
        }

        return gdb_stub::action::resume;
    }

    gdb_stub::action singlestep() override
    {
        try
        {
            this->linux_emu_->start(1);
        }
        catch (const std::exception& e)
        {
            this->linux_emu_->log.error("GDB singlestep: %s\n", e.what());
        }

        return gdb_stub::action::resume;
    }

    uint32_t get_current_thread_id() override
    {
        if (this->linux_emu_->process.active_thread)
        {
            return this->linux_emu_->process.active_thread->tid;
        }

        return 1;
    }

    std::vector<uint32_t> get_thread_ids() override
    {
        const auto& threads = this->linux_emu_->process.threads;

        std::vector<uint32_t> ids{};
        ids.reserve(threads.size());

        for (const auto& [tid, t] : threads)
        {
            if (!t.terminated)
            {
                ids.push_back(t.tid);
            }
        }

        return ids;
    }

    bool switch_to_thread(const uint32_t thread_id) override
    {
        auto& proc = this->linux_emu_->process;
        auto& emu = this->linux_emu_->emu();

        const auto it = proc.threads.find(thread_id);
        if (it == proc.threads.end() || it->second.terminated)
        {
            return false;
        }

        if (proc.active_thread && proc.active_thread->tid == thread_id)
        {
            return true;
        }

        // Save current thread state
        if (proc.active_thread)
        {
            proc.active_thread->save(emu);
        }

        // Switch to the target thread
        proc.active_thread = &it->second;
        proc.active_thread->restore(emu);

        return true;
    }

    std::optional<uint32_t> get_exit_code() override
    {
        if (!this->linux_emu_->process.exit_status)
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(*this->linux_emu_->process.exit_status);
    }

    std::vector<gdb_stub::library_info> get_libraries() override
    {
        std::vector<gdb_stub::library_info> libs{};
        const auto& modules = this->linux_emu_->mod_manager.get_modules();
        libs.reserve(modules.size());

        for (const auto& [base_addr, mod] : modules)
        {
            libs.push_back({mod.path.string(), base_addr});
        }

        return libs;
    }

    std::string get_executable_path() override
    {
        return this->linux_emu_->mod_manager.get_executable_path().string();
    }

  private:
    linux_emulator* linux_emu_{};
    utils::optional_function<bool()> should_stop_{};
};

#pragma once
#include "cpu_gdb_stub_handler.hpp"
#include "windows_filesystem.hpp"

#include <atomic>
#include <windows_emulator.hpp>
#include <thread_context.hpp>
#include <utils/function.hpp>
#include <utils/string.hpp>

namespace sogen
{

    enum class gdb_target_architecture
    {
        automatic,
        bits_64,
        bits_32,
    };

    class win_gdb_stub_handler : public cpu_gdb_stub_handler
    {
      public:
        win_gdb_stub_handler(windows_emulator& win_emu, utils::optional_function<bool()> should_stop = {},
                             const gdb_target_architecture target_architecture = gdb_target_architecture::bits_64)
            : cpu_gdb_stub_handler(win_emu.emu()),
              win_emu_(&win_emu),
              should_stop_(std::move(should_stop)),
              windows_filesystem_(win_emu),
              target_architecture_(target_architecture)
        {
            auto hook = [this](mapped_module& mod) {
                if (!this->is_visible_module(mod))
                {
                    return;
                }

                library_stop_pending_ = true;
                module_event_ = true;
                win_emu_->stop();
            };

            mod_load_id = win_emu_->callbacks.on_module_load.add(hook);
            mod_unload_id = win_emu_->callbacks.on_module_unload.add(hook);
            dbg_msg_id = win_emu_->callbacks.on_debug_string.add([this](std::string_view message) {
                debug_message.assign(message);
                action = gdb_stub::action::output;
                win_emu_->stop();
            });
        }

        ~win_gdb_stub_handler() override
        {
            win_emu_->callbacks.on_module_load.remove(mod_load_id);
            win_emu_->callbacks.on_module_unload.remove(mod_unload_id);
            win_emu_->callbacks.on_debug_string.remove(dbg_msg_id);
        }

        void on_interrupt() override
        {
            interrupt_requested_ = true;
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

            return action;
        }

        gdb_stub::action singlestep() override
        {
            try
            {
                auto& vcpu = this->win_emu_->vcpu(0);

                // Bypass the emulator scheduler: the GDB protocol already
                // selected the target thread via switch_to_thread(). Going
                // through windows_emulator::start() would let the scheduler
                // redirect to a different "ready" thread (e.g. when the
                // stepped thread is in NtDelayExecution), causing IDA to
                // receive a T05 for an unexpected tid → SIGTRAP error.
                vcpu.switch_thread = false;

                if (this->is_wow64_view())
                {
                    this->step_wow64_thread(vcpu);
                    return action;
                }

                vcpu.thread().setup_if_necessary(vcpu.cpu, this->win_emu_->process);
                vcpu.cpu.start(1);
            }
            catch (const std::exception& e)
            {
                this->win_emu_->log.error("%s\n", e.what());
            }

            return action;
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
            return this->win_emu_->activate_thread(this->win_emu_->vcpu(0), thread_id);
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
                if (!mod.module_path.empty() && this->is_visible_module(mod))
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

        std::vector<gdb_stub::thread_info> get_thread_list() const override
        {
            std::vector<gdb_stub::thread_info> thread_list{};
            const auto& threads = this->win_emu_->process.threads;
            thread_list.reserve(threads.size());

            for (const auto& t : threads | std::views::values)
            {
                if (!t.is_terminated())
                {
                    thread_list.push_back({.id = t.id, .name = u16_to_u8(t.name)});
                }
            }

            return thread_list;
        }

        uint64_t get_thread_teb_addr(uint32_t id) const override
        {
            for (const auto& t : this->win_emu_->process.threads | std::views::values)
            {
                if (t.id == id)
                {
                    if (!t.is_terminated() && this->is_wow64_view() && t.teb32)
                    {
                        return t.teb32->value();
                    }

                    if (!t.is_terminated() && t.teb64)
                    {
                        return t.teb64->value();
                    }

                    break;
                }
            }

            return 0;
        }

        std::string consume_debug_output() override
        {
            action = gdb_stub::action::resume;
            return std::exchange(debug_message, std::string{});
        }

        std::string get_os_abi() override
        {
            return "Windows";
        }

        bool is_32_bit() const override
        {
            switch (target_architecture_)
            {
            case gdb_target_architecture::automatic:
                return win_emu_->process.is_wow64_process;
            case gdb_target_architecture::bits_32:
                return true;
            case gdb_target_architecture::bits_64:
                return false;
            }

            throw std::runtime_error("Invalid GDB target architecture");
        }

        gdb_stub::filesystem_interface* get_filesystem() override
        {
            return &windows_filesystem_;
        }

        size_t read_register(const size_t reg, void* data, const size_t max_length) override
        {
            if (this->shows_saved_wow64_state())
            {
                return this->read_saved_wow64_register(reg, data, max_length);
            }

            return cpu_gdb_stub_handler::read_register(reg, data, max_length);
        }

        size_t write_register(const size_t reg, const void* data, const size_t size) override
        {
            if (this->shows_saved_wow64_state())
            {
                return this->write_saved_wow64_register(reg, data, size);
            }

            return cpu_gdb_stub_handler::write_register(reg, data, size);
        }

      private:
        static constexpr size_t max_instructions_per_step = 1'000'000;

        static DWORD* wow64_context_field(WOW64_CONTEXT& c, const x86_register reg)
        {
            switch (reg)
            {
            case x86_register::eax:
                return &c.Eax;
            case x86_register::ecx:
                return &c.Ecx;
            case x86_register::edx:
                return &c.Edx;
            case x86_register::ebx:
                return &c.Ebx;
            case x86_register::esp:
                return &c.Esp;
            case x86_register::ebp:
                return &c.Ebp;
            case x86_register::esi:
                return &c.Esi;
            case x86_register::edi:
                return &c.Edi;
            case x86_register::eip:
                return &c.Eip;
            case x86_register::eflags:
                return &c.EFlags;
            case x86_register::cs:
                return &c.SegCs;
            case x86_register::ss:
                return &c.SegSs;
            case x86_register::ds:
                return &c.SegDs;
            case x86_register::es:
                return &c.SegEs;
            case x86_register::fs:
                return &c.SegFs;
            case x86_register::gs:
                return &c.SegGs;
            default:
                return nullptr;
            }
        }

        static bool is_segment_register(const x86_register reg)
        {
            return reg == x86_register::cs || reg == x86_register::ss || reg == x86_register::ds || reg == x86_register::es ||
                   reg == x86_register::fs || reg == x86_register::gs;
        }

        x86_register get_x86_register(const size_t reg) const
        {
            const auto registers = this->get_register_mapping();
            return reg < registers.size() ? static_cast<x86_register>(registers[reg].reg) : x86_register::invalid;
        }

        bool shows_saved_wow64_state() const
        {
            return this->is_wow64_view() && !is_32bit_code_segment(this->win_emu_->vcpu(0).x64_cpu());
        }

        // In the WOW64 view, the client debugs a WOW64 process with the i386 target, so it sees only the 32-bit part of the
        // process: the I386 modules and the 32-bit code. While the thread runs 64-bit WOW64 code, the stub reads the registers
        // from the saved 32-bit context and not from the CPU.
        bool is_wow64_view() const
        {
            return this->is_32_bit() && this->win_emu_->process.is_wow64_process;
        }

        // The client of the WOW64 view does not see the 64-bit modules, for example 64-bit ntdll and wow64cpu.
        bool is_visible_module(const mapped_module& mod) const
        {
            return !this->is_wow64_view() || mod.machine == static_cast<uint16_t>(PEMachineType::I386);
        }

        // The client does not see the 64-bit WOW64 code, so one step runs that code together with one 32-bit instruction.
        // A wait syscall does not wait: it marks the thread as waiting, yields and returns at once. The scheduler runs the
        // thread again when the wait ends and sets the syscall result then. A step does not use the scheduler, so it must not
        // start a thread that still waits, and it must end at the yield.
        void step_wow64_thread(vcpu_context& vcpu)
        {
            auto& thread = vcpu.thread();

            if (!this->resume_stepped_thread(vcpu, thread))
            {
                return;
            }

            module_event_ = false;
            interrupt_requested_ = false;
            bool ran_32bit_instruction = false;

            for (size_t i = 0; i < max_instructions_per_step; ++i)
            {
                ran_32bit_instruction |= !this->shows_saved_wow64_state();
                vcpu.cpu.start(1);

                if (action != gdb_stub::action::resume || thread.is_terminated() || this->win_emu_->process.exit_status.has_value())
                {
                    break;
                }

                // A module load needs a library stop.
                if (module_event_)
                {
                    break;
                }

                // A wait yields, and the scheduler must run the thread again. An alertable yield, the time slice and the wake of
                // another thread also request a thread switch, but the thread can still run. The step continues then, because the
                // CPU can be in wow64cpu before the 32-bit context is saved.
                if (vcpu.switch_thread && !this->resume_stepped_thread(vcpu, thread))
                {
                    break;
                }

                // The step ends in 32-bit code after one 32-bit instruction or after a stop in 64-bit code. A breakpoint, watchpoint
                // or interrupt can stop the CPU before wow64cpu saves the 32-bit context, so the stub defers that stop to 32-bit code.
                if (!this->shows_saved_wow64_state() && (ran_32bit_instruction || interrupt_requested_))
                {
                    break;
                }
            }
        }

        // Resumes the thread in the same way as the scheduler: switch_to_thread writes the status of a wait that is over,
        // dispatches an APC to a thread that yielded as alertable and ends the alertable state. A thread that waits and has no
        // APC to dispatch stays with the scheduler.
        bool resume_stepped_thread(vcpu_context& vcpu, emulator_thread& thread)
        {
            const auto can_run =
                this->win_emu_->synchronize([&] { return thread.is_thread_ready(*this->win_emu_) || thread.has_pending_alertable_apc(); });

            // activate_thread does not check if the thread can run.
            vcpu.switch_thread = !can_run || !this->win_emu_->activate_thread(vcpu, thread.id);
            return !vcpu.switch_thread;
        }

        size_t read_saved_wow64_register(const size_t reg, void* data, const size_t max_length)
        {
            const auto x86_reg = this->get_x86_register(reg);
            if (x86_reg == x86_register::fs_base)
            {
                return this->read_teb32_base(data, max_length);
            }

            auto context = this->read_wow64_context();
            const auto* field = context ? wow64_context_field(*context, x86_reg) : nullptr;
            if (!field)
            {
                // The saved context gives only the CONTEXT32_FULL registers. The others, for example x87 and XMM, come from the CPU.
                return cpu_gdb_stub_handler::read_register(reg, data, max_length);
            }

            if (max_length < sizeof(*field))
            {
                return 0;
            }

            auto value = *field;
            if (is_segment_register(x86_reg))
            {
                // 32-bit code stores selectors into a CONTEXT with 16-bit moves, which leave the upper bits undefined.
                value &= 0xFFFF;
            }

            memcpy(data, &value, sizeof(value));
            return sizeof(value);
        }

        size_t write_saved_wow64_register(const size_t reg, const void* data, const size_t size)
        {
            const auto x86_reg = this->get_x86_register(reg);
            if (x86_reg == x86_register::fs_base)
            {
                return this->write_teb32_base(data, size);
            }

            auto context = this->read_wow64_context();
            auto* field = context ? wow64_context_field(*context, x86_reg) : nullptr;
            if (!field)
            {
                // The saved context takes only the CONTEXT32_FULL registers. The others go to the CPU.
                return cpu_gdb_stub_handler::write_register(reg, data, size);
            }

            if (size < sizeof(*field))
            {
                return 0;
            }

            memcpy(field, data, sizeof(*field));
            context->ContextFlags = CONTEXT32_FULL;

            auto& vcpu = this->win_emu_->vcpu(0);
            const auto* bytes = reinterpret_cast<const uint8_t*>(&*context);
            if (!this->win_emu_->synchronize(
                    [&] { return apply_wow64_thread_context(*this->win_emu_, vcpu, vcpu.thread(), {bytes, sizeof(WOW64_CONTEXT)}); }))
            {
                return 0;
            }

            return sizeof(*field);
        }

        size_t read_teb32_base(void* data, const size_t max_length)
        {
            const uint64_t teb32 = this->get_thread_teb_addr(this->get_current_thread_id());
            if (max_length < sizeof(teb32))
            {
                return 0;
            }

            memcpy(data, &teb32, sizeof(teb32));
            return sizeof(teb32);
        }

        // The stub does not move the TEB. It accepts a write of the current address, because a G packet writes all registers.
        size_t write_teb32_base(const void* data, const size_t size)
        {
            const uint64_t teb32 = this->get_thread_teb_addr(this->get_current_thread_id());
            return size >= sizeof(teb32) && memcmp(data, &teb32, sizeof(teb32)) == 0 ? sizeof(teb32) : 0;
        }

        std::optional<WOW64_CONTEXT> read_wow64_context() const
        {
            auto& vcpu = this->win_emu_->vcpu(0);
            const auto bytes =
                this->win_emu_->synchronize([&] { return capture_wow64_thread_context(*this->win_emu_, vcpu, vcpu.thread()); });
            if (bytes.size() < sizeof(WOW64_CONTEXT))
            {
                return std::nullopt;
            }

            WOW64_CONTEXT context{};
            memcpy(&context, bytes.data(), sizeof(context));
            return context;
        }

        windows_emulator* win_emu_{};
        utils::optional_function<bool()> should_stop_{};
        windows_filesystem windows_filesystem_;
        gdb_target_architecture target_architecture_{gdb_target_architecture::bits_64};

        // Track library stop events
        std::atomic<bool> library_stop_pending_{true};
        std::atomic<bool> module_event_{false};
        std::atomic<bool> interrupt_requested_{false};
        std::string debug_message{};
        gdb_stub::action action{gdb_stub::action::resume};
        utils::callback_id_type mod_load_id{};
        utils::callback_id_type mod_unload_id{};
        utils::callback_id_type dbg_msg_id{};
    };

} // namespace sogen

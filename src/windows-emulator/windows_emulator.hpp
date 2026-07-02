#pragma once
#include "std_include.hpp"

#include <arch_emulator.hpp>

#include <stop_reason.hpp>
#include <utils/function.hpp>

#include "syscall_dispatcher.hpp"
#include "process_context.hpp"
#include "kernel_lock.hpp"
#include "logger.hpp"
#include "file_system.hpp"
#include "memory_manager.hpp"
#include "module/module_manager.hpp"
#include "network/dns_lookup.hpp"
#include "network/socket_factory.hpp"
#include "version/windows_version_manager.hpp"
#include <platform/ui_backend.hpp>

namespace sogen
{

    struct io_device;

    struct emulator_callbacks : module_manager::callbacks, process_context::callbacks
    {
        template <typename T>
        using opt_func = utils::optional_function<T>;

        using continuation = instruction_hook_continuation;

        opt_func<void()> on_exception{};

        opt_func<void(uint64_t address, uint64_t length, memory_permission)> on_memory_protect{};
        opt_func<void(uint64_t address, uint64_t length, memory_permission, bool commit)> on_memory_allocate{};
        opt_func<void(uint64_t address, uint64_t length, memory_operation, memory_violation_type type)> on_memory_violate{};

        opt_func<void()> on_rdtsc{};
        opt_func<void()> on_rdtscp{};
        opt_func<continuation(uint32_t syscall_id, std::string_view syscall_name)> on_syscall{};
        opt_func<void(std::string_view data)> on_stdout{};
        opt_func<void(std::string_view type, std::u16string_view name)> on_generic_access{};
        opt_func<void(std::string_view description)> on_generic_activity{};
        opt_func<void(std::string_view description)> on_suspicious_activity{};
        utils::callback_list<void(std::string_view message)> on_debug_string{};
        utils::callback_list<void(const mapped_module& mod, const mapped_section& section, uint64_t address)> on_section_first_execution{};
        opt_func<void(uint64_t address)> on_instruction{};
        opt_func<void(io_device& device, std::u16string_view device_name, ULONG code)> on_ioctrl{};
        opt_func<void(uint32_t fail_code)> on_fast_fail{};
    };

    struct application_settings
    {
        windows_path application{};
        windows_path working_directory{};
        std::vector<std::u16string> arguments{};
        utils::unordered_insensitive_u16string_map<std::u16string> environment{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->application);
            buffer.write(this->working_directory);
            buffer.write_vector(this->arguments);
            buffer.write_map(this->environment);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->application);
            buffer.read(this->working_directory);
            buffer.read_vector(this->arguments);
            buffer.read_map(this->environment);
        }
    };

    // Knobs for values the emulator exposes to the emulated process that don't
    // depend on the host environment. Samples (particularly anti-analysis
    // payloads) probe these to detect VM/sandbox; today they are hardcoded in
    // process_context.cpp (PEB.NumberOfProcessors = 4) and kusd_mmio.cpp
    // (KUSER_SHARED_DATA.NtProductType = NtProductWinNt). Defaults match the
    // legacy hardcoded values — behavior is unchanged when consumers leave
    // this field at its default.
    struct fake_environment_config
    {
        uint32_t number_of_processors{4};
        uint8_t nt_product_type{1}; // NtProductWinNt
    };

    struct emulator_settings
    {
        bool disable_logging{false};
        bool use_relative_time{false};
        bool use_instruction_precision{true};

        std::filesystem::path emulation_root{};
        std::filesystem::path registry_directory{"./registry"};

        std::unordered_map<uint16_t, uint16_t> port_mappings{};
        std::unordered_map<windows_path, std::filesystem::path> path_mappings{};

        fake_environment_config fake_env{};
    };

    struct emulator_interfaces
    {
        std::unique_ptr<utils::clock> clock{};
        std::unique_ptr<network::dns_lookup> dns_lookup{};
        std::unique_ptr<network::socket_factory> socket_factory{};
        std::unique_ptr<ui_backend> ui{};
    };

    // Per-vCPU scheduler state: the guest thread a virtual CPU is currently executing
    // and its yield request. Replaces the previous global "active thread" notion
    // (docs/multi-vcpu-design.md, section 5.1).
    struct vcpu_context
    {
        x86_64_cpu& cpu;
        emulator_thread* active_thread{};
        std::atomic_bool switch_thread{false};

        emulator_thread& thread() const
        {
            if (!this->active_thread)
            {
                throw std::runtime_error("No active thread!");
            }

            return *this->active_thread;
        }
    };

    class windows_emulator
    {
        uint64_t executed_instructions_{0};
        application_settings application_settings_{};

        std::unique_ptr<x86_64_emulator> emu_{};
        std::unique_ptr<utils::clock> clock_{};
        std::unique_ptr<network::dns_lookup> dns_lookup_{};
        std::unique_ptr<network::socket_factory> socket_factory_{};
        std::unique_ptr<ui_backend> ui_backend_{};
        bool setup_completed_{false};

      public:
        const std::filesystem::path emulation_root{};
        const fake_environment_config fake_env{};
        emulator_callbacks callbacks{};
        logger log{};
        file_system file_sys;
        memory_manager memory;
        registry_manager registry{};
        windows_version_manager version{};
        module_manager mod_manager;
        process_context process;
        syscall_dispatcher dispatcher;

        windows_emulator(std::unique_ptr<x86_64_emulator> emu, const emulator_settings& settings = {}, emulator_callbacks callbacks = {},
                         emulator_interfaces interfaces = {});
        windows_emulator(std::unique_ptr<x86_64_emulator> emu, application_settings app_settings, const emulator_settings& settings = {},
                         emulator_callbacks callbacks = {}, emulator_interfaces interfaces = {});

        windows_emulator(windows_emulator&&) = delete;
        windows_emulator(const windows_emulator&) = delete;
        windows_emulator& operator=(windows_emulator&&) = delete;
        windows_emulator& operator=(const windows_emulator&) = delete;

        ~windows_emulator();

        x86_64_emulator& emu()
        {
            return *this->emu_;
        }

        const x86_64_emulator& emu() const
        {
            return *this->emu_;
        }

        utils::clock& clock()
        {
            return *this->clock_;
        }

        const utils::clock& clock() const
        {
            return *this->clock_;
        }
        network::dns_lookup& dns_lookup()
        {
            return *this->dns_lookup_;
        }

        const network::dns_lookup& dns_lookup() const
        {
            return *this->dns_lookup_;
        }

        network::socket_factory& socket_factory()
        {
            return *this->socket_factory_;
        }

        const network::socket_factory& socket_factory() const
        {
            return *this->socket_factory_;
        }

        ui_backend& ui()
        {
            return *this->ui_backend_;
        }

        const ui_backend& ui() const
        {
            return *this->ui_backend_;
        }

        void handle_ui_event(const ui_event& event);
        void deliver_raw_input(const process_context::raw_input_payload& payload, hwnd explicit_target);
        void deliver_raw_mouse_input(int32_t dx, int32_t dy, uint16_t button_flags);
        void deliver_raw_keyboard_input(uint16_t vkey, uint16_t scan_code, uint32_t message, bool extended);

        // Observer convenience for external consumers (gdb stub, analyzer, python
        // bindings) and legacy paths that don't thread a vcpu_context through. Resolves
        // to the vCPU whose handler is currently running: all handler/observer code runs
        // under the kernel lock, so exactly one vCPU dispatches at a time and this is
        // race-free. Falls back to vCPU 0 when no handler is active (e.g. setup).
        emulator_thread& current_thread() const
        {
            return (this->dispatch_vcpu_ ? this->dispatch_vcpu_ : this->vcpus_[0].get())->thread();
        }

        // The CPU of the vCPU currently dispatching a handler on the calling host thread (see
        // scoped_dispatch). emu() is only the facade/vCPU 0, so handlers that inspect the faulting
        // register state must go through here to observe the right vCPU when vcpu_count > 1.
        x86_64_cpu& active_cpu() const
        {
            return (this->dispatch_vcpu_ ? this->dispatch_vcpu_ : this->vcpus_[0].get())->cpu;
        }

        // Run fn as a dispatched handler for the CPU that triggered a hook: takes the kernel lock and
        // marks that vCPU as the dispatching one, so active_cpu()/current_thread() resolve to it. Meant
        // for hooks installed outside setup_hooks (e.g. the analyzer's cpuid hook) which otherwise run
        // lock-free and would observe only the facade/vCPU 0 when vcpu_count > 1.
        template <typename Function>
        auto dispatch_on_cpu(cpu_interface& cpu, Function&& fn)
        {
            const std::scoped_lock lock(this->kernel_lock_);
            const scoped_dispatch dispatch(*this, this->vcpu(cpu.index()));
            return std::forward<Function>(fn)();
        }

        vcpu_context& vcpu(const size_t index)
        {
            return *this->vcpus_.at(index);
        }

        // Marks vcpu as the one currently dispatching a handler on the calling host
        // thread, so current_thread() resolves correctly. RAII; must be held only while
        // the kernel lock is held.
        class scoped_dispatch
        {
          public:
            scoped_dispatch(windows_emulator& emu, vcpu_context& vcpu)
                : emu_(&emu),
                  previous_(emu.dispatch_vcpu_)
            {
                emu.dispatch_vcpu_ = &vcpu;
            }

            ~scoped_dispatch()
            {
                this->emu_->dispatch_vcpu_ = this->previous_;
            }

            scoped_dispatch(const scoped_dispatch&) = delete;
            scoped_dispatch& operator=(const scoped_dispatch&) = delete;
            scoped_dispatch(scoped_dispatch&&) = delete;
            scoped_dispatch& operator=(scoped_dispatch&&) = delete;

          private:
            windows_emulator* emu_{};
            vcpu_context* previous_{};
        };

        // Post-mortem exception capture for multi-vCPU debugging. Recorded under the
        // kernel lock (no I/O, no extra locking), dumped after the run ends, so it does
        // not perturb the timing-sensitive races it is meant to catch.
        struct exception_trace_entry
        {
            uint32_t status{};
            uint32_t tid{};
            uint32_t vcpu{};
            uint64_t rip{};
            uint64_t info{};
        };

        void record_exception_trace(const exception_trace_entry& entry)
        {
            this->exception_trace_[this->exception_trace_index_ % this->exception_trace_.size()] = entry;
            ++this->exception_trace_index_;
        }

        void dump_exception_trace();

        // Prints BEL contention stats when SOGEN_LOCK_PROFILE is set (see kernel_lock).
        void dump_lock_profile();

        uint64_t get_executed_instructions() const
        {
            return this->executed_instructions_;
        }

        bool uses_instruction_precision() const
        {
            return this->instruction_precision_;
        }

        uint32_t vcpu_count() const
        {
            return this->vcpu_count_;
        }

        stop_reason last_stop_reason() const
        {
            return this->last_stop_reason_;
        }

        const std::string& last_stop_detail() const
        {
            return this->last_stop_detail_;
        }

        // Called by internal paths that force a stop due to an error (syscall
        // dispatcher unknown/unimplemented/exception). Public so future error
        // paths can record themselves without friending everyone.
        void record_stop(stop_reason r, std::string detail = {})
        {
            this->last_stop_reason_ = r;
            this->last_stop_detail_ = std::move(detail);
        }

        void setup_process_if_necessary();

        void start(size_t count = 0);
        void stop();

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

        void save_snapshot();
        void restore_snapshot();

        uint16_t get_host_port(const uint16_t emulator_port) const
        {
            const auto entry = this->port_mappings_.find(emulator_port);
            if (entry == this->port_mappings_.end())
            {
                return emulator_port;
            }

            return entry->second;
        }

        uint16_t get_emulator_port(const uint16_t host_port) const
        {
            for (const auto& mapping : this->port_mappings_)
            {
                if (mapping.second == host_port)
                {
                    return mapping.first;
                }
            }

            return host_port;
        }

        void map_port(const uint16_t emulator_port, const uint16_t host_port)
        {
            if (emulator_port != host_port)
            {
                this->port_mappings_[emulator_port] = host_port;
                return;
            }

            const auto entry = this->port_mappings_.find(emulator_port);
            if (entry != this->port_mappings_.end())
            {
                this->port_mappings_.erase(entry);
            }
        }

        void yield_thread(vcpu_context& vcpu, bool alertable = false);
        bool perform_thread_switch(vcpu_context& vcpu, std::unique_lock<kernel_lock>& lock);
        bool perform_thread_switch(vcpu_context& vcpu);
        bool activate_thread(vcpu_context& vcpu, uint32_t id);

      private:
        bool use_relative_time_{false}; // TODO: Get rid of that
        bool instruction_precision_{true};
        uint32_t vcpu_count_{1};
        std::atomic_bool should_stop{false};

        // The emulator kernel lock: held by all code touching shared kernel state,
        // released while guest code executes (docs/multi-vcpu-design.md, section 7).
        kernel_lock kernel_lock_{};

        // The vCPU currently running a handler under the kernel lock; drives
        // current_thread(). See scoped_dispatch.
        vcpu_context* dispatch_vcpu_{};

        std::array<exception_trace_entry, 32> exception_trace_{};
        size_t exception_trace_index_{0};

        // unique_ptr because vcpu_context contains an atomic and must stay address-stable.
        std::vector<std::unique_ptr<vcpu_context>> vcpus_{};

        std::unordered_map<uint16_t, uint16_t> port_mappings_{};

        std::vector<std::byte> process_snapshot_{};
        // std::optional<process_context> process_snapshot_{};

        stop_reason last_stop_reason_{stop_reason::none};
        std::string last_stop_detail_{};

        std::map<uint64_t, std::vector<emulator_hook*>> section_first_execution_hooks_{};

        void setup_hooks();
        void setup_process();
        void vcpu_worker(vcpu_context& vcpu);
        void on_instruction_execution(vcpu_context& vcpu, uint64_t address);
        void on_basic_block_execution(vcpu_context& vcpu, const basic_block& block);

        bool uses_section_first_execution_hooks() const;
        void clear_section_first_execution_hooks();
        void install_section_first_execution_hook(const mapped_module& mod, size_t section_index);
        void install_section_first_execution_hooks();
        void track_section_first_execution(uint64_t address);
        void restore_ui_backend();

        void register_factories(utils::buffer_deserializer& buffer);
    };

} // namespace sogen

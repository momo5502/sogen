#pragma once
#include "std_include.hpp"

#include <arch_emulator.hpp>
#include <machine_type.hpp>
#include "process_manager.hpp"

#include <stop_reason.hpp>
#include <utils/finally.hpp>
#include <utils/function.hpp>

#include "syscall_dispatcher.hpp"
#include "process_context.hpp"
#include "kernel_lock.hpp"
#include "scheduler_clock.hpp"
#include "logger.hpp"
#include "file_system.hpp"
#include "frame_pacer.hpp"
#include "filesystem/mount_spec.hpp"
#include "memory_manager.hpp"
#include "module/module_manager.hpp"
#include "network/dns_lookup.hpp"
#include "network/socket_activity.hpp"
#include "network/socket_factory.hpp"
#include "version/windows_version_manager.hpp"
#include <platform/audio_backend.hpp>
#include <platform/crypt_protect_backend.hpp>
#include <platform/console_backend.hpp>
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
        opt_func<void(network::socket_activity activity, const network::address& address)> on_socket_activity{};
        utils::callback_list<void(std::string_view message)> on_debug_string{};
        utils::callback_list<void(const mapped_module& mod, const mapped_section& section, uint64_t address)> on_section_first_execution{};
        opt_func<void(uint64_t address)> on_instruction{};
        opt_func<void(io_device& device, std::u16string_view device_name, ULONG code)> on_ioctrl{};
        opt_func<void(uint32_t fail_code)> on_fast_fail{};
    };

    struct application_settings
    {
        windows_path application{};
        std::u16string argument0{};
        windows_path working_directory{};
        std::vector<std::u16string> arguments{};
        std::optional<std::u16string> command_line{};
        utils::unordered_insensitive_u16string_map<std::u16string> environment{};
        uint32_t process_id{default_guest_process_id};
        uint32_t thread_id{8};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->application);
            buffer.write(this->argument0);
            buffer.write(this->working_directory);
            buffer.write_vector(this->arguments);
            buffer.write_optional(this->command_line);
            buffer.write_map(this->environment);
            buffer.write(this->process_id);
            buffer.write(this->thread_id);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->application);
            buffer.read(this->argument0);
            buffer.read(this->working_directory);
            buffer.read_vector(this->arguments);
            buffer.read_optional(this->command_line);
            buffer.read_map(this->environment);
            buffer.read(this->process_id);
            buffer.read(this->thread_id);
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

        // Timing fidelity vs. speed. With interception on (the default, used by the analyzer) rdtsc reads
        // and KUSER_SHARED_DATA accesses trap so the emulated clock stays exact and deterministic. Turning
        // them off (as the sandbox does) lets the guest read the host TSC natively and KUSD from a plain
        // mapped page, removing those VM exits. The cost is exact/reproducible timing, and KUSD's time
        // fields then only advance in steps of the interrupt thread's 20 ms period.
        bool intercept_rdtsc{true};
        bool intercept_kusd{true};

        std::filesystem::path emulation_root{};
        std::filesystem::path registry_directory{"./registry"};

        // When false, construct without loading the registry hives. Intended for headless harnesses
        // (e.g. fuzzing) that never run the guest and don't read the registry, so they can construct
        // with no emulation root / registry directory on disk. The registry ctor otherwise eagerly
        // parses the mandatory hives (SYSTEM/SOFTWARE/SAM/...).
        bool load_registry{true};

        std::unordered_map<uint16_t, uint16_t> port_mappings{};
        filesystem_mode root_mode{default_root_mode};
        std::vector<mount_spec> mounts{};

        fake_environment_config fake_env{};

        network::network_mode network{network::network_mode::offline};

        // Report Vulkan features DXVK requires to games even where the host GPU lacks them (Mali, older Adreno), so
        // DXVK accepts the adapter. A game that really uses one of them renders incorrectly there.
        bool spoof_gpu_features{false};
    };

    struct provider_mount
    {
        windows_path guest_path{};
        std::unique_ptr<mount_provider> provider{};
        filesystem_mode mode{default_mount_mode};
    };

    struct emulator_interfaces
    {
        std::unique_ptr<utils::clock> clock{};
        std::unique_ptr<network::dns_lookup> dns_lookup{};
        std::unique_ptr<network::socket_factory> socket_factory{};
        std::unique_ptr<ui_backend> ui{};
        std::unique_ptr<audio_backend> audio{};
        std::unique_ptr<crypt_protect_backend> crypt_protect{};
        process_manager* processes{};
        std::unique_ptr<console_backend> console{};
        // Mounted after emulator_settings::mounts, for sources that are not a host directory.
        std::vector<provider_mount> mounts{};
    };

    // Per-vCPU scheduler state: the guest thread a virtual CPU is currently executing
    // and its yield request. Replaces the previous global "active thread" notion
    // (docs/multi-vcpu-design.md, section 5.1).
    struct vcpu_context
    {
        guest_cpu& cpu;
        emulator_thread* active_thread{};
        std::atomic_bool switch_thread{false};
        // Set while this vCPU waits in wait_for_thread_registers or runs a host call through run_without_kernel_lock.
        // Its active thread's registers are saved meanwhile, and it reloads them before it continues.
        bool active_thread_saved{false};

        void set_active_thread(emulator_thread* thread)
        {
            this->active_thread = thread;
            if (thread)
            {
                thread->last_vcpu = this;
            }
        }

        emulator_thread& thread() const
        {
            if (!this->active_thread)
            {
                throw std::runtime_error("No active thread!");
            }

            return *this->active_thread;
        }

        x86_64_cpu& x64_cpu() const
        {
            auto* x64 = dynamic_cast<x86_64_cpu*>(&this->cpu);
            if (!x64)
            {
                throw std::runtime_error("This operation is only implemented for x86-64 guest CPUs");
            }

            return *x64;
        }
    };

    // The COFF machine of an application, resolved through its working directory and the settings' emulation
    // root and path mappings as the emulator will resolve it. Selects which guest CPU to create for the application.
    PEMachineType get_application_machine(const emulator_settings& settings, application_settings app_settings);

    // AMD64, ARM64EC and I386 (WoW64) images run on an x86-64 CPU, ARM64 images on an ARM64 CPU.
    machine_type get_machine_type(PEMachineType machine);

    class windows_emulator
    {
        uint64_t executed_instructions_{0};
        application_settings application_settings_{};

        std::unique_ptr<guest_machine> emu_{};
        bool use_native_timestamp_counter_{};
        std::unique_ptr<scheduler_clock> clock_{};
        network::network_mode network_mode_{};
        std::unique_ptr<network::dns_lookup> dns_lookup_{};
        std::unique_ptr<network::socket_factory> socket_factory_{};
        std::unique_ptr<ui_backend> ui_backend_{};
        frame_pacer frame_pacer_{};
        std::unique_ptr<audio_backend> audio_backend_{};
        std::unique_ptr<crypt_protect_backend> crypt_protect_backend_{};
        process_manager* process_manager_{};
        std::unique_ptr<console_backend> console_backend_{};
        bool setup_completed_{false};

        // Host threads owned by the process, such as audio streams, signal the guest through these, so they have to
        // outlive it.
        std::mutex idle_wakeup_mutex_{};
        std::condition_variable idle_wakeup_cv_{};
        uint32_t pending_idle_wakeups_{};
        std::atomic<uint32_t> idle_vcpus_{0};
        std::mutex pending_event_signals_mutex_{};
        std::map<uint64_t, uint32_t> pending_event_signals_{};

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

        windows_emulator(std::unique_ptr<guest_machine> emu, const emulator_settings& settings = {}, emulator_callbacks callbacks = {},
                         emulator_interfaces interfaces = {});
        windows_emulator(std::unique_ptr<guest_machine> emu, application_settings app_settings, const emulator_settings& settings = {},
                         emulator_callbacks callbacks = {}, emulator_interfaces interfaces = {});

        windows_emulator(windows_emulator&&) = delete;
        windows_emulator(const windows_emulator&) = delete;
        windows_emulator& operator=(windows_emulator&&) = delete;
        windows_emulator& operator=(const windows_emulator&) = delete;

        ~windows_emulator();

        guest_machine& emu()
        {
            return *this->emu_;
        }

        const guest_machine& emu() const
        {
            return *this->emu_;
        }

        x86_64_emulator& x64_emu() const
        {
            auto* x64 = dynamic_cast<x86_64_emulator*>(this->emu_.get());
            if (!x64)
            {
                throw std::runtime_error("This operation is only implemented for x86-64 guest machines");
            }

            return *x64;
        }

        const windows_arch_abi& abi() const
        {
            return this->process.abi();
        }

        utils::clock& clock()
        {
            return *this->clock_;
        }

        [[nodiscard]] scheduler_clock& scheduling_clock()
        {
            return *this->clock_;
        }

        const utils::clock& clock() const
        {
            return *this->clock_;
        }

        network::network_mode network_mode() const
        {
            return this->network_mode_;
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

        frame_pacer& frames()
        {
            return this->frame_pacer_;
        }

        const ui_backend& ui() const
        {
            return *this->ui_backend_;
        }

        audio_backend& audio()
        {
            return *this->audio_backend_;
        }

        const audio_backend& audio() const
        {
            return *this->audio_backend_;
        }

        crypt_protect_backend& crypt_protect()
        {
            return *this->crypt_protect_backend_;
        }

        const crypt_protect_backend& crypt_protect() const
        {
            return *this->crypt_protect_backend_;
        }

        process_manager* processes() const
        {
            return this->process_manager_;
        }

        console_backend& console()
        {
            return *this->console_backend_;
        }

        const console_backend& console() const
        {
            return *this->console_backend_;
        }

        void handle_ui_event(const ui_event& event);
        void handle_ui_event_locked(const ui_event& event);
        void update_key_state(const ui_event& event);
        ui_event resolve_injected_event(const ui_event& event) const;
        void apply_cursor_capture();

        // The highest address the process's own code can reach: 32-bit code only reaches the low 2GB unless the
        // executable is large-address aware.
        uint64_t highest_user_address() const;
        void release_held_input();

        // Input held for low-level hooks: the next hook the given thread has to run, marked as running.
        std::optional<std::pair<process_context::low_level_input, user_low_level_hook>> begin_low_level_hook(emulator_thread& thread);
        void finish_low_level_hook(emulator_thread& thread, bool swallow);
        bool has_low_level_hook_work(uint32_t thread_id);
        emulator_thread* drain_low_level_input();
        void deliver_raw_input(const process_context::raw_input_payload& payload, hwnd explicit_target);
        void deliver_raw_mouse_input(int32_t dx, int32_t dy, uint16_t button_flags, uint16_t button_data = 0);
        void deliver_raw_keyboard_input(uint16_t vkey, uint16_t scan_code, uint32_t message, bool extended);

        // Observer convenience for external consumers (gdb stub, analyzer, python
        // bindings) and legacy paths that don't thread a vcpu_context through. Resolves
        // to the vCPU whose handler is currently running: all handler/observer code runs
        // under the kernel lock, so exactly one vCPU dispatches at a time and this is
        // race-free. Falls back to vCPU 0 when no handler is active (e.g. setup).
        emulator_thread& current_thread() const
        {
            return this->active_vcpu().thread();
        }

        // The CPU of the vCPU currently dispatching a handler on the calling host thread (see
        // scoped_dispatch). emu() is only the facade/vCPU 0, so handlers that inspect the faulting
        // register state must go through here to observe the right vCPU when vcpu_count > 1.
        guest_cpu& active_cpu() const
        {
            return this->active_vcpu().cpu;
        }

        x86_64_cpu& active_x64_cpu() const
        {
            return this->active_vcpu().x64_cpu();
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

        template <typename Function>
        auto synchronize(Function&& fn)
        {
            const std::scoped_lock lock(this->kernel_lock_);
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

        // Signal a guest event from a host-owned thread (e.g. the audio render thread). The request is queued and
        // applied by the scheduler under the kernel lock, so it cannot race a concurrent close on an emulator
        // thread; a handle the guest has already closed is simply ignored. Taking the kernel lock here instead
        // would either stall the caller behind long kernel operations or drop the signal while it is busy.
        // Requests count as separate wakes: a new one is only delivered once the guest consumed the previous
        // one, so signals queued while the lock was busy don't coalesce in the event. At most maximum_pending
        // wakes are kept, for clients that stop waiting.
        void request_guest_event_signal(handle event_handle, uint32_t maximum_pending);
        void apply_pending_guest_event_signals();

        // For state restored from outside (a minidump): the process already exists, so it is not bootstrapped.
        void mark_process_setup_completed()
        {
            this->setup_completed_ = true;
        }

        void wake_idle_vcpu();

        uint64_t get_executed_instructions() const
        {
            return this->executed_instructions_;
        }

        bool uses_instruction_precision() const
        {
            return this->instruction_precision_;
        }

        bool uses_relative_time() const
        {
            return this->use_relative_time_;
        }

        bool spoofs_gpu_features() const
        {
            return this->spoof_gpu_features_;
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

        // Stops the process for a snapshot: right away, or at the end of the next frame of a device that waits for one.
        void request_suspend();
        // Called by such a device at the end of a frame.
        bool take_suspend_request();
        void cancel_suspend_request();

        void prepare_serialization();
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

        // Runs f with the kernel lock released, for host calls that can block for a long time (e.g. a GPU present)
        // and would otherwise stall every other vCPU. f must not touch any emulator or guest state. Requires the
        // kernel lock, which is held again when this returns. Other vCPUs dispatch meanwhile, so the dispatch
        // marker is set aside for that time. The dispatching vCPU's thread counts as saved meanwhile, so that a
        // context access from another vCPU does not wait for f; its registers, including any change made to them,
        // are reloaded before this returns. If f throws, the vCPU keeps its own registers and such a change is lost.
        // Within a dispatch, only a syscall handler may call this, as an access treats the saved registers as the
        // syscall's hook frame.
        template <typename F>
        decltype(auto) run_without_kernel_lock(F&& f)
        {
            this->kernel_lock_.assert_held();
            auto* dispatch_vcpu = this->dispatch_vcpu_;
            const auto saved = this->save_dispatching_thread(dispatch_vcpu);
            this->dispatch_vcpu_ = nullptr;
            this->kernel_lock_.unlock();

            const auto run_unlocked = [&]() -> decltype(auto) {
                const auto _ = utils::finally([this, dispatch_vcpu, saved] {
                    this->kernel_lock_.lock();
                    this->dispatch_vcpu_ = dispatch_vcpu;
                    if (saved)
                    {
                        dispatch_vcpu->active_thread_saved = false;
                    }
                });
                return f();
            };

            if constexpr (std::is_void_v<decltype(run_unlocked())>)
            {
                run_unlocked();
                if (saved)
                {
                    this->reload_dispatching_thread(*dispatch_vcpu);
                }
            }
            else
            {
                auto result = run_unlocked();
                if (saved)
                {
                    this->reload_dispatching_thread(*dispatch_vcpu);
                }

                return result;
            }
        }

        // Swaps the registers of the outgoing thread on a vCPU for the incoming one. With more than one vCPU the
        // hypercalls run with the kernel lock released: they only touch this vCPU, but cost tens of microseconds and
        // would otherwise serialize every other vCPU behind each thread switch. Both threads are marked as in transit
        // meanwhile. Requires the kernel lock, and incoming must already be the vCPU's active thread.
        void transfer_thread_registers(vcpu_context& vcpu, emulator_thread* outgoing, emulator_thread& incoming);

        // Blocks until the thread's saved registers are current: no vCPU is transferring them and no other vCPU has the
        // thread loaded. A vCPU that has it loaded is asked to switch away, which saves them. The calling vCPU's active
        // thread must already be saved, so that two vCPUs accessing each other's thread do not wait for each other,
        // and the caller must reload it from the saved registers afterwards, since other vCPUs may read and write them
        // during the wait. Requires the kernel lock, which it releases while waiting. Returns false if the emulator
        // stops first, in which case the thread's registers must not be accessed.
        [[nodiscard]] bool wait_for_thread_registers(vcpu_context& vcpu, emulator_thread& thread);

      private:
        bool save_dispatching_thread(vcpu_context* vcpu);
        void reload_dispatching_thread(vcpu_context& vcpu) const;
        void set_foreground_window(hwnd window);
        void deliver_ui_event(const ui_event& event);
        void resize_window_from_host(hwnd window_handle, bool maximized, int32_t client_width, int32_t client_height);
        std::pair<int32_t, int32_t> cursor_after_pointer_event(const ui_event& event) const;
        std::pair<emulator_thread*, const user_low_level_hook*> find_low_level_hook(uint64_t handle);
        void wake_thread(emulator_thread& thread);

        bool use_relative_time_{false}; // TODO: Get rid of that
        bool instruction_precision_{true};
        bool intercept_rdtsc_{true};
        bool intercept_kusd_{true};
        bool spoof_gpu_features_{false};
        uint32_t vcpu_count_{1};
        std::atomic_bool suspend_requested_{false};
        std::atomic_bool should_stop{false};

        // The emulator kernel lock: held by all code touching shared kernel state,
        // released while guest code executes (docs/multi-vcpu-design.md, section 7).
        kernel_lock kernel_lock_{};
        // Notified under the kernel lock whenever a thread's registers become saved or leave transit.
        std::condition_variable_any thread_registers_saved_{};

        void credit_idle_wakeup();
        bool wait_for_idle_wakeup(std::chrono::milliseconds timeout);

        // The vCPU currently running a handler under the kernel lock; drives
        // current_thread(). See scoped_dispatch.
        vcpu_context* dispatch_vcpu_{};

        std::array<exception_trace_entry, 32> exception_trace_{};
        size_t exception_trace_index_{0};

        // unique_ptr because vcpu_context contains an atomic and must stay address-stable.
        std::vector<std::unique_ptr<vcpu_context>> vcpus_{};

        vcpu_context& active_vcpu() const
        {
            return this->dispatch_vcpu_ ? *this->dispatch_vcpu_ : *this->vcpus_[0];
        }

        std::unordered_map<uint16_t, uint16_t> port_mappings_{};

        std::vector<std::byte> process_snapshot_{};
        // std::optional<process_context> process_snapshot_{};

        stop_reason last_stop_reason_{stop_reason::none};
        std::string last_stop_detail_{};

        std::map<uint64_t, std::vector<emulator_hook*>> section_first_execution_hooks_{};

        void setup_hooks();
        void setup_x64_hooks(x86_64_emulator& emu);
        void setup_arm64_hooks(arm64_emulator& emu);
        void handle_arm64_breakpoint(vcpu_context& vcpu);
        void setup_process();
        void vcpu_worker(vcpu_context& vcpu);
        void on_instruction_execution(vcpu_context& vcpu, uint64_t address);
        void on_basic_block_execution(vcpu_context& vcpu, const basic_block& block);

        bool uses_section_first_execution_hooks() const;
        void clear_section_first_execution_hooks();
        void install_section_first_execution_hook(const mapped_module& mod, size_t section_index);
        void install_section_first_execution_hooks();
        void track_section_first_execution(uint64_t address);

        void register_factories(utils::buffer_deserializer& buffer);
        void ensure_serialization_support() const;
    };

} // namespace sogen

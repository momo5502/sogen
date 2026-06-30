#pragma once

#include "std_include.hpp"

#include <arch_emulator.hpp>
#include <hook_interface.hpp>
#include <stop_reason.hpp>
#include <platform/compiler.hpp>
#include <utils/function.hpp>

#include "linux_logger.hpp"
#include "linux_file_system.hpp"
#include "linux_memory_manager.hpp"
#include "linux_process_context.hpp"
#include "linux_syscall_dispatcher.hpp"
#include "signal_dispatch.hpp"
#include "procfs.hpp"
#include "vdso.hpp"
#include "module/linux_module_manager.hpp"

namespace sogen
{

    class linux_port_mapper
    {
      public:
        void map_port(const uint16_t emulator_port, const uint16_t host_port)
        {
            if (emulator_port == 0 || host_port == 0)
            {
                throw std::runtime_error("Linux port mappings require non-zero TCP ports");
            }

            if (const auto old_host = this->get_host_port(emulator_port); old_host != 0)
            {
                this->host_to_emulator_.erase(old_host);
            }
            if (const auto old_emulator = this->get_emulator_port(host_port); old_emulator != 0)
            {
                this->emulator_to_host_.erase(old_emulator);
            }

            this->emulator_to_host_[emulator_port] = host_port;
            this->host_to_emulator_[host_port] = emulator_port;
        }

        uint16_t get_host_port(const uint16_t emulator_port) const
        {
            const auto it = this->emulator_to_host_.find(emulator_port);
            return it == this->emulator_to_host_.end() ? 0 : it->second;
        }

        uint16_t get_emulator_port(const uint16_t host_port) const
        {
            const auto it = this->host_to_emulator_.find(host_port);
            return it == this->host_to_emulator_.end() ? 0 : it->second;
        }

      private:
        std::map<uint16_t, uint16_t> emulator_to_host_{};
        std::map<uint16_t, uint16_t> host_to_emulator_{};
    };

    class linux_emulator
    {
        uint64_t executed_instructions_{0};
        std::unique_ptr<x86_64_emulator> emu_{};

      public:
        std::filesystem::path emulation_root{};
        linux_logger log{};
        linux_file_system file_sys{};
        linux_memory_manager memory;
        linux_module_manager mod_manager;
        linux_process_context process{};
        linux_syscall_dispatcher dispatcher{};
        signal_dispatcher signals{};
        procfs proc_fs{};
        linux_vdso vdso{};
        linux_port_mapper port_mapper{};

        linux_emulator(std::unique_ptr<x86_64_emulator> emu, const std::filesystem::path& emulation_root);
        linux_emulator(std::unique_ptr<x86_64_emulator> emu, const std::filesystem::path& emulation_root,
                       const std::filesystem::path& executable, std::vector<std::string> argv, const std::vector<std::string>& envp);

        linux_emulator(const linux_emulator&) = delete;
        linux_emulator& operator=(const linux_emulator&) = delete;
        linux_emulator(linux_emulator&&) = delete;
        linux_emulator& operator=(linux_emulator&&) = delete;

        ~linux_emulator() = default;

        x86_64_emulator& emu()
        {
            return *this->emu_;
        }

        const x86_64_emulator& emu() const
        {
            return *this->emu_;
        }

        std::function<void(std::string_view data)> on_stdout{};
        std::function<void(std::string_view data)> on_stderr{};
        std::function<instruction_hook_continuation(uint64_t syscall_id, std::string_view syscall_name)> on_syscall{};
        utils::callback_list<void(int signum, uint64_t fault_addr, int si_code)> on_signal{};
        utils::callback_list<void(linux_mapped_module&)> on_module_load{};
        utils::callback_list<void(linux_thread&)> on_thread_create{};
        utils::callback_list<void(linux_thread&)> on_thread_terminated{};
        utils::callback_list<void(uint32_t old_tid, uint32_t new_tid)> on_thread_switch{};

        // Callback invoked periodically during emulation (every 0x20000 instructions).
        // Used by the web debugger for ASYNCIFY yield and event handling.
        std::function<void()> on_periodic_event{};

        void start(size_t count = 0);
        void stop();
        linux_thread* current_thread() const;
        std::optional<uint32_t> current_thread_id() const;
        bool activate_thread(uint32_t tid);
        bool perform_thread_switch();
        void yield_thread();

        void serialize(utils::buffer_serializer& buffer, bool is_snapshot) const;
        void deserialize(utils::buffer_deserializer& buffer, bool is_snapshot);

        uint64_t get_executed_instructions() const
        {
            return this->executed_instructions_;
        }

        stop_reason last_stop_reason() const
        {
            return this->last_stop_reason_;
        }

        const std::string& last_stop_detail() const
        {
            return this->last_stop_detail_;
        }

        void record_stop(stop_reason reason, std::string detail = {});
        void load_application(const std::filesystem::path& executable, std::vector<std::string> argv, const std::vector<std::string>& envp);

        uint64_t add_memory_violation_observer(
            std::function<memory_violation_continuation(uint64_t, size_t, memory_operation, memory_violation_type)> observer);
        void remove_memory_violation_observer(uint64_t id);

      private:
        std::atomic_bool should_stop{false};
        stop_reason last_stop_reason_{stop_reason::none};
        std::string last_stop_detail_{};
        uint64_t next_memory_violation_observer_id_{1};
        std::vector<
            std::pair<uint64_t, std::function<memory_violation_continuation(uint64_t, size_t, memory_operation, memory_violation_type)>>>
            memory_violation_observers_{};
        memory_violation_continuation notify_memory_violation_observers(uint64_t address, size_t size, memory_operation operation,
                                                                        memory_violation_type type);
        void initialize_cpu_and_filesystem();
        void setup_hooks();
        void on_instruction_execution(uint64_t address);
        void resolve_irelative_relocations();
    };

} // namespace sogen

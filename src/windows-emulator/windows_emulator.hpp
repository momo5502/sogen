#pragma once
#include "std_include.hpp"

#include <arch_emulator.hpp>

#include <utils/function.hpp>

#include "syscall_dispatcher.hpp"
#include "process_context.hpp"
#include "logger.hpp"
#include "file_system.hpp"
#include "memory_manager.hpp"
#include "module/module_manager.hpp"
#include "network/socket_factory.hpp"
#include "version/windows_version_manager.hpp"

struct io_device;

#define opt_func utils::optional_function

struct emulator_callbacks : module_manager::callbacks, process_context::callbacks
{
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
    opt_func<void(std::string_view message)> on_debug_string{};
    opt_func<void(uint64_t address)> on_instruction{};
    opt_func<void(io_device& device, std::u16string_view device_name, ULONG code)> on_ioctrl{};
};

struct application_settings
{
    windows_path application{};
    windows_path working_directory{};
    std::vector<std::u16string> arguments{};

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->application);
        buffer.write(this->working_directory);
        buffer.write_vector(this->arguments);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->application);
        buffer.read(this->working_directory);
        buffer.read_vector(this->arguments);
    }
};

struct emulator_settings
{
    bool disable_logging{false};
    bool use_relative_time{false};

    std::filesystem::path emulation_root{};
    std::filesystem::path registry_directory{"./registry"};

    std::unordered_map<uint16_t, uint16_t> port_mappings{};
    std::unordered_map<windows_path, std::filesystem::path> path_mappings{};
};

struct emulator_interfaces
{
    std::unique_ptr<utils::clock> clock{};
    std::unique_ptr<network::socket_factory> socket_factory{};
};

class windows_emulator
{
    uint64_t executed_instructions_{0};
    std::optional<application_settings> application_settings_{};

    std::unique_ptr<x86_64_emulator> emu_{};
    std::unique_ptr<utils::clock> clock_{};
    std::unique_ptr<network::socket_factory> socket_factory_{};

  public:
    std::vector<uint8_t> sid{}; // TODO: Isolate into some user management component
    std::filesystem::path emulation_root{};
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

    network::socket_factory& socket_factory()
    {
        return *this->socket_factory_;
    }

    const network::socket_factory& socket_factory() const
    {
        return *this->socket_factory_;
    }

    emulator_thread& current_thread() const
    {
        if (!this->process.active_thread)
        {
            throw std::runtime_error("No active thread!");
        }

        return *this->process.active_thread;
    }

    uint64_t get_executed_instructions() const
    {
        return this->executed_instructions_;
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

    void yield_thread(bool alertable = false);
    bool perform_thread_switch();
    bool activate_thread(uint32_t id);

  private:
    bool switch_thread_{false};
    bool use_relative_time_{false}; // TODO: Get rid of that
    std::atomic_bool should_stop{false};

    std::unordered_map<uint16_t, uint16_t> port_mappings_{};

    std::vector<std::byte> process_snapshot_{};
    // std::optional<process_context> process_snapshot_{};

    void setup_hooks();
    void setup_process(const application_settings& app_settings);
    void on_instruction_execution(uint64_t address);

    void register_factories(utils::buffer_deserializer& buffer);
};

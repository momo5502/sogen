#pragma once

#include "sogen_bindings.hpp"
#include <utils/function.hpp>

namespace sogen::py
{

    // RAII wrapper for emulator hooks. Removes the hook on destruction.
    struct hook_handle
    {
        struct hook_state
        {
            windows_emulator* emu{};
            emulator_hook* hook{};

            hook_state() = default;
            hook_state(windows_emulator& emulator, emulator_hook* hook);
            ~hook_state();

            hook_state(const hook_state&) = delete;
            hook_state& operator=(const hook_state&) = delete;

            void remove();
            bool active() const
            {
                return this->hook != nullptr;
            }
        };

        std::shared_ptr<hook_state> shared_state{};
        nb::object owner = nb::none();

        hook_handle() = default;
        hook_handle(windows_emulator& emulator, emulator_hook* hook, nb::object owner);

        bool active() const
        {
            return this->shared_state && this->shared_state->active();
        }
        void remove() const;
    };

    // Information about a single API call passed to the user's Python callback.
    struct api_call_info
    {
        std::string module{};
        std::string name{};
        uint64_t address{};
        uint64_t return_address{};
        uint64_t stack_pointer{};
        uint64_t return_value{0};
    };

    struct api_hook_target
    {
        std::optional<std::string> module{};
        std::string name{};
    };

    struct api_hook_signature
    {
        function_calling_convention cc{function_calling_convention::x64_fastcall};
        nb::object params = nb::none();

        api_hook_signature() = default;
    };

    struct api_hook_hit
    {
        std::string key{};
        std::string module_name{};
        std::string export_name{};
        uint64_t address{};
        uint64_t return_address{};
    };

    struct api_hook_entry
    {
        std::optional<std::string> module_filter{};
        std::string name{};
        function_calling_convention cc{function_calling_convention::x64_fastcall};
        nb::object params = nb::none();
        nb::object callback = nb::none();
        std::vector<std::pair<uint64_t, hook_handle>> hooks{};

        api_hook_entry() = default;
        api_hook_entry(const api_hook_entry&) = delete;
        api_hook_entry& operator=(const api_hook_entry&) = delete;
        api_hook_entry(api_hook_entry&&) noexcept = default;
        api_hook_entry& operator=(api_hook_entry&&) noexcept = default;
    };

    // Dispatches API hooks by matching executed instruction addresses against
    // every module's address_names map (mirrors the analyzer's behavior so
    // forwarders / thunks are handled naturally).
    struct api_hook_registry
    {
        windows_emulator* win_emu{};
        std::map<std::string, api_hook_entry, std::less<>> entries{};
        std::map<uint64_t, std::vector<api_hook_hit>> address_index{};
        std::optional<hook_handle> execution_hook{};
        utils::callback_id_type module_load_id{};
        utils::callback_id_type module_unload_id{};

        api_hook_registry() = delete;
        api_hook_registry(const api_hook_registry&) = delete;
        api_hook_registry& operator=(const api_hook_registry&) = delete;
        api_hook_registry(api_hook_registry&&) = delete;
        api_hook_registry& operator=(api_hook_registry&&) = delete;

        explicit api_hook_registry(windows_emulator& emulator);
        ~api_hook_registry();

        void clear();
        void del_item(const std::string& key);
        void set_item(const std::string& key, nb::object callback);

      private:
        nb::list resolve_params(const api_hook_entry& entry) const;
        void return_from_api(const api_call_info& call) const;
        void invoke_hook(const api_hook_hit& hit);
        void refresh_index();
        void ensure_execution_hook();
        void remove_execution_hook();
        void add_entry_for_module(const std::string& key, api_hook_entry& entry, const mapped_module& module);
        void dispatch_address(uint64_t address);
    };

    // Wrapper holding bound hook lifetimes for the various low-level emulator hooks
    // (memory_execution, instruction, basic_block, ...).
    struct hook_registry
    {
        windows_emulator* emu{};
        std::shared_ptr<api_hook_registry> apis{};
        std::vector<hook_handle> active_hooks{};

        explicit hook_registry(windows_emulator& emulator);

        hook_handle make_hook(emulator_hook* hook);

        hook_handle memory_execution(nb::object callback);
        hook_handle memory_execution_at(uint64_t address, nb::object callback);
        hook_handle memory_read(uint64_t address, uint64_t size, nb::object callback);
        hook_handle memory_write(uint64_t address, uint64_t size, nb::object callback);
        hook_handle instruction(int instruction_type, nb::object callback);
        hook_handle interrupt(nb::object callback);
        hook_handle memory_violation(nb::object callback);
        hook_handle basic_block(nb::object callback);
    };

    // Holds Python callbacks for windows_emulator events. Construction installs
    // dispatchers that forward to the assigned slots; assignments are routed
    // through a small member-pointer table so we don't repeat the callback
    // names in many places.
    struct callback_registry
    {
        windows_emulator* emu{};
        nb::object module_load_cb = nb::none();
        nb::object module_unload_cb = nb::none();
        nb::object stdout_cb = nb::none();
        nb::object syscall_cb = nb::none();
        nb::object generic_access_cb = nb::none();
        nb::object generic_activity_cb = nb::none();
        nb::object suspicious_activity_cb = nb::none();
        nb::object exception_cb = nb::none();
        nb::object instruction_cb = nb::none();
        nb::object memory_protect_cb = nb::none();
        nb::object memory_allocate_cb = nb::none();
        nb::object memory_violate_cb = nb::none();
        nb::object rdtsc_cb = nb::none();
        nb::object rdtscp_cb = nb::none();
        nb::object ioctrl_cb = nb::none();
        nb::object debug_string_cb = nb::none();
        nb::object thread_create_cb = nb::none();
        nb::object thread_terminated_cb = nb::none();
        nb::object thread_set_name_cb = nb::none();
        nb::object thread_switch_cb = nb::none();

        explicit callback_registry(windows_emulator& emulator);

        // name accepts either "stdout" or "on_stdout"; passing nb::none()
        // clears the slot. Throws on unknown name or non-callable value.
        void set(std::string_view name, nb::object callable);
        void clear(std::string_view name);

        // Resolves a slot name to the corresponding nb::object member, or returns
        // nullptr if the name is unknown. Used by both `set` and the property
        // bindings.
        static nb::object callback_registry::* slot_for(std::string_view name);
    };

    struct sogen_process_context
    {
        process_context* ctx{};
        std::shared_ptr<callback_registry> callbacks{};
        nb::object owner = nb::none();

        sogen_process_context(process_context& context, std::shared_ptr<callback_registry> callback_registry, nb::object owner);

        bool is_wow64_process() const;
        std::optional<NTSTATUS> exit_status() const;
        size_t live_thread_count() const;
        uint32_t spawned_thread_count() const;
        emulator_thread* active_thread() const;
        callback_registry& callback_view() const;
    };

    struct sogen_windows_emulator
    {
        std::unique_ptr<windows_emulator> emu{};
        std::shared_ptr<callback_registry> callbacks{};
        std::shared_ptr<hook_registry> hooks{};

        explicit sogen_windows_emulator(std::unique_ptr<windows_emulator> emulator);

        ~sogen_windows_emulator();

        sogen_windows_emulator(const sogen_windows_emulator&) = delete;
        sogen_windows_emulator& operator=(const sogen_windows_emulator&) = delete;
        sogen_windows_emulator(sogen_windows_emulator&&) noexcept;
        sogen_windows_emulator& operator=(sogen_windows_emulator&&) noexcept;

        windows_emulator& native() const;

        void start(size_t count = 0) const;
        void stop() const;
        void save_snapshot() const;
        void restore_snapshot() const;
        nb::bytes serialize_state() const;
        void deserialize_state(const nb::bytes& buffer) const;
        void setup_process_if_necessary() const;
        void yield_thread(bool alertable = false) const;
        bool perform_thread_switch() const;
        bool activate_thread(uint32_t id) const;

        sogen_process_context process();

        memory_manager& memory() const;
        emulator_thread* current_thread() const;
        std::optional<uint32_t> current_thread_id() const;

        nb::bytes read_memory(uint64_t address, size_t size) const;
        void write_memory(uint64_t address, const nb::bytes& buffer) const;
        uint64_t read_register(x86_register reg) const;
        void write_register(x86_register reg, uint64_t value) const;

        uint16_t get_host_port(uint16_t emulator_port) const;
        uint16_t get_emulator_port(uint16_t host_port) const;
        void map_port(uint16_t emulator_port, uint16_t host_port) const;
    };

    struct linux_callback_registry
    {
        linux_emulator* emu{};
        nb::object stdout_cb = nb::none();
        nb::object stderr_cb = nb::none();
        nb::object syscall_cb = nb::none();

        explicit linux_callback_registry(linux_emulator& emulator);

        void set(std::string_view name, nb::object callable);
        void clear(std::string_view name);

        static nb::object linux_callback_registry::* slot_for(std::string_view name);
    };

    struct sogen_linux_process_context
    {
        linux_process_context* ctx{};
        nb::object owner = nb::none();

        sogen_linux_process_context(linux_process_context& context, nb::object owner);

        std::optional<int> exit_status() const;
        uint32_t pid() const;
        uint32_t ppid() const;
        uint32_t uid() const;
        uint32_t gid() const;
        uint32_t euid() const;
        uint32_t egid() const;
        size_t thread_count() const;
        linux_thread* active_thread() const;
    };

    struct sogen_linux_emulator
    {
        std::unique_ptr<linux_emulator> emu{};
        std::shared_ptr<linux_callback_registry> callbacks{};

        explicit sogen_linux_emulator(std::unique_ptr<linux_emulator> emulator);

        ~sogen_linux_emulator();

        sogen_linux_emulator(const sogen_linux_emulator&) = delete;
        sogen_linux_emulator& operator=(const sogen_linux_emulator&) = delete;
        sogen_linux_emulator(sogen_linux_emulator&&) noexcept;
        sogen_linux_emulator& operator=(sogen_linux_emulator&&) noexcept;

        linux_emulator& native() const;

        void start(size_t count = 0) const;
        void stop() const;

        sogen_linux_process_context process();

        linux_memory_manager& memory() const;
        nb::bytes read_memory(uint64_t address, size_t size) const;
        void write_memory(uint64_t address, const nb::bytes& buffer) const;
        uint64_t read_register(x86_register reg) const;
        void write_register(x86_register reg, uint64_t value) const;
        uint64_t executed_instructions() const;
        std::string backend_name() const;
        std::filesystem::path emulation_root() const;
    };
}

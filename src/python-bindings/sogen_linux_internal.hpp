#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <backend_selection.hpp>
#include <disassembler.hpp>
#include <hook_interface.hpp>
#include <linux_emulator.hpp>
#include <linux_emulator_callbacks.hpp>
#include <linux_syscall_info.hpp>
#include <x86_register.hpp>

namespace nb = nanobind;

namespace sogen::py
{
    using sogen::backend_type;
    using sogen::basic_block;
    using sogen::emulator_hook;
    using sogen::instruction_hook_continuation;
    using sogen::linux_emulator;
    using sogen::linux_exported_symbol;
    using sogen::linux_mapped_module;
    using sogen::linux_mapped_section;
    using sogen::linux_memory_manager;
    using sogen::linux_process_context;
    using sogen::linux_syscall_hook_continuation;
    using sogen::linux_syscall_info;
    using sogen::linux_thread;
    using sogen::memory_interface;
    using sogen::memory_operation;
    using sogen::memory_permission;
    using sogen::memory_violation_continuation;
    using sogen::memory_violation_type;
    using sogen::x86_hookable_instructions;
    using sogen::x86_register;

    enum class linux_syscall_continuation
    {
        run_handler,
        skip_handler,
        stop,
    };

    struct linux_hook_handle
    {
        linux_emulator* emu{};
        emulator_hook* hook{};
        nb::object owner = nb::none();

        linux_hook_handle() = default;
        linux_hook_handle(linux_emulator& emulator, emulator_hook* hook, nb::object owner);
        ~linux_hook_handle();

        linux_hook_handle(const linux_hook_handle&) = delete;
        linux_hook_handle& operator=(const linux_hook_handle&) = delete;
        linux_hook_handle(linux_hook_handle&& other) noexcept;
        linux_hook_handle& operator=(linux_hook_handle&& other) noexcept;

        bool active() const
        {
            return this->hook != nullptr;
        }
        void remove();
    };

    struct linux_syscall_hook_registry
    {
        linux_emulator* emu{};
        std::map<std::string, nb::object, std::less<>> entries{};

        explicit linux_syscall_hook_registry(linux_emulator& emulator);

        void clear();
        void del_item(const std::string& key);
        void set_item(const std::string& key, nb::object callback);

        linux_syscall_hook_continuation invoke(const linux_syscall_info& info) const;
    };

    struct linux_hook_registry
    {
        linux_emulator* emu{};
        std::shared_ptr<linux_syscall_hook_registry> syscalls{};

        explicit linux_hook_registry(linux_emulator& emulator);

        linux_hook_handle make_hook(emulator_hook* hook);

        linux_hook_handle memory_execution(nb::object callback);
        linux_hook_handle memory_execution_at(uint64_t address, nb::object callback);
        linux_hook_handle memory_read(uint64_t address, uint64_t size, nb::object callback);
        linux_hook_handle memory_write(uint64_t address, uint64_t size, nb::object callback);
        linux_hook_handle instruction(int instruction_type, nb::object callback);
        linux_hook_handle interrupt(nb::object callback);
        linux_hook_handle memory_violation(nb::object callback);
        linux_hook_handle basic_block(nb::object callback);
    };

    struct linux_callback_registry
    {
        linux_emulator* emu{};
        std::shared_ptr<linux_syscall_hook_registry> syscall_hooks{};

        nb::object stdout_cb = nb::none();
        nb::object stderr_cb = nb::none();
        nb::object syscall_cb = nb::none();
        nb::object module_load_cb = nb::none();
        nb::object instruction_cb = nb::none();
        nb::object memory_violate_cb = nb::none();

        explicit linux_callback_registry(linux_emulator& emulator, std::shared_ptr<linux_syscall_hook_registry> syscall_hooks);

        void set(std::string_view name, nb::object callable);
        void clear(std::string_view name);

        static nb::object linux_callback_registry::* slot_for(std::string_view name);
    };

    nb::bytes read_memory_bytes(const memory_interface& memory, uint64_t address, size_t size);
    void write_memory_bytes(memory_interface& memory, uint64_t address, const nb::bytes& buffer);
    instruction_hook_continuation coerce_instruction_continuation(nb::handle result);
    memory_violation_continuation coerce_memory_violation_continuation(nb::handle result);
    linux_syscall_hook_continuation coerce_linux_syscall_continuation(nb::handle result);
    std::unique_ptr<linux_emulator> create_linux_application_emulator(const nb::object& application, const nb::object& args,
                                                                      const nb::kwargs& kwargs);

    struct sogen_linux_process_context
    {
        linux_process_context* ctx{};
        nb::object owner = nb::none();

        sogen_linux_process_context(linux_process_context& context, nb::object owner);

        std::optional<int> exit_status() const
        {
            return this->ctx->exit_status;
        }
        uint32_t pid() const
        {
            return this->ctx->pid;
        }
        linux_thread* active_thread() const
        {
            return this->ctx->active_thread;
        }
    };

    struct sogen_linux_emulator
    {
        std::unique_ptr<linux_emulator> emu{};
        std::shared_ptr<linux_hook_registry> hooks{};
        std::shared_ptr<linux_callback_registry> callbacks{};

        explicit sogen_linux_emulator(std::unique_ptr<linux_emulator> emulator);

        linux_emulator& native() const
        {
            return *this->emu;
        }

        void start(size_t count = 0) const
        {
            this->emu->start(count);
        }
        void stop() const
        {
            this->emu->stop();
        }

        sogen_linux_process_context process() const;

        linux_memory_manager& memory() const
        {
            return this->emu->memory;
        }
        linux_thread* current_thread() const
        {
            return this->emu->process.active_thread;
        }
        std::optional<uint32_t> current_thread_id() const;

        nb::bytes read_memory(uint64_t address, size_t size) const
        {
            return read_memory_bytes(this->emu->memory, address, size);
        }
        void write_memory(uint64_t address, const nb::bytes& buffer) const
        {
            write_memory_bytes(this->emu->memory, address, buffer);
        }
        uint64_t read_register(x86_register reg) const
        {
            return this->emu->emu().reg<uint64_t>(reg);
        }
        void write_register(x86_register reg, uint64_t value) const
        {
            this->emu->emu().reg<uint64_t>(reg, value);
        }

        uint16_t get_host_port(uint16_t emulator_port) const
        {
            return this->emu->get_host_port(emulator_port);
        }
        uint16_t get_emulator_port(uint16_t host_port) const
        {
            return this->emu->get_emulator_port(host_port);
        }
        void map_port(uint16_t emulator_port, uint16_t host_port) const
        {
            this->emu->map_port(emulator_port, host_port);
        }
    };

    void register_linux_runtime_bindings(nb::module_& m);
}

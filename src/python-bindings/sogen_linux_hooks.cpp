#include "sogen_linux_internal.hpp"

namespace sogen::py
{
    linux_hook_handle::linux_hook_handle(linux_emulator& emulator, emulator_hook* hook, nb::object owner)
        : emu(&emulator),
          hook(hook),
          owner(std::move(owner))
    {
    }

    linux_hook_handle::~linux_hook_handle()
    {
        this->remove();
    }

    linux_hook_handle::linux_hook_handle(linux_hook_handle&& other) noexcept
        : emu(other.emu),
          hook(other.hook),
          owner(std::move(other.owner))
    {
        other.hook = nullptr;
    }

    linux_hook_handle& linux_hook_handle::operator=(linux_hook_handle&& other) noexcept
    {
        if (this != &other)
        {
            this->remove();
            this->emu = other.emu;
            this->hook = other.hook;
            this->owner = std::move(other.owner);
            other.hook = nullptr;
        }

        return *this;
    }

    void linux_hook_handle::remove()
    {
        if (!this->emu || !this->hook)
        {
            return;
        }

        try
        {
            this->emu->emu().delete_hook(this->hook);
        }
        catch (...)
        {
        }

        this->hook = nullptr;
    }

    linux_syscall_hook_registry::linux_syscall_hook_registry(linux_emulator& emulator)
        : emu(&emulator)
    {
    }

    void linux_syscall_hook_registry::clear()
    {
        this->entries.clear();
    }

    void linux_syscall_hook_registry::del_item(const std::string& key)
    {
        this->entries.erase(key);
    }

    void linux_syscall_hook_registry::set_item(const std::string& key, nb::object callback)
    {
        if (callback.is_none())
        {
            this->entries.erase(key);
            return;
        }

        if (!callback.is_none() && !PyCallable_Check(callback.ptr()))
        {
            throw std::invalid_argument("Syscall hook callback must be callable");
        }

        this->entries[key] = std::move(callback);
    }

    linux_syscall_hook_continuation linux_syscall_hook_registry::invoke(const linux_syscall_info& info) const
    {
        const auto it = this->entries.find(std::string{info.name});
        if (it == this->entries.end() || it->second.is_none())
        {
            return linux_syscall_hook_continuation::run_handler;
        }

        nb::gil_scoped_acquire gil{};
        return coerce_linux_syscall_continuation(it->second(nb::cast(info)));
    }

    linux_hook_registry::linux_hook_registry(linux_emulator& emulator)
        : emu(&emulator),
          syscalls(std::make_shared<linux_syscall_hook_registry>(emulator))
    {
    }

    linux_hook_handle linux_hook_registry::make_hook(emulator_hook* hook)
    {
        return {*this->emu, hook, nb::cast(this, nb::rv_policy::reference_internal)};
    }

    linux_hook_handle linux_hook_registry::memory_execution(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_execution([cb = std::move(callback)](const uint64_t address) {
            nb::gil_scoped_acquire gil{};
            cb(address);
        });
        return make_hook(hook);
    }

    linux_hook_handle linux_hook_registry::memory_execution_at(const uint64_t address, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_execution(address, [cb = std::move(callback)](const uint64_t addr) {
            nb::gil_scoped_acquire gil{};
            cb(addr);
        });
        return make_hook(hook);
    }

    linux_hook_handle linux_hook_registry::memory_read(const uint64_t address, const uint64_t size, nb::object callback)
    {
        auto* hook =
            this->emu->emu().hook_memory_read(address, size, [cb = std::move(callback)](const uint64_t addr, const void* data, size_t length) {
                nb::gil_scoped_acquire gil{};
                cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
            });
        return make_hook(hook);
    }

    linux_hook_handle linux_hook_registry::memory_write(const uint64_t address, const uint64_t size, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_write(address, size,
                                                          [cb = std::move(callback)](const uint64_t addr, const void* data, size_t length) {
                                                              nb::gil_scoped_acquire gil{};
                                                              cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
                                                          });
        return make_hook(hook);
    }

    linux_hook_handle linux_hook_registry::instruction(const int instruction_type, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_instruction(static_cast<x86_hookable_instructions>(instruction_type),
                                                       [cb = std::move(callback)](const uint64_t data) {
                                                           nb::gil_scoped_acquire gil{};
                                                           return coerce_instruction_continuation(cb(data));
                                                       });
        return make_hook(hook);
    }

    linux_hook_handle linux_hook_registry::interrupt(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_interrupt([cb = std::move(callback)](const int interrupt) {
            nb::gil_scoped_acquire gil{};
            cb(interrupt);
        });
        return make_hook(hook);
    }

    linux_hook_handle linux_hook_registry::memory_violation(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_violation(
            [cb = std::move(callback)](const uint64_t address, const size_t size, const memory_operation operation,
                                       const memory_violation_type type) {
                nb::gil_scoped_acquire gil{};
                return coerce_memory_violation_continuation(cb(address, size, operation, type));
            });
        return make_hook(hook);
    }

    linux_hook_handle linux_hook_registry::basic_block(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_basic_block([cb = std::move(callback)](const sogen::basic_block& block) {
            nb::gil_scoped_acquire gil{};
            cb(block);
        });
        return make_hook(hook);
    }

    sogen_linux_emulator::sogen_linux_emulator(std::unique_ptr<linux_emulator> emulator)
        : emu(std::move(emulator)),
          hooks(std::make_shared<linux_hook_registry>(*this->emu)),
          callbacks(std::make_shared<linux_callback_registry>(*this->emu, this->hooks->syscalls))
    {
    }

    sogen_linux_process_context sogen_linux_emulator::process() const
    {
        return {this->emu->process, nb::cast(this, nb::rv_policy::reference_internal)};
    }

    std::optional<uint32_t> sogen_linux_emulator::current_thread_id() const
    {
        if (this->emu->process.active_thread == nullptr)
        {
            return std::nullopt;
        }

        return this->emu->process.active_thread->tid;
    }

    sogen_linux_process_context::sogen_linux_process_context(linux_process_context& context, nb::object owner)
        : ctx(&context),
          owner(std::move(owner))
    {
    }
}

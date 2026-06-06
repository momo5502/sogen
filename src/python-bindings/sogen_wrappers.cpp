#include "sogen_internal.hpp"

namespace sogen::py
{
    // ----- hook_registry -----

    hook_registry::hook_registry(windows_emulator& emulator)
        : emu(&emulator),
          apis(std::make_shared<api_hook_registry>(emulator))
    {
    }

    hook_handle hook_registry::make_hook(emulator_hook* hook)
    {
        hook_handle stored{*this->emu, hook, nb::none()};
        this->active_hooks.emplace_back(stored);

        auto exposed = stored;
        exposed.owner = nb::cast(this, nb::rv_policy::reference_internal);
        return exposed;
    }

    hook_handle hook_registry::memory_execution(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_execution([cb = std::move(callback)](uint64_t address) {
            nb::gil_scoped_acquire gil{};
            cb(address);
        });
        return make_hook(hook);
    }

    hook_handle hook_registry::memory_execution_at(uint64_t address, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_execution(address, [cb = std::move(callback)](uint64_t addr) {
            nb::gil_scoped_acquire gil{};
            cb(addr);
        });
        return make_hook(hook);
    }

    hook_handle hook_registry::memory_read(uint64_t address, uint64_t size, nb::object callback)
    {
        auto* hook =
            this->emu->emu().hook_memory_read(address, size, [cb = std::move(callback)](uint64_t addr, const void* data, size_t length) {
                nb::gil_scoped_acquire gil{};
                cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
            });
        return make_hook(hook);
    }

    hook_handle hook_registry::memory_write(uint64_t address, uint64_t size, nb::object callback)
    {
        auto* hook =
            this->emu->emu().hook_memory_write(address, size, [cb = std::move(callback)](uint64_t addr, const void* data, size_t length) {
                nb::gil_scoped_acquire gil{};
                cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
            });
        return make_hook(hook);
    }

    hook_handle hook_registry::instruction(int instruction_type, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_instruction(static_cast<x86_hookable_instructions>(instruction_type),
                                                       [cb = std::move(callback)](uint64_t data) {
                                                           nb::gil_scoped_acquire gil{};
                                                           return coerce_instruction_continuation(cb(data));
                                                       });
        return make_hook(hook);
    }

    hook_handle hook_registry::interrupt(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_interrupt([cb = std::move(callback)](int interrupt) {
            nb::gil_scoped_acquire gil{};
            cb(interrupt);
        });
        return make_hook(hook);
    }

    hook_handle hook_registry::memory_violation(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_violation(
            [cb = std::move(callback)](uint64_t address, size_t size, memory_operation operation, memory_violation_type type) {
                nb::gil_scoped_acquire gil{};
                return coerce_memory_violation_continuation(cb(address, size, operation, type));
            });
        return make_hook(hook);
    }

    hook_handle hook_registry::basic_block(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_basic_block([cb = std::move(callback)](const sogen::basic_block& block) {
            nb::gil_scoped_acquire gil{};
            cb(block);
        });
        return make_hook(hook);
    }

    // ----- sogen_process_context -----

    sogen_process_context::sogen_process_context(process_context& context, std::shared_ptr<callback_registry> callback_registry,
                                                 nb::object owner)
        : ctx(&context),
          callbacks(std::move(callback_registry)),
          owner(std::move(owner))
    {
    }

    // ----- sogen_windows_emulator -----

    sogen_windows_emulator::sogen_windows_emulator(std::unique_ptr<windows_emulator> emulator)
        : emu(std::move(emulator)),
          callbacks(std::make_shared<callback_registry>(*this->emu)),
          hooks(std::make_shared<hook_registry>(*this->emu))
    {
    }

    sogen_process_context sogen_windows_emulator::process()
    {
        return {this->emu->process, this->callbacks, nb::cast(this, nb::rv_policy::reference_internal)};
    }

    std::optional<uint32_t> sogen_windows_emulator::current_thread_id() const
    {
        if (!this->emu->process.active_thread)
        {
            return std::nullopt;
        }

        return this->emu->process.active_thread->id;
    }
}

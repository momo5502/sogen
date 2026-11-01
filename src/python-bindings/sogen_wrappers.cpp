#include "sogen_internal.hpp"

#include <windows_emulator.hpp>

#include <array>
#include <cstring>

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
        hook_handle stored{this->emu->emu(), hook, nb::none()};
        this->active_hooks.emplace_back(stored);

        auto exposed = stored;
        exposed.owner = nb::cast(this, nb::rv_policy::reference_internal);
        return exposed;
    }

    hook_handle hook_registry::memory_execution(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_execution([cb = std::move(callback)](cpu_interface&, uint64_t address) {
            nb::gil_scoped_acquire gil{};
            cb(address);
        });
        return make_hook(hook);
    }

    hook_handle hook_registry::memory_execution_at(uint64_t address, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_execution(address, [cb = std::move(callback)](cpu_interface&, uint64_t addr) {
            nb::gil_scoped_acquire gil{};
            cb(addr);
        });
        return make_hook(hook);
    }

    hook_handle hook_registry::memory_read(uint64_t address, uint64_t size, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_read(
            address, size, [cb = std::move(callback)](cpu_interface&, uint64_t addr, const void* data, size_t length) {
                nb::gil_scoped_acquire gil{};
                cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
            });
        return make_hook(hook);
    }

    hook_handle hook_registry::memory_write(uint64_t address, uint64_t size, nb::object callback)
    {
        auto* hook = this->emu->emu().hook_memory_write(
            address, size, [cb = std::move(callback)](cpu_interface&, uint64_t addr, const void* data, size_t length) {
                nb::gil_scoped_acquire gil{};
                cb(addr, nb::bytes(static_cast<const char*>(data), static_cast<nb::ssize_t>(length)));
            });
        return make_hook(hook);
    }

    hook_handle hook_registry::instruction(x86_hookable_instructions instruction_type, nb::object callback)
    {
        auto* hook = this->emu->x64_emu().hook_instruction(instruction_type, [cb = std::move(callback)](cpu_interface&, uint64_t data) {
            nb::gil_scoped_acquire gil{};
            return coerce_instruction_continuation(cb(data));
        });
        return make_hook(hook);
    }

    hook_handle hook_registry::interrupt(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_interrupt([cb = std::move(callback)](cpu_interface&, int interrupt) {
            nb::gil_scoped_acquire gil{};
            cb(interrupt);
        });
        return make_hook(hook);
    }

    hook_handle hook_registry::memory_violation(nb::object callback)
    {
        auto* hook =
            this->emu->emu().hook_memory_violation([cb = std::move(callback)](cpu_interface&, uint64_t address, size_t size,
                                                                              memory_operation operation, memory_violation_type type) {
                nb::gil_scoped_acquire gil{};
                return coerce_memory_violation_continuation(cb(address, size, operation, type));
            });
        return make_hook(hook);
    }

    hook_handle hook_registry::basic_block(nb::object callback)
    {
        auto* hook = this->emu->emu().hook_basic_block([cb = std::move(callback)](cpu_interface&, const sogen::basic_block& block) {
            nb::gil_scoped_acquire gil{};
            cb(block);
        });
        return make_hook(hook);
    }

    // ----- sogen_process_context -----

    sogen_process_context::sogen_process_context(windows_emulator& win_emu, std::shared_ptr<callback_registry> callback_registry,
                                                 nb::object owner)
        : emu(&win_emu),
          ctx(&win_emu.process),
          callbacks(std::move(callback_registry)),
          owner(std::move(owner))
    {
    }

    bool sogen_process_context::is_wow64_process() const
    {
        return this->ctx->is_wow64_process;
    }

    std::optional<NTSTATUS> sogen_process_context::exit_status() const
    {
        return this->ctx->exit_status;
    }

    size_t sogen_process_context::live_thread_count() const
    {
        return this->ctx->get_live_thread_count();
    }

    uint32_t sogen_process_context::spawned_thread_count() const
    {
        return this->ctx->spawned_thread_count;
    }

    emulator_thread* sogen_process_context::active_thread() const
    {
        return this->emu->vcpu(0).active_thread;
    }

    callback_registry& sogen_process_context::callback_view() const
    {
        return *this->callbacks;
    }

    // ----- sogen_windows_emulator -----

    sogen_windows_emulator::sogen_windows_emulator(std::unique_ptr<windows_emulator> emulator)
        : emu(std::move(emulator)),
          callbacks(std::make_shared<callback_registry>(*this->emu)),
          hooks(std::make_shared<hook_registry>(*this->emu))
    {
    }

    sogen_windows_emulator::~sogen_windows_emulator() = default;

    sogen_windows_emulator::sogen_windows_emulator(sogen_windows_emulator&&) noexcept = default;

    sogen_windows_emulator& sogen_windows_emulator::operator=(sogen_windows_emulator&&) noexcept = default;

    windows_emulator& sogen_windows_emulator::native() const
    {
        return *this->emu;
    }

    void sogen_windows_emulator::start(const size_t count) const
    {
        this->emu->start(count);
    }

    void sogen_windows_emulator::stop() const
    {
        this->emu->stop();
    }

    void sogen_windows_emulator::save_snapshot() const
    {
        this->emu->save_snapshot();
    }

    void sogen_windows_emulator::restore_snapshot() const
    {
        this->emu->restore_snapshot();
    }

    nb::bytes sogen_windows_emulator::serialize_state() const
    {
        return serialize_state_bytes(*this->emu);
    }

    void sogen_windows_emulator::deserialize_state(const nb::bytes& buffer) const
    {
        deserialize_state_bytes(*this->emu, buffer);
    }

    void sogen_windows_emulator::setup_process_if_necessary() const
    {
        this->emu->setup_process_if_necessary();
    }

    void sogen_windows_emulator::yield_thread(const bool alertable) const
    {
        this->emu->yield_thread(this->emu->vcpu(0), alertable);
    }

    bool sogen_windows_emulator::perform_thread_switch() const
    {
        return this->emu->perform_thread_switch(this->emu->vcpu(0));
    }

    bool sogen_windows_emulator::activate_thread(const uint32_t id) const
    {
        return this->emu->activate_thread(this->emu->vcpu(0), id);
    }

    memory_manager& sogen_windows_emulator::memory() const
    {
        return this->emu->memory;
    }

    emulator_thread* sogen_windows_emulator::current_thread() const
    {
        return this->emu->vcpu(0).active_thread;
    }

    nb::bytes sogen_windows_emulator::read_memory(const uint64_t address, const size_t size) const
    {
        return read_memory_bytes(this->emu->memory, address, size);
    }

    void sogen_windows_emulator::write_memory(const uint64_t address, const nb::bytes& buffer) const
    {
        write_memory_bytes(this->emu->memory, address, buffer);
    }

    uint64_t sogen_windows_emulator::read_register(const x86_register reg) const
    {
        return this->emu->x64_emu().reg<uint64_t>(reg);
    }

    void sogen_windows_emulator::write_register(const x86_register reg, const uint64_t value) const
    {
        this->emu->x64_emu().reg<uint64_t>(reg, value);
    }

    namespace
    {
        // Backends disagree on a buffer narrower than the register: some reject it, some read or write the
        // register's full width regardless. Handing every backend the widest register's worth (ZMM, 64 bytes)
        // keeps each access in bounds and gives one behaviour everywhere: short values are zero-extended, and
        // bytes past the register's width read as zero.
        constexpr size_t max_register_size = 64;
        using register_buffer = std::array<std::byte, max_register_size>;

        void validate_register_size(const size_t size)
        {
            if (size > max_register_size)
            {
                throw std::invalid_argument("Registers are at most " + std::to_string(max_register_size) + " bytes wide");
            }
        }

        template <typename Cpu, typename Register>
        nb::bytes read_padded_register(Cpu& cpu, const Register reg, const size_t size)
        {
            validate_register_size(size);
            register_buffer buffer{};
            cpu.read_register(reg, buffer.data(), buffer.size());
            return nb::bytes(reinterpret_cast<const char*>(buffer.data()), size);
        }

        template <typename Cpu, typename Register>
        void write_padded_register(Cpu& cpu, const Register reg, const nb::bytes& value)
        {
            validate_register_size(value.size());
            register_buffer buffer{};
            std::memcpy(buffer.data(), value.c_str(), value.size());
            cpu.write_register(reg, buffer.data(), buffer.size());
        }
    }

    nb::bytes sogen_windows_emulator::read_register_bytes(const x86_register reg, const size_t size) const
    {
        return read_padded_register(this->emu->x64_emu(), reg, size);
    }

    void sogen_windows_emulator::write_register_bytes(const x86_register reg, const nb::bytes& value) const
    {
        write_padded_register(this->emu->x64_emu(), reg, value);
    }

    namespace
    {
        arm64_cpu& active_arm64_cpu(const windows_emulator& emu)
        {
            auto* cpu = dynamic_cast<arm64_cpu*>(&emu.active_cpu());
            if (!cpu)
            {
                throw std::invalid_argument("ARM64 registers require an ARM64 guest");
            }
            return *cpu;
        }
    }

    uint64_t sogen_windows_emulator::read_register(const arm64_register reg) const
    {
        return active_arm64_cpu(*this->emu).reg<uint64_t>(reg);
    }

    void sogen_windows_emulator::write_register(const arm64_register reg, const uint64_t value) const
    {
        active_arm64_cpu(*this->emu).reg<uint64_t>(reg, value);
    }

    nb::bytes sogen_windows_emulator::read_register_bytes(const arm64_register reg, const size_t size) const
    {
        return read_padded_register(active_arm64_cpu(*this->emu), reg, size);
    }

    void sogen_windows_emulator::write_register_bytes(const arm64_register reg, const nb::bytes& value) const
    {
        write_padded_register(active_arm64_cpu(*this->emu), reg, value);
    }

    uint16_t sogen_windows_emulator::get_host_port(const uint16_t emulator_port) const
    {
        return this->emu->get_host_port(emulator_port);
    }

    uint16_t sogen_windows_emulator::get_emulator_port(const uint16_t host_port) const
    {
        return this->emu->get_emulator_port(host_port);
    }

    void sogen_windows_emulator::map_port(const uint16_t emulator_port, const uint16_t host_port) const
    {
        this->emu->map_port(emulator_port, host_port);
    }

    sogen_process_context sogen_windows_emulator::process()
    {
        return {*this->emu, this->callbacks, nb::cast(this, nb::rv_policy::reference_internal)};
    }

    std::optional<uint32_t> sogen_windows_emulator::current_thread_id() const
    {
        const auto* active_thread = this->emu->vcpu(0).active_thread;
        if (!active_thread)
        {
            return std::nullopt;
        }

        return active_thread->id;
    }
}

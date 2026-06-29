#include "sogen_internal.hpp"

#include <linux_emulator.hpp>

namespace sogen::py
{
    namespace
    {
        nb::object get_kwarg_object(const nb::kwargs& kwargs, const char* name)
        {
            return kwargs.get(name, nb::none());
        }

        template <typename T>
        T get_kwarg(const nb::kwargs& kwargs, const char* name, const T& default_value)
        {
            const auto value = get_kwarg_object(kwargs, name);
            return value.is_none() ? default_value : nb::cast<T>(value);
        }

        std::vector<std::string> parse_linux_arguments(const nb::object& object)
        {
            std::vector<std::string> result{};
            if (object.is_none())
            {
                return result;
            }

            const auto seq = nb::cast<nb::sequence>(object);
            result.reserve(static_cast<size_t>(nb::len(seq)));
            for (const auto& item : seq)
            {
                result.emplace_back(nb::cast<std::string>(item));
            }

            return result;
        }

        std::vector<std::string> parse_linux_environment(const nb::object& object)
        {
            std::vector<std::string> result{};
            if (object.is_none())
            {
                return {"PATH=/usr/bin:/bin", "HOME=/root", "TERM=xterm"};
            }

            const auto dict = nb::cast<nb::dict>(object);
            result.reserve(static_cast<size_t>(nb::len(dict)));
            for (const auto& item : dict)
            {
                result.emplace_back(nb::cast<std::string>(item.first) + "=" + nb::cast<std::string>(item.second));
            }

            return result;
        }

        backend_type get_backend_type(const nb::kwargs& kwargs)
        {
            return get_kwarg<backend_type>(kwargs, "backend", backend_type::unicorn);
        }
    }

    std::unique_ptr<linux_emulator> create_linux_application_emulator(const nb::object& application, const nb::object& args,
                                                                      const nb::kwargs& kwargs)
    {
        const auto application_path = nb::cast<std::filesystem::path>(application);
        auto argv = std::vector<std::string>{application_path.string()};
        auto application_args = parse_linux_arguments(args);
        for (auto& arg : application_args)
        {
            argv.emplace_back(std::move(arg));
        }

        auto envp = parse_linux_environment(get_kwarg_object(kwargs, "environment"));
        auto linux_emu = std::make_unique<linux_emulator>(create_x86_64_emulator(get_backend_type(kwargs)),
                                                          get_kwarg<std::filesystem::path>(kwargs, "emulation_root", {}), application_path,
                                                          std::move(argv), envp);
        linux_emu->log.disable_output(get_kwarg<bool>(kwargs, "disable_logging", true));
        return linux_emu;
    }

    sogen_linux_process_context::sogen_linux_process_context(linux_process_context& context, nb::object owner)
        : ctx(&context),
          owner(std::move(owner))
    {
    }

    std::optional<int> sogen_linux_process_context::exit_status() const
    {
        return this->ctx->exit_status;
    }

    uint32_t sogen_linux_process_context::pid() const
    {
        return this->ctx->pid;
    }

    uint32_t sogen_linux_process_context::ppid() const
    {
        return this->ctx->ppid;
    }

    uint32_t sogen_linux_process_context::uid() const
    {
        return this->ctx->uid;
    }

    uint32_t sogen_linux_process_context::gid() const
    {
        return this->ctx->gid;
    }

    uint32_t sogen_linux_process_context::euid() const
    {
        return this->ctx->euid;
    }

    uint32_t sogen_linux_process_context::egid() const
    {
        return this->ctx->egid;
    }

    size_t sogen_linux_process_context::thread_count() const
    {
        return this->ctx->threads.size();
    }

    linux_thread* sogen_linux_process_context::active_thread() const
    {
        return this->ctx->active_thread;
    }

    sogen_linux_emulator::sogen_linux_emulator(std::unique_ptr<linux_emulator> emulator)
        : emu(std::move(emulator)),
          callbacks(std::make_shared<linux_callback_registry>(*this->emu))
    {
    }

    sogen_linux_emulator::~sogen_linux_emulator() = default;

    sogen_linux_emulator::sogen_linux_emulator(sogen_linux_emulator&&) noexcept = default;

    sogen_linux_emulator& sogen_linux_emulator::operator=(sogen_linux_emulator&&) noexcept = default;

    linux_emulator& sogen_linux_emulator::native() const
    {
        return *this->emu;
    }

    void sogen_linux_emulator::start(const size_t count) const
    {
        this->emu->start(count);
    }

    void sogen_linux_emulator::stop() const
    {
        this->emu->stop();
    }

    sogen_linux_process_context sogen_linux_emulator::process()
    {
        return {this->emu->process, nb::cast(this, nb::rv_policy::reference_internal)};
    }

    linux_memory_manager& sogen_linux_emulator::memory() const
    {
        return this->emu->memory;
    }

    nb::bytes sogen_linux_emulator::read_memory(const uint64_t address, const size_t size) const
    {
        return read_memory_bytes(this->emu->memory, address, size);
    }

    void sogen_linux_emulator::write_memory(const uint64_t address, const nb::bytes& buffer) const
    {
        write_memory_bytes(this->emu->memory, address, buffer);
    }

    uint64_t sogen_linux_emulator::read_register(const x86_register reg) const
    {
        return this->emu->emu().reg<uint64_t>(reg);
    }

    void sogen_linux_emulator::write_register(const x86_register reg, const uint64_t value) const
    {
        this->emu->emu().reg<uint64_t>(reg, value);
    }

    uint64_t sogen_linux_emulator::executed_instructions() const
    {
        return this->emu->get_executed_instructions();
    }

    std::string sogen_linux_emulator::backend_name() const
    {
        return this->emu->emu().get_name();
    }

    std::filesystem::path sogen_linux_emulator::emulation_root() const
    {
        return this->emu->emulation_root;
    }
}

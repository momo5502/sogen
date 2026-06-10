#include "sogen_linux_internal.hpp"

namespace sogen::py
{
    linux_syscall_hook_continuation coerce_linux_syscall_continuation(nb::handle result)
    {
        if (result.is_none())
        {
            return linux_syscall_hook_continuation::run_handler;
        }

        if (nb::isinstance<nb::bool_>(result))
        {
            return nb::cast<bool>(result) ? linux_syscall_hook_continuation::skip_handler
                                          : linux_syscall_hook_continuation::run_handler;
        }

        if (nb::isinstance<linux_syscall_continuation>(result))
        {
            switch (nb::cast<linux_syscall_continuation>(result))
            {
            case linux_syscall_continuation::run_handler:
                return linux_syscall_hook_continuation::run_handler;
            case linux_syscall_continuation::skip_handler:
                return linux_syscall_hook_continuation::skip_handler;
            case linux_syscall_continuation::stop:
                return linux_syscall_hook_continuation::stop;
            }
        }

        return nb::cast<linux_syscall_hook_continuation>(result);
    }

    namespace
    {
        template <typename T>
        T get_kwarg(const nb::kwargs& kwargs, const char* name, T default_value)
        {
            if (!kwargs.contains(name))
            {
                return default_value;
            }

            return nb::cast<T>(kwargs[name]);
        }

        std::vector<std::string> parse_string_arguments(const nb::object& object)
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

        std::vector<std::string> parse_environment(const nb::object& object)
        {
            std::vector<std::string> result{};
            if (object.is_none())
            {
                result.emplace_back("PATH=/usr/bin:/bin");
                result.emplace_back("HOME=/root");
                result.emplace_back("TERM=xterm");
                return result;
            }

            const auto dict = nb::cast<nb::dict>(object);
            for (const auto& item : dict)
            {
                const auto key = nb::cast<std::string>(item.first);
                const auto value = nb::cast<std::string>(item.second);
                result.emplace_back(key + "=" + value);
            }

            return result;
        }

        std::vector<std::string> build_argv(const std::filesystem::path& executable, const std::vector<std::string>& args)
        {
            if (!args.empty())
            {
                return args;
            }

            return {executable.filename().string()};
        }

        backend_type get_backend_type(const nb::kwargs& kwargs)
        {
            return get_kwarg<backend_type>(kwargs, "backend", backend_type::unicorn);
        }

        std::unordered_map<std::string, std::filesystem::path> parse_path_mappings(const nb::object& object)
        {
            std::unordered_map<std::string, std::filesystem::path> result{};
            if (object.is_none())
            {
                return result;
            }

            const auto dict = nb::cast<nb::dict>(object);
            for (const auto& item : dict)
            {
                result.emplace(nb::cast<std::string>(item.first), nb::cast<std::filesystem::path>(item.second));
            }

            return result;
        }

        std::unordered_map<uint16_t, uint16_t> parse_port_mappings(const nb::object& object)
        {
            std::unordered_map<uint16_t, uint16_t> result{};
            if (object.is_none())
            {
                return result;
            }

            const auto dict = nb::cast<nb::dict>(object);
            for (const auto& item : dict)
            {
                result.emplace(nb::cast<uint16_t>(item.first), nb::cast<uint16_t>(item.second));
            }

            return result;
        }
    }

    std::unique_ptr<linux_emulator> create_linux_application_emulator(const nb::object& application, const nb::object& args,
                                                                      const nb::kwargs& kwargs)
    {
        const auto executable = nb::cast<std::filesystem::path>(application);
        auto argv = build_argv(executable, parse_string_arguments(args));
        auto envp = parse_environment(kwargs.contains("env") ? kwargs["env"] : nb::none());
        const auto emulation_root = get_kwarg<std::filesystem::path>(kwargs, "emulation_root", {});

        auto linux_emu = std::make_unique<linux_emulator>(create_x86_64_emulator(get_backend_type(kwargs)), emulation_root, executable,
                                                          std::move(argv), std::move(envp));

        const auto disable_logging = get_kwarg<bool>(kwargs, "disable_logging", false);
        const auto verbose = get_kwarg<bool>(kwargs, "verbose", true);
        if (disable_logging || !verbose)
        {
            linux_emu->log.disable_output(true);
        }

        if (kwargs.contains("library_paths"))
        {
            const auto seq = nb::cast<nb::sequence>(kwargs["library_paths"]);
            for (const auto& item : seq)
            {
                linux_emu->mod_manager.add_library_path(nb::cast<std::filesystem::path>(item));
            }
        }

        if (kwargs.contains("passthrough_prefixes"))
        {
            const auto seq = nb::cast<nb::sequence>(kwargs["passthrough_prefixes"]);
            for (const auto& item : seq)
            {
                linux_emu->file_sys.add_passthrough_prefix(nb::cast<std::filesystem::path>(item));
            }
        }

        if (kwargs.contains("path_mappings"))
        {
            for (const auto& [guest, host] : parse_path_mappings(kwargs["path_mappings"]))
            {
                linux_emu->file_sys.map(guest, host);
            }
        }

        if (kwargs.contains("port_mappings"))
        {
            for (const auto& [emulator_port, host_port] : parse_port_mappings(kwargs["port_mappings"]))
            {
                linux_emu->map_port(emulator_port, host_port);
            }
        }

        return linux_emu;
    }
}

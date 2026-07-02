#include "sogen_internal.hpp"
#include <windows_emulator.hpp>

namespace sogen::py
{
    std::string stop_reason_to_string(const stop_reason reason)
    {
        switch (reason)
        {
        case stop_reason::none:
            return "none";
        case stop_reason::unknown_syscall:
            return "unknown_syscall";
        case stop_reason::unimplemented_syscall:
            return "unimplemented_syscall";
        case stop_reason::syscall_exception:
            return "syscall_exception";
        case stop_reason::instruction_limit:
            return "instruction_limit";
        case stop_reason::normal_exit:
            return "normal_exit";
        case stop_reason::signal_termination:
            return "signal_termination";
        case stop_reason::unhandled_memory_violation:
            return "unhandled_memory_violation";
        case stop_reason::explicit_stop:
            return "explicit_stop";
        case stop_reason::backend_error:
            return "backend_error";
        case stop_reason::breakpoint:
            return "breakpoint";
        case stop_reason::watchpoint:
            return "watchpoint";
        }

        return "unknown";
    }

    api_call_continuation coerce_api_continuation(nb::handle result)
    {
        if (result.is_none())
        {
            return api_call_continuation::run_original;
        }

        if (nb::isinstance<nb::bool_>(result))
        {
            return nb::cast<bool>(result) ? api_call_continuation::intercept : api_call_continuation::run_original;
        }

        return nb::cast<api_call_continuation>(result);
    }

    instruction_hook_continuation coerce_instruction_continuation(nb::handle result)
    {
        if (result.is_none())
        {
            return instruction_hook_continuation::run_instruction;
        }

        if (nb::isinstance<nb::bool_>(result))
        {
            return nb::cast<bool>(result) ? instruction_hook_continuation::skip_instruction
                                          : instruction_hook_continuation::run_instruction;
        }

        return nb::cast<instruction_hook_continuation>(result);
    }

    memory_violation_continuation coerce_memory_violation_continuation(nb::handle result)
    {
        if (result.is_none())
        {
            return memory_violation_continuation::resume;
        }

        if (nb::isinstance<nb::bool_>(result))
        {
            return nb::cast<bool>(result) ? memory_violation_continuation::resume : memory_violation_continuation::stop;
        }

        return nb::cast<memory_violation_continuation>(result);
    }

    nb::bytes read_memory_bytes(const memory_interface& memory, const uint64_t address, const size_t size)
    {
        const auto data = memory.read_memory(address, size);
        return nb::bytes(reinterpret_cast<const char*>(data.data()), static_cast<nb::ssize_t>(data.size()));
    }

    void write_memory_bytes(memory_interface& memory, const uint64_t address, const nb::bytes& buffer)
    {
        memory.write_memory(address, buffer.data(), buffer.size());
    }

    nb::bytes serialize_state_bytes(const windows_emulator& emulator)
    {
        utils::buffer_serializer serializer{};
        emulator.serialize(serializer);

        const auto data = serializer.move_buffer();
        return nb::bytes(reinterpret_cast<const char*>(data.data()), static_cast<nb::ssize_t>(data.size()));
    }

    void deserialize_state_bytes(windows_emulator& emulator, const nb::bytes& buffer)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(buffer.data());
        const auto* end = begin + buffer.size();
        utils::buffer_deserializer deserializer{std::span(begin, end)};
        emulator.deserialize(deserializer);
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

        std::vector<std::u16string> parse_arguments(const nb::object& object)
        {
            std::vector<std::u16string> result{};
            if (object.is_none())
            {
                return result;
            }

            const auto seq = nb::cast<nb::sequence>(object);
            result.reserve(static_cast<size_t>(nb::len(seq)));
            for (const auto& item : seq)
            {
                result.emplace_back(u8_to_u16(nb::cast<std::string>(item)));
            }

            return result;
        }

        utils::unordered_insensitive_u16string_map<std::u16string> parse_environment(const nb::object& object)
        {
            utils::unordered_insensitive_u16string_map<std::u16string> result{};
            if (object.is_none())
            {
                return result;
            }

            const auto dict = nb::cast<nb::dict>(object);
            for (const auto& item : dict)
            {
                result.emplace(u8_to_u16(nb::cast<std::string>(item.first)), u8_to_u16(nb::cast<std::string>(item.second)));
            }

            return result;
        }

        std::unordered_map<windows_path, std::filesystem::path> parse_path_mappings(const nb::object& object)
        {
            std::unordered_map<windows_path, std::filesystem::path> result{};
            if (object.is_none())
            {
                return result;
            }

            const auto dict = nb::cast<nb::dict>(object);
            for (const auto& item : dict)
            {
                result.emplace(nb::cast<std::filesystem::path>(item.first), nb::cast<std::filesystem::path>(item.second));
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

        emulator_settings make_emulator_settings(const nb::kwargs& kwargs)
        {
            emulator_settings settings{};
            settings.disable_logging = get_kwarg<bool>(kwargs, "disable_logging", false);
            settings.use_relative_time = get_kwarg<bool>(kwargs, "use_relative_time", false);
            settings.emulation_root = get_kwarg<std::filesystem::path>(kwargs, "emulation_root", {});
            settings.registry_directory =
                get_kwarg<std::filesystem::path>(kwargs, "registry_directory", std::filesystem::path{"./registry"});
            settings.path_mappings = parse_path_mappings(kwargs.contains("path_mappings") ? kwargs["path_mappings"] : nb::none());
            settings.port_mappings = parse_port_mappings(kwargs.contains("port_mappings") ? kwargs["port_mappings"] : nb::none());
            settings.fake_env.number_of_processors = get_kwarg<uint32_t>(kwargs, "number_of_processors", 4);
            settings.fake_env.nt_product_type = static_cast<uint8_t>(get_kwarg<uint32_t>(kwargs, "nt_product_type", 1));
            return settings;
        }

        application_settings make_application_settings(const nb::object& application, const nb::object& args, const nb::kwargs& kwargs)
        {
            application_settings settings{};
            settings.application = nb::cast<std::filesystem::path>(application);
            settings.working_directory = get_kwarg<std::filesystem::path>(kwargs, "working_directory", {});
            settings.arguments = parse_arguments(args);
            settings.environment = parse_environment(kwargs.contains("environment") ? kwargs["environment"] : nb::none());
            return settings;
        }

        backend_type get_backend_type(const nb::kwargs& kwargs)
        {
            return get_kwarg<backend_type>(kwargs, "backend", backend_type::unicorn);
        }
    }

    std::unique_ptr<windows_emulator> create_empty_emulator(const nb::kwargs& kwargs)
    {
        return std::make_unique<windows_emulator>(create_x86_64_emulator(get_backend_type(kwargs)), make_emulator_settings(kwargs));
    }

    std::unique_ptr<windows_emulator> create_application_emulator(const nb::object& application, const nb::object& args,
                                                                  const nb::kwargs& kwargs)
    {
        auto app_settings = make_application_settings(application, args, kwargs);
        return std::make_unique<windows_emulator>(create_x86_64_emulator(get_backend_type(kwargs)), std::move(app_settings),
                                                  make_emulator_settings(kwargs));
    }
}

#include "std_include.hpp"

#include "macos_syscall_trace.hpp"

#include "bsd_syscall_table.hpp"
#include "mach_trap_table.hpp"
#include "macos_flag_decoders.hpp"
#include "macos_guest_reader.hpp"

#include <array>

namespace sogen
{
    namespace
    {
        struct trace_iovec
        {
            uint64_t base{};
            uint64_t length{};
        };

        static_assert(sizeof(trace_iovec) == 16);

        enum class argument_kind : uint8_t
        {
            pointer,
            path,
            buffer,
            iovec,
            file_descriptor,
            signed_integer,
            unsigned_integer,
            open_flags,
            mmap_protection,
            mmap_flags,
            madvise_advice,
            fcntl_command,
            seek_whence,
            file_mode,
        };

        struct argument_override
        {
            std::string_view syscall{};
            size_t index{};
            argument_kind kind{};
        };

        constexpr std::array overrides{
            argument_override{.syscall = "open", .index = 1, .kind = argument_kind::open_flags},
            argument_override{.syscall = "open", .index = 2, .kind = argument_kind::file_mode},
            argument_override{.syscall = "open_nocancel", .index = 1, .kind = argument_kind::open_flags},
            argument_override{.syscall = "open_nocancel", .index = 2, .kind = argument_kind::file_mode},
            argument_override{.syscall = "openat", .index = 2, .kind = argument_kind::open_flags},
            argument_override{.syscall = "openat", .index = 3, .kind = argument_kind::file_mode},
            argument_override{.syscall = "openat_nocancel", .index = 2, .kind = argument_kind::open_flags},
            argument_override{.syscall = "mmap", .index = 2, .kind = argument_kind::mmap_protection},
            argument_override{.syscall = "mmap", .index = 3, .kind = argument_kind::mmap_flags},
            argument_override{.syscall = "mmap", .index = 4, .kind = argument_kind::file_descriptor},
            argument_override{.syscall = "mprotect", .index = 2, .kind = argument_kind::mmap_protection},
            argument_override{.syscall = "madvise", .index = 2, .kind = argument_kind::madvise_advice},
            argument_override{.syscall = "fcntl", .index = 1, .kind = argument_kind::fcntl_command},
            argument_override{.syscall = "fcntl_nocancel", .index = 1, .kind = argument_kind::fcntl_command},
            argument_override{.syscall = "lseek", .index = 2, .kind = argument_kind::seek_whence},
            argument_override{.syscall = "chmod", .index = 1, .kind = argument_kind::file_mode},
            argument_override{.syscall = "fchmod", .index = 1, .kind = argument_kind::file_mode},
            argument_override{.syscall = "mkdir", .index = 1, .kind = argument_kind::file_mode},
        };

        struct buffer_pairing
        {
            std::string_view syscall{};
            size_t buffer_index{};
            size_t length_index{};
        };

        constexpr std::array buffer_pairings{
            buffer_pairing{.syscall = "read", .buffer_index = 1, .length_index = 2},
            buffer_pairing{.syscall = "read_nocancel", .buffer_index = 1, .length_index = 2},
            buffer_pairing{.syscall = "write", .buffer_index = 1, .length_index = 2},
            buffer_pairing{.syscall = "write_nocancel", .buffer_index = 1, .length_index = 2},
            buffer_pairing{.syscall = "pread", .buffer_index = 1, .length_index = 2},
            buffer_pairing{.syscall = "pwrite", .buffer_index = 1, .length_index = 2},
            buffer_pairing{.syscall = "getentropy", .buffer_index = 0, .length_index = 1},
        };

        struct iovec_pairing
        {
            std::string_view syscall{};
            size_t vector_index{};
            size_t count_index{};
        };

        constexpr std::array iovec_pairings{
            iovec_pairing{.syscall = "readv", .vector_index = 1, .count_index = 2},
            iovec_pairing{.syscall = "writev", .vector_index = 1, .count_index = 2},
        };

        constexpr std::array path_argument_names{
            std::string_view{"path"},  std::string_view{"path1"}, std::string_view{"path2"},    std::string_view{"link"},
            std::string_view{"fname"}, std::string_view{"from"},  std::string_view{"to"},       std::string_view{"old"},
            std::string_view{"new"},   std::string_view{"name"},  std::string_view{"attrname"},
        };

        bool is_pointer_type(const std::string_view type)
        {
            return type.ends_with("*") || type == "user_addr_t" || type == "caddr_t";
        }

        bool is_signed_type(const std::string_view type)
        {
            return type == "int" || type == "long" || type == "off_t" || type == "ssize_t" || type == "user_ssize_t" || type == "int32_t" ||
                   type == "int64_t" || type == "user_long_t" || type == "pid_t" || type == "id_t";
        }

        bool is_path_name(const std::string_view name)
        {
            for (const auto& candidate : path_argument_names)
            {
                if (name == candidate)
                {
                    return true;
                }
            }

            return false;
        }

        argument_kind classify(const std::string_view syscall, const size_t index, const bsd_syscall_argument& argument)
        {
            for (const auto& entry : overrides)
            {
                if (entry.syscall == syscall && entry.index == index)
                {
                    return entry.kind;
                }
            }

            for (const auto& entry : buffer_pairings)
            {
                if (entry.syscall == syscall && entry.buffer_index == index)
                {
                    return argument_kind::buffer;
                }
            }

            for (const auto& entry : iovec_pairings)
            {
                if (entry.syscall == syscall && entry.vector_index == index)
                {
                    return argument_kind::iovec;
                }
            }

            if (is_pointer_type(argument.type))
            {
                return is_path_name(argument.name) ? argument_kind::path : argument_kind::pointer;
            }

            if (argument.name == "fd" || argument.name == "fildes")
            {
                return argument_kind::file_descriptor;
            }

            if (argument.name == "mode" && argument.type == "int")
            {
                return argument_kind::file_mode;
            }

            return is_signed_type(argument.type) ? argument_kind::signed_integer : argument_kind::unsigned_integer;
        }

        std::string describe_path(const memory_interface& memory, const uint64_t address, const macos_trace_options& options)
        {
            if (address == 0)
            {
                return "NULL";
            }

            const auto text = read_bounded_guest_string(memory, address, options.string_limit);
            if (!text)
            {
                return "<unreadable>";
            }

            return quote_trace_text(*text);
        }

        std::string describe_buffer(const memory_interface& memory, const uint64_t address, const uint64_t length,
                                    const macos_trace_options& options)
        {
            if (address == 0)
            {
                return "NULL";
            }

            if (length == 0)
            {
                return "\"\"";
            }

            const auto wanted = length > options.string_limit ? options.string_limit : static_cast<size_t>(length);
            const auto bytes = read_bounded_guest_bytes(memory, address, wanted);
            if (!bytes)
            {
                return "<unreadable>";
            }

            if (is_printable_trace_run(*bytes))
            {
                macos_guest_text text{};
                text.text.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                text.truncated = bytes->size() < length;
                return quote_trace_text(text);
            }

            const auto preview = bytes->size() > options.buffer_preview_limit ? options.buffer_preview_limit : bytes->size();
            return format_byte_preview(*bytes, 0, preview, static_cast<size_t>(length));
        }

        std::string describe_iovec(const memory_interface& memory, const uint64_t address, const uint64_t count,
                                   const macos_trace_options& options)
        {
            if (address == 0)
            {
                return "NULL";
            }

            constexpr uint64_t max_entries = 8;
            const auto entries = count > max_entries ? max_entries : count;

            std::string result = "[" + std::to_string(count) + " buffers]";

            const auto budget = options.buffer_preview_limit;
            size_t consumed = 0;

            for (uint64_t i = 0; i < entries; ++i)
            {
                trace_iovec entry{};
                if (!memory.try_read_memory(address + i * sizeof(entry), &entry, sizeof(entry)))
                {
                    result += " <unreadable>";
                    break;
                }

                const auto entry_length = static_cast<size_t>(entry.length);
                if (consumed > budget || entry_length > budget - consumed)
                {
                    result += " ...";
                    break;
                }

                consumed += entry_length;

                result.push_back(' ');
                result += describe_buffer(memory, entry.base, entry.length, options);
            }

            if (count > entries)
            {
                result += " ...";
            }

            return result;
        }

        std::string describe_argument(const memory_interface& memory, const std::string_view syscall, const size_t index,
                                      const bsd_syscall_argument& argument, const std::span<const uint64_t> values,
                                      const macos_trace_options& options)
        {
            const auto value = values[index];

            switch (classify(syscall, index, argument))
            {
            case argument_kind::path:
                return describe_path(memory, value, options);

            case argument_kind::buffer:
                for (const auto& entry : buffer_pairings)
                {
                    if (entry.syscall == syscall && entry.buffer_index == index && entry.length_index < values.size())
                    {
                        return describe_buffer(memory, value, values[entry.length_index], options);
                    }
                }
                return format_hex(value);

            case argument_kind::iovec:
                for (const auto& entry : iovec_pairings)
                {
                    if (entry.syscall == syscall && entry.vector_index == index && entry.count_index < values.size())
                    {
                        return describe_iovec(memory, value, values[entry.count_index], options);
                    }
                }
                return format_hex(value);

            case argument_kind::file_descriptor:
            case argument_kind::signed_integer:
                return std::to_string(static_cast<int64_t>(value));

            case argument_kind::unsigned_integer:
                return std::to_string(value);

            case argument_kind::open_flags:
                return format_open_flags(value);

            case argument_kind::mmap_protection:
                return format_mmap_protection(value);

            case argument_kind::mmap_flags:
                return format_mmap_flags(value);

            case argument_kind::madvise_advice:
                return format_madvise_advice(value);

            case argument_kind::fcntl_command:
                return format_fcntl_command(value);

            case argument_kind::seek_whence:
                return format_seek_whence(value);

            case argument_kind::file_mode:
                return format_file_mode(value);

            case argument_kind::pointer:
            default:
                return format_hex(value);
            }
        }
    }

    std::vector<macos_trace_detail> describe_bsd_syscall(const memory_interface& memory, const uint32_t number,
                                                         const std::span<const uint64_t> arguments, const macos_trace_options& options)
    {
        std::vector<macos_trace_detail> details{};

        const auto* prototype = find_bsd_syscall_prototype(number);
        if (prototype == nullptr)
        {
            // Positional rows only for what is non-zero: six "0x0" lines per unprototyped call would
            // drown the calls that carry information.
            for (size_t i = 0; i < arguments.size(); ++i)
            {
                if (arguments[i] == 0)
                {
                    continue;
                }

                details.push_back({"arg" + std::to_string(i), format_hex(arguments[i])});
            }

            return details;
        }

        const auto count = prototype->arguments.size() > arguments.size() ? arguments.size() : prototype->arguments.size();
        details.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            const auto& argument = prototype->arguments[i];
            details.push_back({std::string(argument.name), describe_argument(memory, prototype->name, i, argument, arguments, options)});
        }

        return details;
    }

    std::vector<macos_trace_detail> describe_mach_trap(const uint32_t index, const std::span<const uint64_t> arguments)
    {
        std::vector<macos_trace_detail> details{};

        const auto* prototype = find_mach_trap_prototype(index);
        if (prototype == nullptr)
        {
            return details;
        }

        const auto declared = static_cast<size_t>(prototype->argument_count);
        const auto count = declared > arguments.size() ? arguments.size() : declared;
        details.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            details.push_back({"arg" + std::to_string(i), format_hex(arguments[i])});
        }

        return details;
    }
}

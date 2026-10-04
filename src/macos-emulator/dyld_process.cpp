#include "std_include.hpp"
#include "dyld_process.hpp"

#include <cinttypes>
#include <cstdio>

namespace sogen
{
    namespace
    {
        std::string to_hex(const uint64_t value)
        {
            std::array<char, 32> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "0x%" PRIx64, value);
            return buffer.data();
        }

        std::string to_padded_hex(const uint64_t value)
        {
            std::array<char, 32> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "0x%016" PRIx64, value);
            return buffer.data();
        }
    }

    std::string format_hex_key(const std::string_view key, const uint64_t value)
    {
        return std::string{key} + to_hex(value);
    }

    std::string format_padded_hex_key(const std::string_view key, const uint64_t value)
    {
        return std::string{key} + to_padded_hex(value);
    }

    // The order and the value formats are the kernel's, measured on build 25G76 with a four-argument
    // main() probe; the slot widths between consecutive entries are what recovered the widths of the
    // values dyld scrubs before main() runs.
    std::vector<std::string> macos_apple_strings::render() const
    {
        std::vector<std::string> entries{};
        entries.reserve(12);

        entries.push_back("executable_path=" + this->executable_path);
        entries.push_back(format_hex_key("pfz=", this->pfz));
        entries.push_back(format_padded_hex_key("stack_guard=", this->stack_guard));
        entries.push_back(format_padded_hex_key("malloc_entropy=", this->malloc_entropy[0]) + "," +
                          format_padded_hex_key("", this->malloc_entropy[1]));
        entries.push_back(format_padded_hex_key("ptr_munge=", this->ptr_munge));
        entries.push_back(format_hex_key("main_stack=", this->main_stack_top) + "," + to_hex(this->main_stack_size) + "," +
                          to_hex(this->main_stack_alloc_base) + "," + to_hex(this->main_stack_alloc_size));
        entries.push_back(format_hex_key("executable_file=", this->executable_file.packed_fsid()) + "," +
                          to_hex(this->executable_file.object_id));
        entries.push_back(format_hex_key("dyld_file=", this->dyld_file.packed_fsid()) + "," + to_hex(this->dyld_file.object_id));
        entries.push_back("executable_cdhash=" + this->executable_cdhash);
        entries.push_back("executable_boothash=" + this->executable_boothash);
        entries.push_back(format_hex_key("th_port=", this->th_port));
        entries.push_back(format_hex_key("security_config=", this->security_config));

        return entries;
    }

    macos_kernel_args_layout build_dyld_kernel_args(macos_memory_manager& memory, const uint64_t stack_top, const uint64_t stack_base,
                                                    const uint64_t mach_header, const std::vector<std::string>& argv,
                                                    const std::vector<std::string>& envp, const std::vector<std::string>& apple)
    {
        macos_kernel_args_layout layout{};

        if (stack_top <= stack_base)
        {
            return layout;
        }

        const auto available = stack_top - stack_base;

        size_t string_bytes = 0;
        for (const auto* block : {&argv, &envp, &apple})
        {
            for (const auto& entry : *block)
            {
                string_bytes += entry.size() + 1;
            }
        }

        const auto pointer_slots = argv.size() + envp.size() + apple.size() + 3;
        const auto pointer_bytes = (pointer_slots + 2) * sizeof(uint64_t);

        // Redundant with the try_write_memory calls and the stack_pointer < stack_base check below,
        // which between them refuse everything this refuses -- no mutation of this test survives them.
        // Kept because it states the bound before stack_top - string_bytes can wrap.
        if (string_bytes > available || pointer_bytes > available - string_bytes)
        {
            return layout;
        }

        auto string_base = stack_top - string_bytes;
        string_base -= string_base % sizeof(uint64_t);

        auto cursor = string_base;

        std::vector<uint64_t> argv_pointers{};
        std::vector<uint64_t> envp_pointers{};
        std::vector<uint64_t> apple_pointers{};

        auto write_block = [&](const std::vector<std::string>& block, std::vector<uint64_t>& pointers) {
            for (const auto& entry : block)
            {
                if (!memory.try_write_memory(cursor, entry.c_str(), entry.size() + 1))
                {
                    return false;
                }

                pointers.push_back(cursor);
                cursor += entry.size() + 1;
            }

            return true;
        };

        if (!write_block(argv, argv_pointers) || !write_block(envp, envp_pointers) || !write_block(apple, apple_pointers))
        {
            return layout;
        }

        // Derived from the address the strings actually start at rather than from the unrounded
        // stack_top - string_bytes. Both happen to be safe, but only the first is safe without an
        // argument about how the two roundings interact, and the pointers are written second.
        auto stack_pointer = string_base - pointer_bytes;
        stack_pointer -= stack_pointer % 16;

        if (stack_pointer < stack_base)
        {
            return layout;
        }

        std::vector<uint64_t> words{};
        words.reserve(pointer_slots + 2);

        words.push_back(mach_header);
        words.push_back(argv.size());
        words.insert(words.end(), argv_pointers.begin(), argv_pointers.end());
        words.push_back(0);
        words.insert(words.end(), envp_pointers.begin(), envp_pointers.end());
        words.push_back(0);
        words.insert(words.end(), apple_pointers.begin(), apple_pointers.end());
        words.push_back(0);

        if (!memory.try_write_memory(stack_pointer, words.data(), words.size() * sizeof(uint64_t)))
        {
            return layout;
        }

        layout.stack_pointer = stack_pointer;
        layout.mach_header = mach_header;
        layout.argc = argv.size();
        layout.argv = stack_pointer + 16;
        layout.envp = layout.argv + (argv.size() + 1) * sizeof(uint64_t);
        layout.apple = layout.envp + (envp.size() + 1) * sizeof(uint64_t);
        layout.valid = true;

        return layout;
    }
}

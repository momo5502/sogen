#include "../std_include.hpp"
#include "linux_module_manager.hpp"
#include "../linux_file_system.hpp"

#include <platform/elf.hpp>
#include <utils/io.hpp>
#include <serialization_helper.hpp>
#include <address_utils.hpp>
#include <limits>

namespace sogen
{

    using namespace elf; // NOLINT(google-build-using-namespace)

    namespace
    {
        constexpr size_t runtime_module_metadata_read_limit = 16 * 1024 * 1024;

        bool checked_add_u64(const uint64_t lhs, const uint64_t rhs, uint64_t& result)
        {
            if (lhs > std::numeric_limits<uint64_t>::max() - rhs)
            {
                return false;
            }

            result = lhs + rhs;
            return true;
        }

        bool range_within_file(const size_t file_size, const uint64_t offset, const uint64_t size)
        {
            const auto size_u64 = static_cast<uint64_t>(file_size);
            return offset <= size_u64 && size <= size_u64 - offset;
        }

        bool checked_page_align_up(const uint64_t value, uint64_t& aligned)
        {
            if (value > std::numeric_limits<uint64_t>::max() - (LINUX_PAGE_SIZE - 1))
            {
                return false;
            }

            aligned = page_align_up(value);
            return true;
        }

        const Elf64_Phdr* get_checked_program_headers(const std::span<const std::byte> data)
        {
            const auto* ehdr = get_header(data);
            if (!ehdr || !ehdr->e_phoff || !ehdr->e_phnum || ehdr->e_phentsize != sizeof(Elf64_Phdr))
            {
                return nullptr;
            }

            const auto phdr_size = static_cast<uint64_t>(ehdr->e_phnum) * sizeof(Elf64_Phdr);
            if (!range_within_file(data.size(), ehdr->e_phoff, phdr_size))
            {
                return nullptr;
            }

            return reinterpret_cast<const Elf64_Phdr*>(data.data() + static_cast<size_t>(ehdr->e_phoff));
        }

        std::vector<std::byte> read_runtime_module_metadata_prefix(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return {};
            }

            std::array<std::byte, sizeof(Elf64_Ehdr)> header_data{};
            stream.read(reinterpret_cast<char*>(header_data.data()), static_cast<std::streamsize>(header_data.size()));
            if (stream.gcount() != static_cast<std::streamsize>(header_data.size()))
            {
                return {};
            }

            const auto header_span = std::span<const std::byte>(header_data.data(), header_data.size());
            if (!is_valid_elf(header_span) || !is_elf64(header_span) || !is_little_endian(header_span))
            {
                return {};
            }

            const auto* ehdr = get_header(header_span);
            if (!ehdr || ehdr->e_phentsize != sizeof(Elf64_Phdr) || ehdr->e_phnum == 0)
            {
                return {};
            }

            const auto phdr_bytes = static_cast<uint64_t>(ehdr->e_phnum) * ehdr->e_phentsize;
            uint64_t phdr_end = 0;
            if (!checked_add_u64(ehdr->e_phoff, phdr_bytes, phdr_end))
            {
                return {};
            }

            if (phdr_end > runtime_module_metadata_read_limit)
            {
                return {};
            }

            auto bytes_to_read = runtime_module_metadata_read_limit;
            std::error_code ec{};
            const auto file_size = std::filesystem::file_size(path, ec);
            if (!ec)
            {
                bytes_to_read = static_cast<size_t>(std::min<std::uintmax_t>(file_size, runtime_module_metadata_read_limit));
            }

            if (bytes_to_read < phdr_end)
            {
                return {};
            }

            std::vector<std::byte> data(bytes_to_read);
            stream.clear();
            stream.seekg(0, std::ios::beg);
            stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));

            const auto read_count = stream.gcount();
            if (read_count < static_cast<std::streamsize>(phdr_end))
            {
                return {};
            }

            data.resize(static_cast<size_t>(read_count));
            return data;
        }
    }

    namespace utils
    {
        static void serialize(buffer_serializer& buffer, const linux_exported_symbol& symbol)
        {
            buffer.write(symbol.name);
            buffer.write(symbol.rva);
            buffer.write(symbol.address);
        }

        static void deserialize(buffer_deserializer& buffer, linux_exported_symbol& symbol)
        {
            buffer.read(symbol.name);
            buffer.read(symbol.rva);
            buffer.read(symbol.address);
        }

        static void serialize(buffer_serializer& buffer, const linux_mapped_section& section)
        {
            buffer.write(section.name);
            buffer.write(section.start);
            buffer.write<uint64_t>(section.length);
            buffer.write(section.permissions);
        }

        static void deserialize(buffer_deserializer& buffer, linux_mapped_section& section)
        {
            buffer.read(section.name);
            buffer.read(section.start);
            section.length = static_cast<size_t>(buffer.read<uint64_t>());
            buffer.read(section.permissions);
        }

        static void serialize(buffer_serializer& buffer, const linux_mapped_module& module)
        {
            buffer.write(module.name);
            buffer.write(module.path);
            buffer.write(module.image_base);
            buffer.write(module.size_of_image);
            buffer.write(module.entry_point);
            buffer.write(module.machine);
            buffer.write(module.elf_type);
            buffer.write(module.phdr_vaddr);
            buffer.write(module.phdr_count);
            buffer.write(module.phdr_entry_size);
            buffer.write(module.tls_image_addr);
            buffer.write(module.tls_image_size);
            buffer.write(module.tls_mem_size);
            buffer.write(module.tls_alignment);
            buffer.write_vector(module.exports);
            buffer.write_vector(module.needed_libraries);
            buffer.write_vector(module.sections);
            buffer.write(module.rpath);
            buffer.write(module.runpath);
        }

        static void deserialize(buffer_deserializer& buffer, linux_mapped_module& module)
        {
            buffer.read(module.name);
            buffer.read(module.path);
            buffer.read(module.image_base);
            buffer.read(module.size_of_image);
            buffer.read(module.entry_point);
            buffer.read(module.machine);
            buffer.read(module.elf_type);
            buffer.read(module.phdr_vaddr);
            buffer.read(module.phdr_count);
            buffer.read(module.phdr_entry_size);
            buffer.read(module.tls_image_addr);
            buffer.read(module.tls_image_size);
            buffer.read(module.tls_mem_size);
            buffer.read(module.tls_alignment);
            buffer.read_vector(module.exports);
            buffer.read_vector(module.needed_libraries);
            buffer.read_vector(module.sections);
            buffer.read(module.rpath);
            buffer.read(module.runpath);
        }

        static void serialize(buffer_serializer& buffer, const elf_irelative_entry& entry)
        {
            buffer.write(entry.got_addr);
            buffer.write(entry.resolver_addr);
        }

        static void deserialize(buffer_deserializer& buffer, elf_irelative_entry& entry)
        {
            buffer.read(entry.got_addr);
            buffer.read(entry.resolver_addr);
        }
    }

    linux_mapped_module* linux_module_manager::insert_module(linux_mapped_module mod)
    {
        const auto base = mod.image_base;
        auto [it, inserted] = this->modules_.emplace(base, std::move(mod));
        if (!inserted)
        {
            throw std::runtime_error("Module already mapped at base 0x" + std::to_string(base));
        }

        this->on_module_load(it->second);

        return &it->second;
    }

    void linux_module_manager::map_main_modules(const std::filesystem::path& executable_path)
    {
        auto file_data = utils::io::read_file(executable_path);
        if (file_data.empty())
        {
            throw std::runtime_error("Failed to read executable: " + executable_path.string());
        }

        const auto data_span = std::span<const std::byte>(file_data.data(), file_data.size());

        // Map the ELF into emulated memory
        auto mod = map_elf_from_data(*this->memory_, data_span, executable_path);

        // Check for PT_INTERP — dynamic linker path
        const auto* ehdr = get_header(data_span);
        const auto* phdrs = get_checked_program_headers(data_span);
        bool has_interp = false;

        if (ehdr && phdrs)
        {
            for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
            {
                const auto& ph = phdrs[i];
                if (ph.p_type != PT_INTERP)
                {
                    continue;
                }

                has_interp = true;

                if (!range_within_file(file_data.size(), ph.p_offset, ph.p_filesz))
                {
                    throw std::runtime_error("Invalid PT_INTERP file range in ELF: " + executable_path.string());
                }

                const auto* interp_str = reinterpret_cast<const char*>(file_data.data() + static_cast<size_t>(ph.p_offset));
                auto interp_len = ph.p_filesz;

                // Strip trailing null
                while (interp_len > 0 && interp_str[interp_len - 1] == '\0')
                {
                    --interp_len;
                }

                this->interpreter_path = std::string(interp_str, static_cast<size_t>(interp_len));

                break;
            }
        }

        // For statically-linked binaries (no PT_INTERP), apply relocations now.
        // For dynamically-linked binaries, leave relocations to the runtime linker.
        if (!has_interp)
        {
            const int64_t base_delta = static_cast<int64_t>(mod.entry_point) - static_cast<int64_t>(ehdr->e_entry);
            apply_elf_relocations(*this->memory_, mod, data_span, base_delta);

            // Collect IRELATIVE relocations (IFUNC resolvers, common in glibc static binaries)
            auto irelatives = collect_irelative_relocations(data_span, base_delta);
            if (!irelatives.empty())
            {
                this->irelative_entries.insert(this->irelative_entries.end(), irelatives.begin(), irelatives.end());
            }
        }

        // Store the module
        this->executable = this->insert_module(std::move(mod));
    }

    linux_mapped_module* linux_module_manager::map_module(const std::filesystem::path& path, const uint64_t forced_base,
                                                          const bool apply_relocations)
    {
        auto file_data = utils::io::read_file(path);
        if (file_data.empty())
        {
            throw std::runtime_error("Failed to read module: " + path.string());
        }

        const auto data_span = std::span<const std::byte>(file_data.data(), file_data.size());

        auto mod = map_elf_from_data(*this->memory_, data_span, path, forced_base);

        if (apply_relocations)
        {
            const auto* ehdr = get_header(data_span);
            const int64_t base_delta = static_cast<int64_t>(mod.entry_point) - static_cast<int64_t>(ehdr->e_entry);
            apply_elf_relocations(*this->memory_, mod, data_span, base_delta);
        }

        return this->insert_module(std::move(mod));
    }

    linux_mapped_module* linux_module_manager::record_runtime_module_mapping(const std::filesystem::path& path,
                                                                             const uint64_t mapped_address, const uint64_t file_offset)
    {
        if (path.empty() || mapped_address == 0)
        {
            return nullptr;
        }

        for (auto& [_, module] : this->modules_)
        {
            if (module.path == path && module.contains(mapped_address))
            {
                return &module;
            }
        }

        auto file_data = read_runtime_module_metadata_prefix(path);
        if (file_data.empty())
        {
            return nullptr;
        }

        const auto data_span = std::span<const std::byte>(file_data.data(), file_data.size());
        if (!is_valid_elf(data_span) || !is_elf64(data_span) || !is_little_endian(data_span))
        {
            return nullptr;
        }

        const auto* ehdr = get_header(data_span);
        const auto* phdrs = get_checked_program_headers(data_span);
        if (!ehdr || !phdrs)
        {
            return nullptr;
        }

        uint64_t vaddr_min = UINT64_MAX;
        for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
        {
            const auto& ph = phdrs[i];
            if (ph.p_type == PT_LOAD && ph.p_filesz > ph.p_memsz)
            {
                return nullptr;
            }

            if (ph.p_type == PT_LOAD)
            {
                vaddr_min = std::min(vaddr_min, page_align_down(ph.p_vaddr));
            }
        }

        if (vaddr_min == UINT64_MAX)
        {
            return nullptr;
        }

        std::optional<uint64_t> image_base{};
        for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
        {
            const auto& ph = phdrs[i];
            if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0)
            {
                continue;
            }

            const auto segment_file_start = page_align_down(ph.p_offset);
            uint64_t segment_file_limit = 0;
            uint64_t segment_file_end = 0;
            if (!checked_add_u64(ph.p_offset, std::max<uint64_t>(ph.p_filesz, 1), segment_file_limit) ||
                !checked_page_align_up(segment_file_limit, segment_file_end))
            {
                return nullptr;
            }
            if (file_offset < segment_file_start || file_offset >= segment_file_end)
            {
                continue;
            }

            const auto segment_vaddr_start = page_align_down(ph.p_vaddr);
            uint64_t relative_vaddr = 0;
            if (!checked_add_u64(segment_vaddr_start - vaddr_min, file_offset - segment_file_start, relative_vaddr))
            {
                return nullptr;
            }
            if (mapped_address < relative_vaddr)
            {
                return nullptr;
            }

            image_base = mapped_address - relative_vaddr;
            break;
        }

        if (!image_base)
        {
            return nullptr;
        }

        if (auto existing = this->modules_.find(*image_base); existing != this->modules_.end())
        {
            return &existing->second;
        }

        linux_mapped_module mod{};
        try
        {
            mod = read_elf_module_metadata(data_span, path, *image_base);
        }
        catch (...)
        {
            return nullptr;
        }

        return this->insert_module(std::move(mod));
    }

    linux_mapped_module* linux_module_manager::find_by_address(const uint64_t address)
    {
        for (auto& [base, mod] : this->modules_)
        {
            if (mod.contains(address))
            {
                return &mod;
            }
        }

        return nullptr;
    }

    linux_mapped_module* linux_module_manager::find_by_name(const std::string_view name)
    {
        for (auto& [base, mod] : this->modules_)
        {
            if (mod.name == name)
            {
                return &mod;
            }
        }

        return nullptr;
    }

    void linux_module_manager::add_library_path(const std::filesystem::path& path)
    {
        this->library_paths_.push_back(path);
    }

    std::vector<std::filesystem::path> linux_module_manager::split_search_paths(const std::string& paths,
                                                                                const std::filesystem::path& origin)
    {
        std::vector<std::filesystem::path> result{};

        if (paths.empty())
        {
            return result;
        }

        size_t start = 0;
        while (start < paths.size())
        {
            auto end = paths.find(':', start);
            if (end == std::string::npos)
            {
                end = paths.size();
            }

            auto entry = paths.substr(start, end - start);

            if (!entry.empty())
            {
                // Replace $ORIGIN with the directory containing the requesting module
                const std::string origin_token = "$ORIGIN";
                size_t pos = 0;
                while ((pos = entry.find(origin_token, pos)) != std::string::npos)
                {
                    entry.replace(pos, origin_token.size(), origin.string());
                    pos += origin.string().size();
                }

                // Also handle ${ORIGIN}
                const std::string origin_token_braces = "${ORIGIN}";
                pos = 0;
                while ((pos = entry.find(origin_token_braces, pos)) != std::string::npos)
                {
                    entry.replace(pos, origin_token_braces.size(), origin.string());
                    pos += origin.string().size();
                }

                result.emplace_back(entry);
            }

            start = end + 1;
        }

        return result;
    }

    std::filesystem::path linux_module_manager::resolve_library(const std::string& name, const linux_mapped_module& requesting_module,
                                                                const linux_file_system& file_sys) const
    {
        // If the name contains a slash, treat it as a path (absolute or relative)
        if (name.find('/') != std::string::npos)
        {
            auto translated = file_sys.translate(name);
            if (std::filesystem::exists(translated))
            {
                return translated;
            }
            return {};
        }

        const auto origin = requesting_module.path.parent_path();

        // Linux ld.so search order (simplified):
        // 1. DT_RPATH of the requesting object (unless DT_RUNPATH is set)
        // 2. LD_LIBRARY_PATH (our configured library_paths_)
        // 3. DT_RUNPATH of the requesting object
        // 4. Default paths: /lib, /usr/lib (under emulation root)

        // 1. DT_RPATH (only if DT_RUNPATH is empty — per ld.so spec, DT_RPATH is ignored when DT_RUNPATH is present)
        if (requesting_module.runpath.empty() && !requesting_module.rpath.empty())
        {
            for (const auto& dir : split_search_paths(requesting_module.rpath, origin))
            {
                const auto candidate = file_sys.translate((dir / name).string());
                if (std::filesystem::exists(candidate))
                {
                    return candidate;
                }
            }
        }

        // 2. Configured library paths (analogous to LD_LIBRARY_PATH)
        for (const auto& dir : this->library_paths_)
        {
            auto candidate = dir / name;
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        // 3. DT_RUNPATH
        if (!requesting_module.runpath.empty())
        {
            for (const auto& dir : split_search_paths(requesting_module.runpath, origin))
            {
                const auto candidate = file_sys.translate((dir / name).string());
                if (std::filesystem::exists(candidate))
                {
                    return candidate;
                }
            }
        }

        // 4. Default search paths under the emulation root
        static constexpr std::array<std::string_view, 7> default_paths = {
            "/lib", "/lib64", "/usr/lib", "/usr/lib64", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/usr/local/lib",
        };

        for (const auto& dir : default_paths)
        {
            const auto candidate = file_sys.translate(std::string(dir) + "/" + name);
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        return {};
    }

    void linux_module_manager::load_dependencies(const linux_file_system& file_sys)
    {
        // BFS traversal of DT_NEEDED dependencies.
        // Collect libraries to load from all currently-loaded modules.
        std::vector<std::pair<std::string, const linux_mapped_module*>> pending{};

        // Seed with needed libraries from all currently-loaded modules
        for (const auto& [base, mod] : this->modules_)
        {
            for (const auto& needed : mod.needed_libraries)
            {
                pending.emplace_back(needed, &mod);
            }
        }

        size_t idx = 0;
        while (idx < pending.size())
        {
            const auto& [lib_name, requester] = pending[idx];
            ++idx;

            // Skip if already loaded
            if (this->find_by_name(lib_name))
            {
                continue;
            }

            // Resolve the library path
            const auto lib_path = this->resolve_library(lib_name, *requester, file_sys);
            if (lib_path.empty())
            {
                // Library not found — skip silently (the dynamic linker in the emulated
                // process will handle the error, or it may be a weak dependency)
                continue;
            }

            // Load the library
            auto* loaded = this->map_module(lib_path);
            if (!loaded)
            {
                continue;
            }

            // Queue its dependencies
            for (const auto& needed : loaded->needed_libraries)
            {
                pending.emplace_back(needed, loaded);
            }
        }
    }

    void linux_module_manager::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_map(this->modules_);
        buffer.write(this->interpreter_path);
        buffer.write_vector(this->irelative_entries);
        buffer.write_vector(this->library_paths_);

        buffer.write(this->executable != nullptr);
        if (this->executable)
        {
            buffer.write(this->executable->image_base);
        }

        buffer.write(this->interpreter != nullptr);
        if (this->interpreter)
        {
            buffer.write(this->interpreter->image_base);
        }
    }

    void linux_module_manager::deserialize(utils::buffer_deserializer& buffer)
    {
        auto new_modules = buffer.read_map<std::map<uint64_t, linux_mapped_module>>();
        auto new_interpreter_path = buffer.read<std::string>();
        auto new_irelative_entries = buffer.read_vector<elf_irelative_entry>();
        auto new_library_paths = buffer.read_vector<std::filesystem::path>();

        std::optional<uint64_t> executable_base{};
        if (buffer.read<bool>())
        {
            executable_base = buffer.read<uint64_t>();
            if (!new_modules.contains(*executable_base))
            {
                throw std::runtime_error("Linux module snapshot references a missing executable module");
            }
        }

        std::optional<uint64_t> interpreter_base{};
        if (buffer.read<bool>())
        {
            interpreter_base = buffer.read<uint64_t>();
            if (!new_modules.contains(*interpreter_base))
            {
                throw std::runtime_error("Linux module snapshot references a missing interpreter module");
            }
        }

        this->modules_ = std::move(new_modules);
        this->interpreter_path = std::move(new_interpreter_path);
        this->irelative_entries = std::move(new_irelative_entries);
        this->library_paths_ = std::move(new_library_paths);

        this->executable = executable_base ? &this->modules_.at(*executable_base) : nullptr;
        this->interpreter = interpreter_base ? &this->modules_.at(*interpreter_base) : nullptr;
    }

} // namespace sogen

#include "../std_include.hpp"
#include "linux_module_manager.hpp"
#include "../linux_file_system.hpp"

#include <platform/elf.hpp>
#include <utils/io.hpp>

using namespace elf;

linux_mapped_module* linux_module_manager::insert_module(linux_mapped_module mod)
{
    const auto base = mod.image_base;
    auto [it, inserted] = this->modules_.emplace(base, std::move(mod));
    if (!inserted)
    {
        throw std::runtime_error("Module already mapped at base 0x" + std::to_string(base));
    }

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

    // Apply relocations (for PIE binaries)
    const int64_t base_delta =
        static_cast<int64_t>(mod.image_base) - static_cast<int64_t>(mod.image_base - (mod.entry_point - get_header(data_span)->e_entry));
    apply_elf_relocations(*this->memory_, mod, data_span, base_delta);

    // Collect IRELATIVE relocations (IFUNC resolvers, common in glibc static binaries)
    auto irelatives = collect_irelative_relocations(data_span, base_delta);
    if (!irelatives.empty())
    {
        this->irelative_entries.insert(this->irelative_entries.end(), irelatives.begin(), irelatives.end());
    }

    // Check for PT_INTERP — dynamic linker path
    const auto* ehdr = get_header(data_span);
    const auto* phdrs = get_program_headers(data_span);

    if (ehdr && phdrs)
    {
        for (uint16_t i = 0; i < ehdr->e_phnum; ++i)
        {
            const auto& ph = phdrs[i];
            if (ph.p_type != PT_INTERP)
            {
                continue;
            }

            if (ph.p_offset + ph.p_filesz <= file_data.size())
            {
                const auto* interp_str = reinterpret_cast<const char*>(file_data.data() + ph.p_offset);
                auto interp_len = ph.p_filesz;

                // Strip trailing null
                while (interp_len > 0 && interp_str[interp_len - 1] == '\0')
                {
                    --interp_len;
                }

                this->interpreter_path = std::string(interp_str, interp_len);
            }

            break;
        }
    }

    // Store the module
    this->executable = this->insert_module(std::move(mod));
}

linux_mapped_module* linux_module_manager::map_module(const std::filesystem::path& path, const uint64_t forced_base)
{
    auto file_data = utils::io::read_file(path);
    if (file_data.empty())
    {
        throw std::runtime_error("Failed to read module: " + path.string());
    }

    const auto data_span = std::span<const std::byte>(file_data.data(), file_data.size());

    auto mod = map_elf_from_data(*this->memory_, data_span, path, forced_base);

    // Compute base delta for relocations
    const auto* ehdr = get_header(data_span);
    const int64_t base_delta = (ehdr->e_type == ET_DYN) ? static_cast<int64_t>(mod.image_base) : 0;

    apply_elf_relocations(*this->memory_, mod, data_span, base_delta);

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

std::vector<std::filesystem::path> linux_module_manager::split_search_paths(const std::string& paths, const std::filesystem::path& origin)
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
        const auto translated = file_sys.translate(name);
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
    static const std::string_view default_paths[] = {
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

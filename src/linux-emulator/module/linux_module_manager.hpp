#pragma once

#include "../std_include.hpp"
#include "../linux_memory_manager.hpp"
#include "elf_mapping.hpp"

#include <utils/io.hpp>

class linux_file_system;

class linux_module_manager
{
  public:
    linux_module_manager(linux_memory_manager& memory)
        : memory_(&memory)
    {
    }

    linux_mapped_module* executable{};
    linux_mapped_module* interpreter{};

    std::string interpreter_path{};

    void map_main_modules(const std::filesystem::path& executable_path);
    linux_mapped_module* map_module(const std::filesystem::path& path, uint64_t forced_base = 0);

    // Load all DT_NEEDED dependencies of already-loaded modules recursively.
    // Uses DT_RPATH, DT_RUNPATH, configured library paths, and emulation root standard directories.
    void load_dependencies(const linux_file_system& file_sys);

    // Add an additional library search directory
    void add_library_path(const std::filesystem::path& path);

    // Resolve a library name (e.g., "libm.so.6") to a host filesystem path using the
    // standard search order: DT_RPATH, LD_LIBRARY_PATH (configured paths), DT_RUNPATH,
    // then default paths under the emulation root.
    std::filesystem::path resolve_library(const std::string& name, const linux_mapped_module& requesting_module,
                                          const linux_file_system& file_sys) const;

    linux_mapped_module* find_by_address(uint64_t address);
    linux_mapped_module* find_by_name(std::string_view name);

    const std::map<uint64_t, linux_mapped_module>& get_modules() const
    {
        return this->modules_;
    }

    const std::filesystem::path& get_executable_path() const
    {
        if (this->executable)
        {
            return this->executable->path;
        }

        static const std::filesystem::path empty{};
        return empty;
    }

  private:
    linux_memory_manager* memory_{};
    std::map<uint64_t, linux_mapped_module> modules_{};
    std::vector<std::filesystem::path> library_paths_{};

    linux_mapped_module* insert_module(linux_mapped_module mod);

    // Split a colon-separated path list (e.g., DT_RPATH value) and replace $ORIGIN
    // with the directory containing the requesting module.
    static std::vector<std::filesystem::path> split_search_paths(const std::string& paths, const std::filesystem::path& origin);
};

#pragma once

#include "../std_include.hpp"
#include "macho_mapping.hpp"

#include <guest/guest_file_system.hpp>

namespace sogen
{
    class macos_module_manager
    {
      public:
        explicit macos_module_manager(macos_memory_manager& memory)
            : memory_(&memory)
        {
        }

        macos_mapped_module* executable{};
        macos_mapped_module* dylinker{};

        void set_emulation_root(std::filesystem::path root)
        {
            this->file_system_ = guest_file_system{std::move(root)};
        }

        // The emulator's own guest_file_system carries the passthrough prefix for the directory the
        // sample was launched from. Building a second one from the root alone drops that prefix, so a
        // dylinker or dylib beside the executable resolves here and not there, or the other way round.
        void adopt_file_system(const guest_file_system& file_system)
        {
            this->file_system_ = file_system;
        }

        std::filesystem::path resolve_guest_path(std::string_view guest_path) const;

        macos_mapped_module* map_module(const std::filesystem::path& path, uint64_t forced_base = 0);
        void map_main_modules(const std::filesystem::path& executable_path);

        macos_mapped_module* find_by_address(uint64_t address);
        macos_mapped_module* find_by_name(std::string_view name);

        const std::map<uint64_t, macos_mapped_module>& get_modules() const
        {
            return this->modules_;
        }

      private:
        macos_memory_manager* memory_{};
        guest_file_system file_system_{};
        std::map<uint64_t, macos_mapped_module> modules_{};
    };
}

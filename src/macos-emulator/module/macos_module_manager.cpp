#include "../std_include.hpp"
#include "macos_module_manager.hpp"

#include <utils/io.hpp>

namespace sogen
{
    // LC_LOAD_DYLINKER is attacker-controlled input, so the joined path has to be normalized and contained
    // before it reaches the host file system: neither an absolute nor a "../" form may escape the root.
    std::filesystem::path macos_module_manager::resolve_guest_path(const std::string_view guest_path) const
    {
        return this->file_system_.translate(guest_path);
    }

    // map_macho_from_data does not unwind the segments it already mapped when a later one fails, so a failure
    // is propagated rather than reported as a null module: the guest address space is left partially occupied
    // and is not safe to map into again.
    macos_mapped_module* macos_module_manager::map_module(const std::filesystem::path& path, const uint64_t forced_base)
    {
        // utils::io::read_file streams until the ifstream stops being good, which never happens for a
        // character device: guest_file_system passes /dev/zero and /dev/urandom through to the host, so a
        // hostile LC_LOAD_DYLINKER naming one would grow the buffer until the analyzer dies.
        std::error_code error{};
        const auto status = std::filesystem::status(path, error);
        if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))
        {
            throw std::runtime_error("Mach-O image is not a regular file: " + path.string());
        }

        std::vector<std::byte> data{};
        if (!utils::io::read_file(path, &data))
        {
            throw std::runtime_error("Failed to read Mach-O image: " + path.string());
        }

        auto module = map_macho_from_data(*this->memory_, data, path, forced_base);
        const auto image_base = module.image_base;

        const auto [entry, inserted] = this->modules_.emplace(image_base, std::move(module));
        if (!inserted)
        {
            throw std::runtime_error("Duplicate module image base: " + path.string());
        }

        return &entry->second;
    }

    void macos_module_manager::map_main_modules(const std::filesystem::path& executable_path)
    {
        this->executable = this->map_module(executable_path);

        if (this->executable->dylinker_path.empty())
        {
            return;
        }

        const auto dylinker_path = this->resolve_guest_path(this->executable->dylinker_path);
        this->dylinker = this->map_module(dylinker_path, MACOS_DYLD_DEFAULT_BASE);
    }

    // The map is keyed by image_base, which is the mach header, while the mapped extent starts at
    // image_start. Those differ whenever a segment sits below the header, so an ordered lookup on the key
    // would miss the module.
    macos_mapped_module* macos_module_manager::find_by_address(const uint64_t address)
    {
        for (auto& [base, module] : this->modules_)
        {
            if (module.contains(address))
            {
                return &module;
            }
        }

        return nullptr;
    }

    macos_mapped_module* macos_module_manager::find_by_name(const std::string_view name)
    {
        for (auto& [base, module] : this->modules_)
        {
            if (module.name == name)
            {
                return &module;
            }
        }

        return nullptr;
    }
}

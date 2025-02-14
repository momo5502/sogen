#pragma once
#include <emulator.hpp>

#include "mapped_module.hpp"
#include "../file_system.hpp"
#include <utils/function.hpp>

class logger;

class module_manager
{
  public:
    using module_map = std::map<uint64_t, mapped_module>;
    utils::optional_function<void(mapped_module& mod)> on_module_load{};
    utils::optional_function<void(mapped_module& mod)> on_module_unload{};

    module_manager(memory_manager& memory, file_system& file_sys);

    void map_main_modules(const windows_path& executable_path, const windows_path& ntdll_path,
                          const windows_path& win32u_path, const logger& logger);

    mapped_module* map_module(const windows_path& file, const logger& logger, bool is_static = false);
    mapped_module* map_local_module(const std::filesystem::path& file, const logger& logger, bool is_static = false);

    mapped_module* find_by_address(const uint64_t address)
    {
        const auto entry = this->get_module(address);
        if (entry != this->modules_.end())
        {
            return &entry->second;
        }

        return nullptr;
    }

    const char* find_name(const uint64_t address)
    {
        const auto* mod = this->find_by_address(address);
        if (!mod)
        {
            return "<N/A>";
        }

        return mod->name.c_str();
    }

    void serialize(utils::buffer_serializer& buffer) const;
    void deserialize(utils::buffer_deserializer& buffer);

    bool unmap(uint64_t address, const logger& logger);
    const module_map& modules() const
    {
        return modules_;
    }

    // TODO: These is wrong here. A good mechanism for quick module access is needed.
    mapped_module* executable{};
    mapped_module* ntdll{};
    mapped_module* win32u{};

  private:
    memory_manager* memory_{};
    file_system* file_sys_{};

    module_map modules_{};

    module_map::iterator get_module(const uint64_t address)
    {
        if (this->modules_.empty())
        {
            return this->modules_.end();
        }

        auto upper_bound = this->modules_.upper_bound(address);
        if (upper_bound == this->modules_.begin())
        {
            return this->modules_.end();
        }

        std::advance(upper_bound, -1);
        return upper_bound;
    }
};

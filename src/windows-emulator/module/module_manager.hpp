#pragma once
#include "mapped_module.hpp"
#include <emulator.hpp>

class logger;

class module_manager
{
  public:
    using module_map = std::map<uint64_t, mapped_module>;
    module_manager(emulator& emu);

    mapped_module* map_module(const std::filesystem::path& file, logger& logger);

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

    bool unmap(const uint64_t address);
    const module_map& modules() const
    {
        return modules_;
    }

  private:
    emulator* emu_{};

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

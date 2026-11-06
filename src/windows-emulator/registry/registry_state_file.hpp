#pragma once

#include <filesystem>
#include <memory>

namespace sogen
{
    class registry_manager;

    class registry_state_file
    {
      public:
        explicit registry_state_file(const std::filesystem::path& path);
        ~registry_state_file();

        registry_state_file(const registry_state_file&) = delete;
        registry_state_file& operator=(const registry_state_file&) = delete;
        registry_state_file(registry_state_file&&) = delete;
        registry_state_file& operator=(registry_state_file&&) = delete;

        void load(registry_manager& registry) const;
        void save(const registry_manager& registry) const;

      private:
        struct implementation;
        std::unique_ptr<implementation> implementation_;
    };
}

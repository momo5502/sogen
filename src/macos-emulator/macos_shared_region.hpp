#pragma once

#include "macos_file_ids.hpp"

#include <posix_memory_manager.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sogen
{
    // The dyld shared cache as XNU presents it to a process: the whole region reserved, the dynamic data page
    // written up front, and cache pages brought in (and rebased) on first access.
    class macos_shared_region
    {
      public:
        macos_shared_region(posix_memory_manager& memory, const std::filesystem::path& main_cache_file, const std::string& guest_cache_path,
                            const macos_file_id& cache_file_id);

        uint64_t base_address() const
        {
            return this->base_address_;
        }

        bool handle_fault(uint64_t address);

      private:
        struct slide_info
        {
            uint64_t delta_mask{};
            uint64_t value_add{};
            std::vector<uint16_t> page_starts{};
            std::vector<uint16_t> page_extras{};
        };

        struct mapping
        {
            uint64_t address{};
            uint64_t size{};
            uint64_t file_offset{};
            memory_permission permissions{};
            size_t file_index{};
            std::optional<slide_info> slide{};
        };

        posix_memory_manager* memory_{};
        uint64_t base_address_{};
        uint64_t size_{};
        std::vector<std::ifstream> files_{};
        std::vector<mapping> mappings_{};

        void load_cache_file(const std::filesystem::path& path);
        void map_dynamic_region(uint64_t address, uint64_t size, const std::string& guest_cache_path, const macos_file_id& cache_file_id);
        static void rebase_page(const slide_info& slide, size_t page_index, std::span<std::byte> page);
    };
}

#include "macos_shared_region.hpp"
#include "dyld_cache_format.hpp"
#include "macos_memory_protection.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace sogen
{
    namespace
    {
        constexpr uint64_t MACOS_PAGE_SIZE = 0x1000;
        constexpr std::string_view CACHE_MAGIC = "dyld_v1  x86_64";
        constexpr std::string_view DYNAMIC_REGION_MAGIC = "dyld_data    v3";
        constexpr size_t DYNAMIC_REGION_FILE_ID_OFFSET = 0x10;
        constexpr size_t DYNAMIC_REGION_CACHE_PATH_OFFSET_FIELD = 0x24;
        constexpr uint32_t DYNAMIC_REGION_HEADER_SIZE = 0x50;

        void read_exact(std::ifstream& file, const uint64_t offset, void* data, const size_t size)
        {
            file.clear();
            file.seekg(static_cast<std::streamoff>(offset));
            file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
            if (!file || static_cast<size_t>(file.gcount()) != size)
            {
                throw std::runtime_error("Failed to read the dyld shared cache");
            }
        }

        template <typename T>
        T read_value(std::ifstream& file, const uint64_t offset)
        {
            T value{};
            read_exact(file, offset, &value, sizeof(value));
            return value;
        }

        std::vector<uint16_t> read_u16_array(std::ifstream& file, const uint64_t offset, const size_t count)
        {
            std::vector<uint16_t> values(count);
            read_exact(file, offset, values.data(), count * sizeof(uint16_t));
            return values;
        }

        void rebase_chain(const uint64_t delta_mask, const uint64_t value_add, const std::span<std::byte> page, uint64_t offset)
        {
            const auto delta_shift = std::countr_zero(delta_mask) - 2;
            const auto value_mask = ~delta_mask;

            uint64_t delta = 1;
            while (delta != 0 && offset + sizeof(uint64_t) <= page.size())
            {
                uint64_t raw{};
                std::memcpy(&raw, page.data() + offset, sizeof(raw));

                delta = (raw & delta_mask) >> delta_shift;
                auto value = raw & value_mask;
                if (value != 0)
                {
                    value += value_add;
                }

                std::memcpy(page.data() + offset, &value, sizeof(value));
                offset += delta;
            }
        }
    }

    macos_shared_region::macos_shared_region(posix_memory_manager& memory, const std::filesystem::path& main_cache_file,
                                             const std::string& guest_cache_path, const macos_file_id& cache_file_id)
    {
        this->load_cache_file(main_cache_file);

        auto& main_file = this->files_.front();

        std::array<char, dyld_cache::MAGIC_SIZE> magic{};
        read_exact(main_file, 0, magic.data(), magic.size());
        if (std::string_view{magic.data()} != CACHE_MAGIC)
        {
            throw std::runtime_error("Not an x86-64 dyld shared cache");
        }

        this->base_address_ = read_value<uint64_t>(main_file, dyld_cache::SHARED_REGION_START_OFFSET);
        this->size_ = read_value<uint64_t>(main_file, dyld_cache::SHARED_REGION_SIZE_OFFSET);

        const auto subcache_offset = read_value<uint32_t>(main_file, dyld_cache::SUBCACHE_ARRAY_OFFSET_OFFSET);
        const auto subcache_count = read_value<uint32_t>(main_file, dyld_cache::SUBCACHE_ARRAY_COUNT_OFFSET);
        const auto dynamic_data_offset = read_value<uint64_t>(main_file, dyld_cache::DYNAMIC_DATA_OFFSET_OFFSET);
        const auto dynamic_data_size = read_value<uint64_t>(main_file, dyld_cache::DYNAMIC_DATA_MAX_SIZE_OFFSET);

        for (uint32_t i = 0; i < subcache_count; ++i)
        {
            const auto entry =
                read_value<dyld_cache::subcache_entry>(this->files_.front(), subcache_offset + (i * sizeof(dyld_cache::subcache_entry)));
            const std::string_view suffix{entry.file_suffix.data(), strnlen(entry.file_suffix.data(), entry.file_suffix.size())};

            auto subcache_file = main_cache_file;
            subcache_file += suffix;
            this->load_cache_file(subcache_file);
        }

        if (!memory.reserve_memory(this->base_address_, static_cast<size_t>(this->size_)))
        {
            throw std::runtime_error("Failed to reserve the shared region");
        }

        map_dynamic_region(memory, this->base_address_ + dynamic_data_offset, dynamic_data_size, guest_cache_path, cache_file_id);
    }

    void macos_shared_region::load_cache_file(const std::filesystem::path& path)
    {
        auto& file = this->files_.emplace_back(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("Failed to open " + path.string());
        }

        const auto file_index = this->files_.size() - 1;
        const auto mapping_offset = read_value<uint32_t>(file, dyld_cache::MAPPING_WITH_SLIDE_OFFSET_OFFSET);
        const auto mapping_count = read_value<uint32_t>(file, dyld_cache::MAPPING_WITH_SLIDE_COUNT_OFFSET);

        for (uint32_t i = 0; i < mapping_count; ++i)
        {
            const auto info =
                read_value<dyld_cache::mapping_and_slide_info>(file, mapping_offset + (i * sizeof(dyld_cache::mapping_and_slide_info)));

            auto& entry = this->mappings_.emplace_back(mapping{
                .address = info.address,
                .size = info.size,
                .file_offset = info.file_offset,
                .permissions = to_memory_permission(info.init_prot),
                .file_index = file_index,
            });

            if (info.slide_info_file_size != 0)
            {
                const auto header = read_value<dyld_cache::slide_info_v2>(file, info.slide_info_file_offset);
                if (header.version != 2 || header.page_size != MACOS_PAGE_SIZE)
                {
                    throw std::runtime_error("Unsupported dyld shared cache slide info");
                }

                entry.slide = slide_info{
                    .delta_mask = header.delta_mask,
                    .value_add = header.value_add,
                    .page_starts = read_u16_array(file, info.slide_info_file_offset + header.page_starts_offset, header.page_starts_count),
                    .page_extras = read_u16_array(file, info.slide_info_file_offset + header.page_extras_offset, header.page_extras_count),
                };
            }
        }
    }

    // dyld requires a non-zero FileIdTuple, resolves the cache path from it with fsgetpath, and, outside Rosetta,
    // restarts into its own copy inside the cache.
    void macos_shared_region::map_dynamic_region(posix_memory_manager& memory, const uint64_t address, const uint64_t size,
                                                 const std::string& guest_cache_path, const macos_file_id& cache_file_id)
    {
        std::vector<std::byte> region(static_cast<size_t>(size));
        if (DYNAMIC_REGION_HEADER_SIZE + guest_cache_path.size() + 1 > region.size())
        {
            throw std::runtime_error("Shared cache path does not fit the dynamic region");
        }

        std::memcpy(region.data(), DYNAMIC_REGION_MAGIC.data(), DYNAMIC_REGION_MAGIC.size());
        std::memcpy(region.data() + DYNAMIC_REGION_FILE_ID_OFFSET, &cache_file_id, sizeof(cache_file_id));
        std::memcpy(region.data() + DYNAMIC_REGION_CACHE_PATH_OFFSET_FIELD, &DYNAMIC_REGION_HEADER_SIZE,
                    sizeof(DYNAMIC_REGION_HEADER_SIZE));
        std::memcpy(region.data() + DYNAMIC_REGION_HEADER_SIZE, guest_cache_path.c_str(), guest_cache_path.size() + 1);

        if (!memory.allocate_memory(address, region.size(), memory_permission::read_write))
        {
            throw std::runtime_error("Failed to map the shared cache dynamic region");
        }

        memory.write_memory(address, region.data(), region.size());
        memory.protect_memory(address, region.size(), memory_permission::read);
    }

    // A fault reports where the access starts, which may be the last bytes of a page that is already present while the
    // rest of an instruction or operand lies on the next one, so every page the access can touch is brought in.
    bool macos_shared_region::handle_fault(posix_memory_manager& memory, const uint64_t address, const size_t size)
    {
        constexpr size_t max_instruction_length = 15;

        if (address < this->base_address_ || address - this->base_address_ >= this->size_)
        {
            return false;
        }

        const auto remaining = this->size_ - (address - this->base_address_);
        const auto access_length = std::min<uint64_t>(std::max(size, max_instruction_length), remaining);
        const auto first_page = page_align_down(address);
        const auto page_count = ((page_align_down(address + access_length - 1) - first_page) / MACOS_PAGE_SIZE) + 1;

        bool mapped_any = false;
        for (uint64_t i = 0; i < page_count; ++i)
        {
            mapped_any |= this->map_page(memory, first_page + (i * MACOS_PAGE_SIZE));
        }

        return mapped_any;
    }

    bool macos_shared_region::map_page(posix_memory_manager& memory, const uint64_t address)
    {
        if (address < this->base_address_ || address - this->base_address_ >= this->size_)
        {
            return false;
        }

        const auto it = std::ranges::find_if(
            this->mappings_, [&](const mapping& entry) { return address >= entry.address && address - entry.address < entry.size; });
        if (it == this->mappings_.end())
        {
            return false;
        }

        const auto page_address = page_align_down(address);
        const auto offset_in_mapping = page_address - it->address;

        std::vector<std::byte> page(MACOS_PAGE_SIZE);
        read_exact(this->files_.at(it->file_index), it->file_offset + offset_in_mapping, page.data(),
                   static_cast<size_t>(std::min(MACOS_PAGE_SIZE, it->size - offset_in_mapping)));

        if (it->slide)
        {
            rebase_page(*it->slide, static_cast<size_t>(offset_in_mapping / MACOS_PAGE_SIZE), page);
        }

        if (!memory.allocate_memory(page_address, page.size(), memory_permission::read_write))
        {
            return false;
        }

        memory.write_memory(page_address, page.data(), page.size());
        memory.protect_memory(page_address, page.size(), it->permissions);
        return true;
    }

    void macos_shared_region::rebase_page(const slide_info& slide, const size_t page_index, const std::span<std::byte> page)
    {
        const auto page_start = slide.page_starts.at(page_index);
        if (page_start == dyld_cache::SLIDE_PAGE_ATTR_NO_REBASE)
        {
            return;
        }

        if ((page_start & dyld_cache::SLIDE_PAGE_ATTR_EXTRA) == 0)
        {
            rebase_chain(slide.delta_mask, slide.value_add, page, static_cast<uint64_t>(page_start) * sizeof(uint32_t));
            return;
        }

        auto extra_index = static_cast<size_t>(page_start & dyld_cache::SLIDE_PAGE_VALUE_MASK);
        while (true)
        {
            const auto extra = slide.page_extras.at(extra_index++);
            rebase_chain(slide.delta_mask, slide.value_add, page,
                         static_cast<uint64_t>(extra & dyld_cache::SLIDE_PAGE_VALUE_MASK) * sizeof(uint32_t));
            if (extra & dyld_cache::SLIDE_PAGE_ATTR_END)
            {
                break;
            }
        }
    }
}

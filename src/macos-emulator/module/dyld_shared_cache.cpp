#include "../std_include.hpp"
#include "dyld_shared_cache.hpp"

#include "macho_mapping.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>

namespace sogen
{
    namespace
    {
        namespace cache = macho::dyld_cache;

        constexpr uint64_t CACHE_HEADER_SIZE = 0x400;
        constexpr uint64_t MAX_IMAGE_PATH_SIZE = 1024;

        size_t clamp_read_size(const uint64_t size)
        {
            return static_cast<size_t>(std::min<uint64_t>(size, std::numeric_limits<size_t>::max()));
        }

        class range_data_source : public dyld_cache_data_source
        {
          public:
            range_data_source(host_range_reader& reader, std::filesystem::path path)
                : reader_(&reader),
                  path_(std::move(path)),
                  size_(reader.file_size(this->path_.string()))
            {
            }

            std::vector<std::byte> read(const uint64_t offset, const uint64_t size) override
            {
                // Redundant with the resize below, which yields an empty vector once the reader declines
                // the range, and kept for the reason the same check is kept in sys_mmap: it refuses an
                // offset past the end while it is still the caller's raw value, before the subtraction
                // underflows. No test can tell the two layers apart.
                if (offset >= this->size_)
                {
                    return {};
                }

                std::vector<std::byte> data(clamp_read_size(std::min(size, this->size_ - offset)));
                const auto read_bytes = this->reader_->read(this->path_.string(), offset, data);

                // Resized to what actually arrived rather than what was asked for: a caller handed the
                // full buffer after a short read would parse the tail as though it were file content,
                // and the zeroes there are indistinguishable from real data.
                data.resize(read_bytes);

                return data;
            }

          private:
            host_range_reader* reader_{};
            std::filesystem::path path_{};
            uint64_t size_{};
        };

        class file_data_source : public dyld_cache_data_source
        {
          public:
            explicit file_data_source(const std::filesystem::path& path)
                : stream_(path, std::ios::binary)
            {
                std::error_code error{};
                const auto size = std::filesystem::file_size(path, error);
                this->size_ = error ? 0 : size;
            }

            bool is_readable() const
            {
                return this->stream_.good() && this->size_ > 0;
            }

            std::vector<std::byte> read(const uint64_t offset, const uint64_t size) override
            {
                if (offset >= this->size_)
                {
                    return {};
                }

                std::vector<std::byte> data(clamp_read_size(std::min(size, this->size_ - offset)));

                this->stream_.seekg(static_cast<std::streamoff>(offset));
                this->stream_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));

                data.resize(static_cast<size_t>(std::max<std::streamsize>(this->stream_.gcount(), 0)));
                this->stream_.clear();

                return data;
            }

          private:
            std::ifstream stream_;
            uint64_t size_{};
        };

        class memory_data_source : public dyld_cache_data_source
        {
          public:
            explicit memory_data_source(std::vector<std::byte> data)
                : data_(std::move(data))
            {
            }

            std::vector<std::byte> read(const uint64_t offset, const uint64_t size) override
            {
                if (offset >= this->data_.size())
                {
                    return {};
                }

                const auto* start = this->data_.data() + static_cast<size_t>(offset);
                const auto available = clamp_read_size(std::min<uint64_t>(size, this->data_.size() - offset));

                return {start, start + available};
            }

          private:
            std::vector<std::byte> data_;
        };

        template <typename T>
        T require(const std::span<const std::byte> data, const uint64_t offset, const std::filesystem::path& path)
        {
            const auto value = macho::read_at<T>(data, offset);
            if (!value)
            {
                throw std::runtime_error("Truncated dyld shared cache: " + path.string());
            }

            return *value;
        }

        std::unique_ptr<dyld_cache_data_source> open_source(const dyld_cache_source_opener& opener, const std::filesystem::path& path)
        {
            auto source = opener ? opener(path) : nullptr;
            if (!source)
            {
                throw std::runtime_error("Failed to open dyld shared cache file: " + path.string());
            }

            return source;
        }

        std::vector<std::byte> read_header(dyld_cache_data_source& source, const std::filesystem::path& path)
        {
            auto header = source.read(0, CACHE_HEADER_SIZE);
            if (!cache::has_magic(header))
            {
                throw std::runtime_error("Not a dyld shared cache: " + path.string());
            }

            return header;
        }

        template <typename T>
        std::vector<std::byte> read_entries(dyld_cache_data_source& source, const uint64_t offset, const uint64_t count,
                                            const std::filesystem::path& path)
        {
            if (count > std::numeric_limits<uint64_t>::max() / sizeof(T))
            {
                throw std::runtime_error("Implausible entry count in dyld shared cache: " + path.string());
            }

            return source.read(offset, count * sizeof(T));
        }

        std::vector<dyld_cache_region> read_regions(dyld_cache_data_source& source, const std::span<const std::byte> header,
                                                    const std::filesystem::path& path)
        {
            using mapping = cache::dyld_cache_mapping_and_slide_info;

            const auto offset = require<uint32_t>(header, cache::MAPPING_WITH_SLIDE_OFFSET, path);
            const auto count = require<uint32_t>(header, cache::MAPPING_WITH_SLIDE_COUNT, path);

            const auto entries = read_entries<mapping>(source, offset, count, path);

            std::vector<dyld_cache_region> regions{};
            regions.reserve(entries.size() / sizeof(mapping));

            for (uint32_t i = 0; i < count; ++i)
            {
                const auto entry = require<mapping>(entries, static_cast<uint64_t>(i) * sizeof(mapping), path);

                regions.push_back(dyld_cache_region{
                    .address = entry.address,
                    .size = entry.size,
                    .file_offset = entry.file_offset,
                    .flags = entry.flags,
                    .initial_permissions = macho_prot_to_permission(entry.init_prot),
                    .max_permissions = macho_prot_to_permission(entry.max_prot),
                    .has_slide_info = entry.slide_info_file_size != 0,
                });
            }

            return regions;
        }

        std::string read_string(dyld_cache_data_source& source, const uint64_t offset, const std::filesystem::path& path)
        {
            const auto data = source.read(offset, MAX_IMAGE_PATH_SIZE);
            if (data.empty())
            {
                throw std::runtime_error("Image path outside the dyld shared cache: " + path.string());
            }

            const auto* text = reinterpret_cast<const char*>(data.data());
            const auto length = ::strnlen(text, data.size());

            if (length == data.size())
            {
                throw std::runtime_error("Unterminated image path in dyld shared cache: " + path.string());
            }

            return std::string{text, length};
        }

        std::filesystem::path resolve_subcache_path(const std::filesystem::path& main_cache_file, const cache::dyld_subcache_entry& entry)
        {
            const std::string suffix{entry.file_suffix.data(), ::strnlen(entry.file_suffix.data(), entry.file_suffix.size())};

            if (suffix.empty() || suffix.find_first_of("/\\") != std::string::npos)
            {
                throw std::runtime_error("Invalid dyld subcache suffix: " + main_cache_file.string());
            }

            auto path = main_cache_file;
            path += suffix;
            return path;
        }

        std::vector<dyld_cache_file_info> read_subcaches(dyld_cache_data_source& source, const std::span<const std::byte> header,
                                                         const std::filesystem::path& main_cache_file,
                                                         const dyld_cache_source_opener& opener)
        {
            using subcache = cache::dyld_subcache_entry;

            const auto offset = require<uint32_t>(header, cache::SUBCACHE_ARRAY_OFFSET, main_cache_file);
            const auto count = require<uint32_t>(header, cache::SUBCACHE_ARRAY_COUNT, main_cache_file);

            const auto entries = read_entries<subcache>(source, offset, count, main_cache_file);

            std::vector<dyld_cache_file_info> files{};
            files.reserve(entries.size() / sizeof(subcache));

            for (uint32_t i = 0; i < count; ++i)
            {
                const auto entry = require<subcache>(entries, static_cast<uint64_t>(i) * sizeof(subcache), main_cache_file);
                const auto path = resolve_subcache_path(main_cache_file, entry);

                auto subcache_source = open_source(opener, path);
                const auto subcache_header = read_header(*subcache_source, path);

                const auto uuid = require<std::array<uint8_t, 16>>(subcache_header, cache::UUID, path);
                if (uuid != entry.uuid)
                {
                    throw std::runtime_error("dyld subcache UUID mismatch: " + path.string());
                }

                files.push_back(dyld_cache_file_info{
                    .path = path,
                    .uuid = uuid,
                    .vm_offset = entry.cache_vm_offset,
                    .regions = read_regions(*subcache_source, subcache_header, path),
                });
            }

            return files;
        }

        std::vector<dyld_cache_image_entry> read_images(dyld_cache_data_source& source, const std::span<const std::byte> header,
                                                        const std::filesystem::path& path)
        {
            using image = cache::dyld_cache_image_text_info;

            const auto offset = require<uint64_t>(header, cache::IMAGES_TEXT_OFFSET, path);
            const auto count = require<uint64_t>(header, cache::IMAGES_TEXT_COUNT, path);

            const auto entries = read_entries<image>(source, offset, count, path);

            std::vector<dyld_cache_image_entry> images{};
            images.reserve(entries.size() / sizeof(image));

            for (uint64_t i = 0; i < count; ++i)
            {
                const auto entry = require<image>(entries, i * sizeof(image), path);

                images.push_back(dyld_cache_image_entry{
                    .address = entry.load_address,
                    .text_size = entry.text_segment_size,
                    .path = read_string(source, entry.path_offset, path),
                });
            }

            std::ranges::sort(
                images, //
                [](const dyld_cache_image_entry& lhs, const dyld_cache_image_entry& rhs) { return lhs.address < rhs.address; });

            return images;
        }
    }

    std::unique_ptr<dyld_cache_data_source> open_dyld_cache_file(const std::filesystem::path& path)
    {
        auto source = std::make_unique<file_data_source>(path);
        if (!source->is_readable())
        {
            return nullptr;
        }

        return source;
    }

    std::unique_ptr<dyld_cache_data_source> make_dyld_cache_range_source(host_range_reader& reader, const std::filesystem::path& path)
    {
        return std::make_unique<range_data_source>(reader, path);
    }

    dyld_cache_source_opener make_host_range_cache_opener(host_range_reader& reader)
    {
        return [&reader](const std::filesystem::path& path) { return make_dyld_cache_range_source(reader, path); };
    }

    std::unique_ptr<dyld_cache_data_source> make_dyld_cache_memory_source(std::vector<std::byte> data)
    {
        return std::make_unique<memory_data_source>(std::move(data));
    }

    dyld_shared_cache_reader dyld_shared_cache_reader::open(const std::filesystem::path& main_cache_file)
    {
        return parse(main_cache_file, [](const std::filesystem::path& path) { return open_dyld_cache_file(path); });
    }

    dyld_shared_cache_reader dyld_shared_cache_reader::parse(const std::filesystem::path& main_cache_file,
                                                             const dyld_cache_source_opener& opener)
    {
        auto source = open_source(opener, main_cache_file);
        const auto header = read_header(*source, main_cache_file);

        dyld_shared_cache_reader reader{};
        reader.opener_ = opener;
        reader.architecture_ = std::string{cache::architecture(header)};
        reader.platform_ = require<uint32_t>(header, cache::PLATFORM, main_cache_file);
        reader.shared_region_start_ = require<uint64_t>(header, cache::SHARED_REGION_START, main_cache_file);
        reader.shared_region_size_ = require<uint64_t>(header, cache::SHARED_REGION_SIZE, main_cache_file);
        reader.max_slide_ = require<uint64_t>(header, cache::MAX_SLIDE, main_cache_file);
        reader.dyld_in_cache_address_ = require<uint64_t>(header, cache::DYLD_IN_CACHE_MH, main_cache_file);
        reader.dylibs_expected_on_disk_ =
            (require<uint32_t>(header, cache::FORMAT_FLAGS, main_cache_file) & cache::FORMAT_FLAG_DYLIBS_EXPECTED_ON_DISK) != 0;

        // The base file describes only its own 544 KiB of metadata; on macOS 26 it holds a single mapping and
        // every other mapping, slide-info blob and protection lives in a subcache header.
        reader.files_.push_back(dyld_cache_file_info{
            .path = main_cache_file,
            .uuid = require<std::array<uint8_t, 16>>(header, cache::UUID, main_cache_file),
            .vm_offset = 0,
            .regions = read_regions(*source, header, main_cache_file),
        });

        auto subcaches = read_subcaches(*source, header, main_cache_file, opener);
        reader.files_.insert(reader.files_.end(), std::make_move_iterator(subcaches.begin()), std::make_move_iterator(subcaches.end()));

        reader.images_ = read_images(*source, header, main_cache_file);

        return reader;
    }

    std::vector<std::byte> dyld_shared_cache_reader::read_at_address(const uint64_t unslid_address, const uint64_t size) const
    {
        if (size == 0 || !this->opener_)
        {
            return {};
        }

        for (const auto& file : this->files_)
        {
            for (const auto& region : file.regions)
            {
                if (unslid_address < region.address || (unslid_address - region.address) >= region.size)
                {
                    continue;
                }

                const auto offset_in_region = unslid_address - region.address;

                // Clamped to the region: a read that ran past it would silently continue into whatever
                // the next part of the file holds, which is a different mapping at a different address.
                const auto available = std::min(size, region.size - offset_in_region);

                auto entry = this->sources_.find(file.path);
                if (entry == this->sources_.end())
                {
                    auto source = this->opener_(file.path);
                    if (!source)
                    {
                        return {};
                    }

                    entry = this->sources_.emplace(file.path, std::move(source)).first;
                }

                return entry->second->read(region.file_offset + offset_in_region, available);
            }
        }

        return {};
    }

    const dyld_cache_image_entry* dyld_shared_cache_reader::find_image_by_path(const std::string_view path) const
    {
        for (const auto& image : this->images_)
        {
            if (image.path == path)
            {
                return &image;
            }
        }

        return nullptr;
    }

    const dyld_cache_image_entry* dyld_shared_cache_reader::find_image_by_address(const uint64_t unslid_address) const
    {
        auto entry = std::ranges::upper_bound(this->images_, unslid_address, std::less{},
                                              [](const dyld_cache_image_entry& image) { return image.address; });

        if (entry == this->images_.begin())
        {
            return nullptr;
        }

        --entry;

        if (unslid_address - entry->address >= entry->text_size)
        {
            return nullptr;
        }

        return &*entry;
    }
}

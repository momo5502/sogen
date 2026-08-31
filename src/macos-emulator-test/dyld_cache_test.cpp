#include <module/dyld_cache_pager.hpp>
#include <host_range_reader.hpp>
#include <span>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

#include <module/dyld_shared_cache.hpp>
#include <platform/macho.hpp>

#include "fixture_utils.hpp"

namespace
{
    namespace cache = sogen::macho::dyld_cache;

    std::vector<std::byte> synthetic_cache_header()
    {
        std::vector<std::byte> data(0x400, std::byte{0});

        constexpr std::string_view magic = "dyld_v1  arm64e";
        std::ranges::transform(magic, data.begin(), [](const char c) { return static_cast<std::byte>(c); });

        const auto put32 = [&](const uint64_t offset, const uint32_t value) { //
            std::memcpy(data.data() + offset, &value, sizeof(value));
        };
        const auto put64 = [&](const uint64_t offset, const uint64_t value) { //
            std::memcpy(data.data() + offset, &value, sizeof(value));
        };

        put32(cache::MAPPING_OFFSET, 0x228);
        put32(cache::MAPPING_COUNT, 1);
        put32(cache::PLATFORM, sogen::macho::PLATFORM_MACOS);
        put32(cache::FORMAT_FLAGS, 0x1000);
        put64(cache::SHARED_REGION_START, 0x180000000ULL);
        put64(cache::SHARED_REGION_SIZE, 0x166444000ULL);
        put64(cache::MAX_SLIDE, 0x10000000ULL);
        put32(cache::SUBCACHE_ARRAY_OFFSET, 0x392d8);
        put32(cache::SUBCACHE_ARRAY_COUNT, 12);
        put32(cache::IMAGES_OFFSET, 0x298);
        put32(cache::IMAGES_COUNT, 3649);

        return data;
    }

    std::optional<std::vector<std::byte>> read_range(const std::filesystem::path& path, const uint64_t offset, const size_t size)
    {
        std::ifstream file{path, std::ios::binary};
        if (!file)
        {
            return std::nullopt;
        }

        file.seekg(static_cast<std::streamoff>(offset));

        std::vector<std::byte> data(size);
        if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
        {
            return std::nullopt;
        }

        return data;
    }

    TEST(DyldCacheFormat, StructuresHaveTheOnDiskSizes)
    {
        static_assert(sizeof(cache::dyld_cache_mapping_info) == 32);
        static_assert(sizeof(cache::dyld_cache_mapping_and_slide_info) == 56);
        static_assert(sizeof(cache::dyld_cache_image_info) == 32);
        static_assert(sizeof(cache::dyld_cache_image_text_info) == 32);
        static_assert(sizeof(cache::dyld_subcache_entry) == 56);
        static_assert(sizeof(cache::dyld_cache_slide_info5) == cache::SLIDE_INFO5_HEADER_SIZE);
        SUCCEED();
    }

    TEST(DyldCacheFormat, SubcacheArrayEndsExactlyAtTheTproMappings)
    {
        EXPECT_EQ(0x392d8u + 12u * sizeof(cache::dyld_subcache_entry), 0x39578u);
    }

    TEST(DyldCacheFormat, RecognisesMagicAndArchitecture)
    {
        const auto data = synthetic_cache_header();

        EXPECT_TRUE(cache::has_magic(data));
        EXPECT_EQ(cache::architecture(data), "arm64e");

        const std::vector<std::byte> garbage(64, std::byte{0x41});
        EXPECT_FALSE(cache::has_magic(garbage));
        EXPECT_TRUE(cache::architecture(garbage).empty());

        const std::vector<std::byte> truncated(8, std::byte{0});
        EXPECT_FALSE(cache::has_magic(truncated));
        EXPECT_TRUE(cache::architecture(truncated).empty());
    }

    TEST(DyldCacheFormat, NamedOffsetsReadBackSyntheticFields)
    {
        const auto data = synthetic_cache_header();

        EXPECT_EQ(sogen::macho::read_at<uint32_t>(data, cache::MAPPING_OFFSET).value(), 0x228u);
        EXPECT_EQ(sogen::macho::read_at<uint32_t>(data, cache::MAPPING_COUNT).value(), 1u);
        EXPECT_EQ(sogen::macho::read_at<uint64_t>(data, cache::SHARED_REGION_START).value(), 0x180000000ULL);
        EXPECT_EQ(sogen::macho::read_at<uint64_t>(data, cache::SHARED_REGION_SIZE).value(), 0x166444000ULL);
        EXPECT_EQ(sogen::macho::read_at<uint32_t>(data, cache::SUBCACHE_ARRAY_COUNT).value(), 12u);
        EXPECT_EQ(sogen::macho::read_at<uint32_t>(data, cache::IMAGES_COUNT).value(), 3649u);

        const auto flags = sogen::macho::read_at<uint32_t>(data, cache::FORMAT_FLAGS).value();
        EXPECT_EQ(flags & cache::FORMAT_FLAG_DYLIBS_EXPECTED_ON_DISK, 0u);
    }

    TEST(DyldCacheFormat, ReadsTheRealHostCacheHeader)
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(path))
        {
            GTEST_SKIP() << "no dyld shared cache on this host";
        }

        std::vector<std::byte> data{};
        ASSERT_TRUE(sogen::utils::io::read_file(path, &data));
        ASSERT_GE(data.size(), 0x228u);

        EXPECT_TRUE(cache::has_magic(data));
        EXPECT_EQ(cache::architecture(data), "arm64e");

        EXPECT_EQ(sogen::macho::read_at<uint32_t>(data, cache::MAPPING_OFFSET).value(), 0x228u);
        EXPECT_EQ(sogen::macho::read_at<uint32_t>(data, cache::PLATFORM).value(), sogen::macho::PLATFORM_MACOS);
        EXPECT_EQ(sogen::macho::read_at<uint64_t>(data, cache::SHARED_REGION_START).value(), 0x180000000ULL);

        const auto subcache_offset = sogen::macho::read_at<uint32_t>(data, cache::SUBCACHE_ARRAY_OFFSET).value();
        const auto subcache_count = sogen::macho::read_at<uint32_t>(data, cache::SUBCACHE_ARRAY_COUNT).value();
        const auto tpro_offset = sogen::macho::read_at<uint32_t>(data, cache::TPRO_MAPPINGS_OFFSET).value();
        EXPECT_EQ(subcache_offset + subcache_count * sizeof(cache::dyld_subcache_entry), tpro_offset);

        const auto signature_offset = sogen::macho::read_at<uint64_t>(data, cache::CODE_SIGNATURE_OFFSET).value();
        const auto signature_size = sogen::macho::read_at<uint64_t>(data, cache::CODE_SIGNATURE_SIZE).value();
        EXPECT_EQ(signature_offset + signature_size, data.size());

        const auto mapping_offset = sogen::macho::read_at<uint32_t>(data, cache::MAPPING_OFFSET).value();
        const auto mapping = sogen::macho::read_at<cache::dyld_cache_mapping_info>(data, mapping_offset);
        ASSERT_TRUE(mapping.has_value());
        EXPECT_EQ(mapping->address, sogen::macho::read_at<uint64_t>(data, cache::SHARED_REGION_START).value());
        EXPECT_EQ(mapping->file_offset, 0u);
        EXPECT_EQ(mapping->init_prot, 5u);

        const auto slide_mapping_offset = sogen::macho::read_at<uint32_t>(data, cache::MAPPING_WITH_SLIDE_OFFSET).value();
        const auto slide_mapping = sogen::macho::read_at<cache::dyld_cache_mapping_and_slide_info>(data, slide_mapping_offset);
        ASSERT_TRUE(slide_mapping.has_value());
        EXPECT_EQ(slide_mapping->address, mapping->address);
        EXPECT_EQ(slide_mapping->size, mapping->size);
        EXPECT_EQ(slide_mapping->file_offset, mapping->file_offset);
        EXPECT_EQ(slide_mapping->init_prot, mapping->init_prot);

        const auto text_offset = sogen::macho::read_at<uint64_t>(data, cache::IMAGES_TEXT_OFFSET).value();
        const auto text_count = sogen::macho::read_at<uint64_t>(data, cache::IMAGES_TEXT_COUNT).value();
        const auto images_offset = sogen::macho::read_at<uint32_t>(data, cache::IMAGES_OFFSET).value();
        const auto images_count = sogen::macho::read_at<uint32_t>(data, cache::IMAGES_COUNT).value();
        ASSERT_EQ(text_count, images_count);
        ASSERT_GT(text_count, 2u);

        const auto dyld_in_cache = sogen::macho::read_at<uint64_t>(data, cache::DYLD_IN_CACHE_MH).value();

        bool found_dyld = false;
        for (uint64_t i = 0; i < text_count; ++i)
        {
            const auto entry =
                sogen::macho::read_at<cache::dyld_cache_image_text_info>(data, text_offset + i * sizeof(cache::dyld_cache_image_text_info));
            ASSERT_TRUE(entry.has_value());

            const auto image =
                sogen::macho::read_at<cache::dyld_cache_image_info>(data, images_offset + i * sizeof(cache::dyld_cache_image_info));
            ASSERT_TRUE(image.has_value());
            ASSERT_EQ(image->address, entry->load_address);
            ASSERT_EQ(image->path_file_offset, entry->path_offset);

            if (entry->load_address != dyld_in_cache)
            {
                continue;
            }

            const auto* name = reinterpret_cast<const char*>(data.data()) + entry->path_offset;
            EXPECT_STREQ(name, "/usr/lib/dyld");
            found_dyld = true;
            break;
        }

        EXPECT_TRUE(found_dyld) << "no image_text_info entry matched dyldInCacheMH - entry size is probably wrong";
    }

    TEST(DyldCacheFormat, DecodesSlideInfoInAHostSubcache)
    {
        const std::filesystem::path base{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(base))
        {
            GTEST_SKIP() << "no dyld shared cache on this host";
        }

        std::vector<std::byte> data{};
        ASSERT_TRUE(sogen::utils::io::read_file(base, &data));

        const auto subcache_offset = sogen::macho::read_at<uint32_t>(data, cache::SUBCACHE_ARRAY_OFFSET).value();
        const auto subcache_count = sogen::macho::read_at<uint32_t>(data, cache::SUBCACHE_ARRAY_COUNT).value();
        ASSERT_GT(subcache_count, 0u);

        bool decoded_slide_info = false;

        for (uint32_t i = 0; i < subcache_count; ++i)
        {
            const auto entry =
                sogen::macho::read_at<cache::dyld_subcache_entry>(data, subcache_offset + i * sizeof(cache::dyld_subcache_entry));
            ASSERT_TRUE(entry.has_value());

            const std::string_view suffix{entry->file_suffix.data(), ::strnlen(entry->file_suffix.data(), entry->file_suffix.size())};
            ASSERT_FALSE(suffix.empty());
            ASSERT_EQ(suffix.front(), '.');

            const auto path = base.parent_path() / (base.filename().string() + std::string{suffix});
            if (!std::filesystem::exists(path))
            {
                continue;
            }

            const auto header = read_range(path, 0, 0x600);
            ASSERT_TRUE(header.has_value());
            ASSERT_TRUE(cache::has_magic(*header));

            const auto slide_mapping_offset = sogen::macho::read_at<uint32_t>(*header, cache::MAPPING_WITH_SLIDE_OFFSET).value();
            const auto slide_mapping_count = sogen::macho::read_at<uint32_t>(*header, cache::MAPPING_WITH_SLIDE_COUNT).value();

            for (uint32_t j = 0; j < slide_mapping_count; ++j)
            {
                const auto mapping = sogen::macho::read_at<cache::dyld_cache_mapping_and_slide_info>(
                    *header, slide_mapping_offset + j * sizeof(cache::dyld_cache_mapping_and_slide_info));
                ASSERT_TRUE(mapping.has_value());

                if (mapping->slide_info_file_offset == 0 || mapping->slide_info_file_size == 0)
                {
                    continue;
                }

                const auto blob = read_range(path, mapping->slide_info_file_offset, cache::SLIDE_INFO5_HEADER_SIZE);
                ASSERT_TRUE(blob.has_value());

                const auto slide = sogen::macho::read_at<cache::dyld_cache_slide_info5>(*blob, 0);
                ASSERT_TRUE(slide.has_value());

                EXPECT_EQ(slide->version, cache::SLIDE_INFO5_VERSION);
                EXPECT_EQ(slide->page_size, 0x4000u);
                EXPECT_EQ(slide->value_add, 0x180000000ULL);
                EXPECT_EQ(cache::SLIDE_INFO5_HEADER_SIZE + slide->page_starts_count * sizeof(uint16_t), mapping->slide_info_file_size);
                EXPECT_EQ(static_cast<uint64_t>(slide->page_starts_count) * slide->page_size, mapping->size);

                decoded_slide_info = true;
            }

            if (decoded_slide_info)
            {
                break;
            }
        }

        EXPECT_TRUE(decoded_slide_info) << "no subcache mapping carried decodable v5 slide info";
    }
}

namespace
{
    constexpr uint64_t SYNTHETIC_HEADER_SIZE = 0x400;
    constexpr uint64_t SYNTHETIC_REGION_START = 0x180000000ULL;
    constexpr uint64_t SYNTHETIC_DYLD_ADDRESS = 0x180010000ULL;

    std::array<uint8_t, 16> make_uuid(const uint8_t seed)
    {
        std::array<uint8_t, 16> uuid{};
        uuid.fill(seed);
        return uuid;
    }

    class cache_builder
    {
      public:
        cache_builder()
            : data_(SYNTHETIC_HEADER_SIZE, std::byte{0})
        {
            constexpr std::string_view magic = "dyld_v1  arm64e";
            std::ranges::transform(magic, this->data_.begin(), [](const char c) { return static_cast<std::byte>(c); });
        }

        void put32(const uint64_t offset, const uint32_t value)
        {
            std::memcpy(this->data_.data() + offset, &value, sizeof(value));
        }

        void put64(const uint64_t offset, const uint64_t value)
        {
            std::memcpy(this->data_.data() + offset, &value, sizeof(value));
        }

        void put_uuid(const uint64_t offset, const std::array<uint8_t, 16>& uuid)
        {
            std::memcpy(this->data_.data() + offset, uuid.data(), uuid.size());
        }

        template <typename T>
        uint64_t append(const T& value)
        {
            const auto offset = this->data_.size();
            this->data_.resize(offset + sizeof(T));
            std::memcpy(this->data_.data() + offset, &value, sizeof(T));
            return offset;
        }

        uint64_t append_string(const std::string_view text, const bool terminate = true)
        {
            const auto offset = this->data_.size();
            this->data_.resize(offset + text.size() + (terminate ? 1u : 0u), std::byte{0});
            std::memcpy(this->data_.data() + offset, text.data(), text.size());
            return offset;
        }

        std::vector<std::byte> take()
        {
            return std::move(this->data_);
        }

      private:
        std::vector<std::byte> data_;
    };

    cache::dyld_cache_mapping_and_slide_info make_mapping(const uint64_t address, const uint64_t size, const uint32_t prot,
                                                          const uint64_t slide_info_size)
    {
        return cache::dyld_cache_mapping_and_slide_info{
            .address = address,
            .size = size,
            .file_offset = 0x4000,
            .slide_info_file_offset = slide_info_size == 0 ? 0u : 0x8000u,
            .slide_info_file_size = slide_info_size,
            .flags = slide_info_size == 0 ? uint64_t{0} : cache::MAPPING_FLAG_AUTH_DATA,
            .max_prot = prot,
            .init_prot = prot,
        };
    }

    std::vector<std::byte> build_subcache(const std::array<uint8_t, 16>& uuid,
                                          const std::vector<cache::dyld_cache_mapping_and_slide_info>& mappings)
    {
        cache_builder builder{};
        builder.put_uuid(cache::UUID, uuid);

        uint64_t first_mapping = 0;
        for (const auto& mapping : mappings)
        {
            const auto offset = builder.append(mapping);
            first_mapping = first_mapping == 0 ? offset : first_mapping;
        }

        builder.put32(cache::MAPPING_WITH_SLIDE_OFFSET, static_cast<uint32_t>(first_mapping));
        builder.put32(cache::MAPPING_WITH_SLIDE_COUNT, static_cast<uint32_t>(mappings.size()));

        return builder.take();
    }

    struct synthetic_cache
    {
        std::filesystem::path main_path{"/synthetic/dyld_shared_cache_arm64e"};
        std::map<std::filesystem::path, std::vector<std::byte>> files{};
        uint64_t subcache_array_offset{};
        uint64_t image_text_offset{};

        void patch32(const std::filesystem::path& path, const uint64_t offset, const uint32_t value)
        {
            std::memcpy(this->files.at(path).data() + offset, &value, sizeof(value));
        }

        void patch64(const std::filesystem::path& path, const uint64_t offset, const uint64_t value)
        {
            std::memcpy(this->files.at(path).data() + offset, &value, sizeof(value));
        }

        sogen::dyld_cache_source_opener opener() const
        {
            return [files = this->files](const std::filesystem::path& path) -> std::unique_ptr<sogen::dyld_cache_data_source> {
                const auto entry = files.find(path);
                if (entry == files.end())
                {
                    return nullptr;
                }

                return sogen::make_dyld_cache_memory_source(entry->second);
            };
        }
    };

    synthetic_cache make_synthetic_cache()
    {
        cache_builder builder{};

        builder.put32(cache::PLATFORM, sogen::macho::PLATFORM_MACOS);
        builder.put32(cache::FORMAT_FLAGS, 0);
        builder.put64(cache::SHARED_REGION_START, SYNTHETIC_REGION_START);
        builder.put64(cache::SHARED_REGION_SIZE, 0x40000000ULL);
        builder.put64(cache::MAX_SLIDE, 0x10000000ULL);
        builder.put64(cache::DYLD_IN_CACHE_MH, SYNTHETIC_DYLD_ADDRESS);
        builder.put_uuid(cache::UUID, make_uuid(0xa0));

        const auto decoy = builder.append(cache::dyld_cache_mapping_info{
            .address = 0xdeadbeef000ULL,
            .size = 0x1000,
            .file_offset = 0,
            .max_prot = 0,
            .init_prot = 0,
        });

        builder.put32(cache::MAPPING_OFFSET, static_cast<uint32_t>(decoy));
        builder.put32(cache::MAPPING_COUNT, 1);

        const auto mappings = builder.append(make_mapping(SYNTHETIC_REGION_START, 0x8000, 5, 0));
        builder.append(make_mapping(SYNTHETIC_REGION_START + 0x8000, 0x8000, 3, 64));

        builder.put32(cache::MAPPING_WITH_SLIDE_OFFSET, static_cast<uint32_t>(mappings));
        builder.put32(cache::MAPPING_WITH_SLIDE_COUNT, 2);

        const auto subcaches = builder.append(cache::dyld_subcache_entry{
            .uuid = make_uuid(0xb1),
            .cache_vm_offset = 0x10000,
            .file_suffix = {'.', '0', '1'},
        });

        builder.append(cache::dyld_subcache_entry{
            .uuid = make_uuid(0xc2),
            .cache_vm_offset = 0x20000,
            .file_suffix = {'.', '0', '2', '.', 'd', 'y', 'l', 'd', 'd', 'a', 't', 'a'},
        });

        builder.put32(cache::SUBCACHE_ARRAY_OFFSET, static_cast<uint32_t>(subcaches));
        builder.put32(cache::SUBCACHE_ARRAY_COUNT, 2);

        const auto last_path = builder.append_string("/usr/lib/libz.dylib");
        const auto dyld_path = builder.append_string("/usr/lib/dyld");
        const auto first_path = builder.append_string("/usr/lib/liba.dylib");

        const auto images = builder.append(cache::dyld_cache_image_text_info{
            .uuid = make_uuid(0x31),
            .load_address = SYNTHETIC_REGION_START + 0x20000,
            .text_segment_size = 0x1000,
            .path_offset = static_cast<uint32_t>(last_path),
        });

        builder.append(cache::dyld_cache_image_text_info{
            .uuid = make_uuid(0x32),
            .load_address = SYNTHETIC_DYLD_ADDRESS,
            .text_segment_size = 0x2000,
            .path_offset = static_cast<uint32_t>(dyld_path),
        });

        builder.append(cache::dyld_cache_image_text_info{
            .uuid = make_uuid(0x33),
            .load_address = SYNTHETIC_REGION_START,
            .text_segment_size = 0x1000,
            .path_offset = static_cast<uint32_t>(first_path),
        });

        builder.put64(cache::IMAGES_TEXT_OFFSET, images);
        builder.put64(cache::IMAGES_TEXT_COUNT, 3);

        synthetic_cache synthetic{};
        synthetic.subcache_array_offset = subcaches;
        synthetic.image_text_offset = images;
        synthetic.files[synthetic.main_path] = builder.take();
        synthetic.files[synthetic.main_path.string() + ".01"] =
            build_subcache(make_uuid(0xb1), {make_mapping(SYNTHETIC_REGION_START + 0x10000, 0x4000, 5, 0)});
        synthetic.files[synthetic.main_path.string() + ".02.dylddata"] =
            build_subcache(make_uuid(0xc2), {make_mapping(SYNTHETIC_REGION_START + 0x20000, 0x4000, 5, 0),
                                             make_mapping(SYNTHETIC_REGION_START + 0x24000, 0x4000, 3, 128)});

        return synthetic;
    }

    class map_range_reader : public sogen::host_range_reader
    {
      public:
        explicit map_range_reader(std::map<std::filesystem::path, std::vector<std::byte>> files)
            : files_(std::move(files))
        {
        }

        uint64_t file_size(const std::string& path) override
        {
            const auto entry = this->files_.find(std::filesystem::path{path});
            return entry == this->files_.end() ? 0 : entry->second.size();
        }

        size_t read(const std::string& path, const uint64_t offset, const std::span<std::byte> destination) override
        {
            ++this->reads;
            this->requests.emplace_back(path, offset, destination.size());

            const auto entry = this->files_.find(std::filesystem::path{path});
            if (entry == this->files_.end() || destination.empty())
            {
                return 0;
            }

            const auto& data = entry->second;
            if (offset >= data.size())
            {
                return 0;
            }

            const auto available = std::min<uint64_t>(destination.size(), data.size() - offset);
            std::memcpy(destination.data(), data.data() + offset, static_cast<size_t>(available));
            this->bytes += available;

            return static_cast<size_t>(available);
        }

        uint64_t reads{};
        uint64_t bytes{};
        std::vector<std::tuple<std::string, uint64_t, size_t>> requests{};

      private:
        std::map<std::filesystem::path, std::vector<std::byte>> files_;
    };

    // A reader is allowed to come up short of what its own file_size promised: the browser bridge can
    // return a partial fetch, and a file can shrink under a size that was cached when it was opened.
    class short_reading_reader : public sogen::host_range_reader
    {
      public:
        short_reading_reader(uint64_t claimed_size, size_t deliver)
            : claimed_size_(claimed_size),
              deliver_(deliver)
        {
        }

        uint64_t file_size(const std::string&) override
        {
            return this->claimed_size_;
        }

        size_t read(const std::string&, uint64_t, const std::span<std::byte> destination) override
        {
            const auto count = std::min(this->deliver_, destination.size());
            std::memset(destination.data(), 0xCD, count);
            return count;
        }

      private:
        uint64_t claimed_size_{};
        size_t deliver_{};
    };

    // The buffer is sized from file_size before the read happens, so a source that does not shrink it
    // afterwards hands the parser a tail of zeroes and lets it believe them. That is the failure this
    // whole layer exists to avoid: the parser cannot tell absent bytes from real ones.
    TEST(DyldCacheBackingRanges, DerivesOneRangePerRegionAndAppliesTheSlide)
    {
        const auto synthetic = make_synthetic_cache();
        const auto reader = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());

        size_t expected_regions = 0;
        for (const auto& file : reader.files())
        {
            expected_regions += file.regions.size();
        }

        const auto unslid = sogen::build_dyld_cache_backing_ranges(reader, 0);
        ASSERT_EQ(unslid.size(), expected_regions);

        const auto slid = sogen::build_dyld_cache_backing_ranges(reader, 0x10000000ULL);
        ASSERT_EQ(slid.size(), unslid.size());

        for (size_t i = 0; i < unslid.size(); ++i)
        {
            EXPECT_EQ(slid[i].address, unslid[i].address + 0x10000000ULL);
            EXPECT_EQ(slid[i].size, unslid[i].size);
            EXPECT_EQ(slid[i].path, unslid[i].path);
            EXPECT_EQ(slid[i].file_offset, unslid[i].file_offset);
        }

        EXPECT_TRUE(std::ranges::is_sorted(unslid, {}, &sogen::dyld_cache_backing_range::address));
    }

    // A slide that would carry a region past the top of the address space has to drop it rather than wrap
    // it round to a low address, where it would collide with the executable.
    TEST(DyldCacheBackingRanges, DropsRegionsAnOverflowingSlideWouldWrap)
    {
        const auto synthetic = make_synthetic_cache();
        const auto reader = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());

        ASSERT_FALSE(sogen::build_dyld_cache_backing_ranges(reader, 0).empty());
        EXPECT_TRUE(sogen::build_dyld_cache_backing_ranges(reader, 0xFFFFFFFFFFFFFFFFULL).empty());
    }

    TEST(DyldCacheRangeSource, AReadShorterThanPromisedIsReportedAsShort)
    {
        short_reading_reader reader{4096, 12};

        const auto source = sogen::make_dyld_cache_range_source(reader, std::filesystem::path{"/claims-4096"});
        const auto data = source->read(0, 512);

        ASSERT_EQ(data.size(), 12u) << "the source reported bytes the reader never delivered";
        for (const auto byte : data)
        {
            EXPECT_EQ(static_cast<uint8_t>(byte), 0xCDu);
        }
    }

    TEST(DyldCacheRangeSource, AReaderThatDeliversNothingYieldsNothing)
    {
        short_reading_reader reader{4096, 0};

        const auto source = sogen::make_dyld_cache_range_source(reader, std::filesystem::path{"/claims-4096"});
        EXPECT_TRUE(source->read(0, 512).empty());
    }

    TEST(DyldCacheRangeSource, ParsesThroughTheRangeReader)
    {
        const auto synthetic = make_synthetic_cache();
        map_range_reader reader{synthetic.files};

        const auto reference = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());
        const auto ranged = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, sogen::make_host_range_cache_opener(reader));

        EXPECT_EQ(ranged.architecture(), reference.architecture());
        EXPECT_EQ(ranged.shared_region_start(), reference.shared_region_start());
        EXPECT_EQ(ranged.shared_region_size(), reference.shared_region_size());
        EXPECT_EQ(ranged.files().size(), reference.files().size());
        EXPECT_EQ(ranged.images().size(), reference.images().size());
    }

    // Not "reads fewer bytes than the file holds" -- on a 3.6 KB fixture the parser re-reads overlapping
    // headers and legitimately touches more bytes than the cache contains, so that assertion fails for a
    // reason that is not a defect. What has to hold is that parsing does not depend on how big the cache
    // is: the real one is 5.4 GB, and a parser whose reads scaled with it could never run in a browser
    // however the bytes were fetched.
    //
    // Compared on the requests issued rather than the bytes delivered. A short fixture truncates reads it
    // is too small to satisfy, so delivered bytes differ between the two even when the parser behaves
    // identically -- which is exactly what happens here.
    TEST(DyldCacheRangeSource, ParseCostDoesNotGrowWithCacheSize)
    {
        const auto small = make_synthetic_cache();
        map_range_reader small_reader{small.files};
        (void)sogen::dyld_shared_cache_reader::parse(small.main_path, sogen::make_host_range_cache_opener(small_reader));

        auto padded = make_synthetic_cache();
        uint64_t padded_total = 0;
        for (auto& [path, data] : padded.files)
        {
            data.resize(data.size() + 4 * 1024 * 1024);
            padded_total += data.size();
        }

        map_range_reader padded_reader{padded.files};
        const auto parsed = sogen::dyld_shared_cache_reader::parse(padded.main_path, sogen::make_host_range_cache_opener(padded_reader));

        EXPECT_EQ(parsed.files().size(), small.files.size()) << "padding must not change what was parsed";
        ASSERT_GT(small_reader.requests.size(), 0u);
        EXPECT_EQ(padded_reader.requests.size(), small_reader.requests.size()) << "a file 1000x larger changed how many reads it took";

        // Sizes are not compared one for one: a few reads ask for min(1024, bytes remaining), which the
        // short fixture satisfies with less. That clamp is bounded and fine. What would not be is a read
        // sized from the file itself, which is why the bound is asserted instead of the equality.
        size_t largest = 0;
        for (const auto& [path, offset, length] : padded_reader.requests)
        {
            largest = std::max(largest, length);
        }

        EXPECT_LE(largest, 4096u) << "a read sized from the file would ask for gigabytes of the real cache";
        EXPECT_LT(padded_reader.bytes, padded_total / 100) << "parsing is streaming the cache rather than seeking in it";
    }

    TEST(DyldCacheRangeSource, MissingFileYieldsAnEmptySource)
    {
        map_range_reader reader{{}};

        const auto source = sogen::make_dyld_cache_range_source(reader, std::filesystem::path{"/nope"});
        ASSERT_NE(source, nullptr);
        EXPECT_TRUE(source->read(0, 64).empty());
    }

    // A source that reported a full read after a short one would hand the parser uninitialised bytes and
    // let it believe them. The reader is allowed to come up short; the source has to pass that on.
    TEST(DyldCacheRangeSource, ShortReadsAreReportedAsShort)
    {
        const auto synthetic = make_synthetic_cache();
        map_range_reader reader{synthetic.files};

        const auto size = reader.file_size(synthetic.main_path.string());
        ASSERT_GT(size, 32ULL);

        const auto source = sogen::make_dyld_cache_range_source(reader, synthetic.main_path);

        EXPECT_EQ(source->read(size - 8, 64).size(), 8ULL);
        EXPECT_TRUE(source->read(size, 64).empty());
        EXPECT_TRUE(source->read(size + 4096, 64).empty());
    }

    TEST(DyldSharedCacheReader, RejectsANonCacheFile)
    {
        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::open(sogen::test::fixture_path("macho_static_arm64")), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsAMissingFile)
    {
        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::open(sogen::test::fixture_path("does_not_exist")), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, ReadsHeaderFieldsOfASyntheticCache)
    {
        const auto synthetic = make_synthetic_cache();
        const auto reader = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());

        EXPECT_EQ(reader.architecture(), "arm64e");
        EXPECT_EQ(reader.platform(), sogen::macho::PLATFORM_MACOS);
        EXPECT_EQ(reader.shared_region_start(), SYNTHETIC_REGION_START);
        EXPECT_EQ(reader.shared_region_size(), 0x40000000ULL);
        EXPECT_EQ(reader.max_slide(), 0x10000000ULL);
        EXPECT_EQ(reader.dyld_in_cache_address(), SYNTHETIC_DYLD_ADDRESS);
        EXPECT_FALSE(reader.dylibs_expected_on_disk());
    }

    TEST(DyldSharedCacheReader, ReportsDylibsExpectedOnDisk)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.patch32(synthetic.main_path, cache::FORMAT_FLAGS, cache::FORMAT_FLAG_DYLIBS_EXPECTED_ON_DISK);

        const auto reader = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());
        EXPECT_TRUE(reader.dylibs_expected_on_disk());
    }

    TEST(DyldSharedCacheReader, ListsTheMainFileAndEverySubcache)
    {
        const auto synthetic = make_synthetic_cache();
        const auto reader = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());

        ASSERT_EQ(reader.files().size(), 3u);

        EXPECT_EQ(reader.files()[0].path, synthetic.main_path);
        EXPECT_EQ(reader.files()[0].vm_offset, 0u);
        EXPECT_EQ(reader.files()[0].uuid, make_uuid(0xa0));

        EXPECT_EQ(reader.files()[1].path.string(), synthetic.main_path.string() + ".01");
        EXPECT_EQ(reader.files()[1].vm_offset, 0x10000u);
        EXPECT_EQ(reader.files()[1].uuid, make_uuid(0xb1));

        EXPECT_EQ(reader.files()[2].path.string(), synthetic.main_path.string() + ".02.dylddata");
        EXPECT_EQ(reader.files()[2].vm_offset, 0x20000u);
        EXPECT_EQ(reader.files()[2].uuid, make_uuid(0xc2));
    }

    TEST(DyldSharedCacheReader, ReadsRegionsFromTheMappingsWithSlideArray)
    {
        const auto synthetic = make_synthetic_cache();
        const auto reader = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());

        ASSERT_EQ(reader.files().size(), 3u);
        ASSERT_EQ(reader.files()[0].regions.size(), 2u);
        ASSERT_EQ(reader.files()[1].regions.size(), 1u);
        ASSERT_EQ(reader.files()[2].regions.size(), 2u);

        const auto& text = reader.files()[0].regions[0];
        EXPECT_EQ(text.address, SYNTHETIC_REGION_START);
        EXPECT_EQ(text.size, 0x8000u);
        EXPECT_EQ(text.file_offset, 0x4000u);
        EXPECT_EQ(text.flags, 0u);
        EXPECT_EQ(text.initial_permissions, sogen::memory_permission::read_exec);
        EXPECT_EQ(text.max_permissions, sogen::memory_permission::read_exec);
        EXPECT_FALSE(text.has_slide_info);

        const auto& data = reader.files()[0].regions[1];
        EXPECT_EQ(data.address, SYNTHETIC_REGION_START + 0x8000);
        EXPECT_EQ(data.flags, cache::MAPPING_FLAG_AUTH_DATA);
        EXPECT_EQ(data.initial_permissions, sogen::memory_permission::read_write);
        EXPECT_TRUE(data.has_slide_info);

        EXPECT_FALSE(reader.files()[2].regions[0].has_slide_info);
        EXPECT_TRUE(reader.files()[2].regions[1].has_slide_info);
    }

    TEST(DyldSharedCacheReader, SortsImagesByAddressAndResolvesThem)
    {
        const auto synthetic = make_synthetic_cache();
        const auto reader = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());

        ASSERT_EQ(reader.images().size(), 3u);
        EXPECT_EQ(reader.images()[0].path, "/usr/lib/liba.dylib");
        EXPECT_EQ(reader.images()[1].path, "/usr/lib/dyld");
        EXPECT_EQ(reader.images()[2].path, "/usr/lib/libz.dylib");
        EXPECT_EQ(reader.images()[1].address, SYNTHETIC_DYLD_ADDRESS);
        EXPECT_EQ(reader.images()[1].text_size, 0x2000u);

        const auto* dyld = reader.find_image_by_address(SYNTHETIC_DYLD_ADDRESS);
        ASSERT_NE(dyld, nullptr);
        EXPECT_EQ(dyld->path, "/usr/lib/dyld");

        const auto* inside = reader.find_image_by_address(SYNTHETIC_DYLD_ADDRESS + 0x1fff);
        ASSERT_NE(inside, nullptr);
        EXPECT_EQ(inside->path, "/usr/lib/dyld");

        EXPECT_EQ(reader.find_image_by_address(SYNTHETIC_REGION_START - 1), nullptr);
        EXPECT_EQ(reader.find_image_by_address(SYNTHETIC_REGION_START + 0x1000), nullptr);
        EXPECT_EQ(reader.find_image_by_address(SYNTHETIC_DYLD_ADDRESS + 0x2000), nullptr);
        EXPECT_EQ(reader.find_image_by_address(0xffffffffffffffffULL), nullptr);
    }

    TEST(DyldSharedCacheReader, RejectsASubcacheWithAMismatchedUuid)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.files[synthetic.main_path.string() + ".01"] =
            build_subcache(make_uuid(0xee), {make_mapping(SYNTHETIC_REGION_START + 0x10000, 0x4000, 5, 0)});

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsAMissingSubcacheFile)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.files.erase(synthetic.main_path.string() + ".02.dylddata");

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsASubcacheWithoutCacheMagic)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.patch32(synthetic.main_path.string() + ".01", cache::MAGIC, 0x41414141);

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsASubcacheEntryWithoutASuffix)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.patch32(synthetic.main_path, synthetic.subcache_array_offset + 24, 0);

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsATruncatedMappingArray)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.patch32(synthetic.main_path, cache::MAPPING_WITH_SLIDE_COUNT, 8);

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsAMappingArrayOutsideTheFile)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.patch32(synthetic.main_path, cache::MAPPING_WITH_SLIDE_OFFSET, 0xfffff000);

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsAbsurdCountsWithoutExhaustingMemory)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.patch32(synthetic.main_path, cache::SUBCACHE_ARRAY_COUNT, 0xffffffff);
        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);

        auto images = make_synthetic_cache();
        images.patch64(images.main_path, cache::IMAGES_TEXT_COUNT, 0xffffffffffffffffULL);
        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(images.main_path, images.opener()), std::runtime_error);

        auto mappings = make_synthetic_cache();
        mappings.patch32(mappings.main_path, cache::MAPPING_WITH_SLIDE_COUNT, 0xffffffff);
        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(mappings.main_path, mappings.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsAnImagePathOutsideTheFile)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.patch32(synthetic.main_path, synthetic.image_text_offset + 28, 0xfffff000);

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, RejectsAnUnterminatedImagePath)
    {
        auto synthetic = make_synthetic_cache();
        auto& data = synthetic.files.at(synthetic.main_path);

        const auto unterminated = static_cast<uint32_t>(data.size());
        data.resize(data.size() + 8, std::byte{0x41});
        synthetic.patch32(synthetic.main_path, synthetic.image_text_offset + 28, unterminated);

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener()), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, AcceptsACacheWithoutSubcachesOrImages)
    {
        auto synthetic = make_synthetic_cache();
        synthetic.patch32(synthetic.main_path, cache::SUBCACHE_ARRAY_COUNT, 0);
        synthetic.patch64(synthetic.main_path, cache::IMAGES_TEXT_COUNT, 0);

        const auto reader = sogen::dyld_shared_cache_reader::parse(synthetic.main_path, synthetic.opener());

        ASSERT_EQ(reader.files().size(), 1u);
        EXPECT_EQ(reader.files()[0].regions.size(), 2u);
        EXPECT_TRUE(reader.images().empty());
        EXPECT_EQ(reader.find_image_by_address(SYNTHETIC_DYLD_ADDRESS), nullptr);
    }

    TEST(DyldSharedCacheReader, ReadsTheRealHostCache)
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(path))
        {
            GTEST_SKIP() << "no dyld shared cache on this host";
        }

        const auto reader = sogen::dyld_shared_cache_reader::open(path);

        EXPECT_EQ(reader.architecture(), "arm64e");
        EXPECT_EQ(reader.platform(), sogen::macho::PLATFORM_MACOS);
        EXPECT_EQ(reader.shared_region_start(), 0x180000000ULL);
        EXPECT_EQ(reader.shared_region_size(), 0x166444000ULL);
        EXPECT_EQ(reader.max_slide(), 0x10000000ULL);
        EXPECT_FALSE(reader.dylibs_expected_on_disk());
        EXPECT_EQ(reader.dyld_in_cache_address(), 0x180114000ULL);

        ASSERT_EQ(reader.files().size(), 13u);
        EXPECT_EQ(reader.files()[0].vm_offset, 0u);
        EXPECT_EQ(reader.files()[1].vm_offset, 0x88000ULL);

        size_t region_count = 0;
        for (const auto& file : reader.files())
        {
            EXPECT_FALSE(file.regions.empty()) << file.path.string();
            region_count += file.regions.size();

            for (const auto& region : file.regions)
            {
                EXPECT_GE(region.address, 0x180000000ULL);
                EXPECT_LE(region.address + region.size, 0x2e6444000ULL);
            }
        }

        EXPECT_EQ(region_count, 34u);

        ASSERT_EQ(reader.images().size(), 3649u);

        const auto* dyld = reader.find_image_by_address(0x180114000ULL);
        ASSERT_NE(dyld, nullptr);
        EXPECT_EQ(dyld->path, "/usr/lib/dyld");

        EXPECT_EQ(reader.find_image_by_address(0x100000000ULL), nullptr);
    }

    TEST(DyldSharedCacheReader, MakesNoStructuralAssumptionsAboutSubcaches)
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(path))
        {
            GTEST_SKIP() << "no dyld shared cache on this host";
        }

        const auto reader = sogen::dyld_shared_cache_reader::open(path);

        size_t single_region_files = 0;
        size_t files_without_slide_info = 0;

        for (const auto& file : reader.files())
        {
            if (file.regions.size() == 1)
            {
                ++single_region_files;
            }

            const auto has_slide = std::ranges::any_of(file.regions, //
                                                       [](const sogen::dyld_cache_region& region) { return region.has_slide_info; });
            if (!has_slide)
            {
                ++files_without_slide_info;
            }
        }

        EXPECT_GT(single_region_files, 0u) << "at least one subcache is a single r-x mapping";
        EXPECT_EQ(files_without_slide_info, 10u) << "only .02, .06 and .10 carry slide info";
    }

    TEST(DyldSharedCacheReader, TouchesOnlyMetadataOfTheRealCache)
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::exists(path))
        {
            GTEST_SKIP() << "no dyld shared cache on this host";
        }

        class counting_source : public sogen::dyld_cache_data_source
        {
          public:
            counting_source(std::unique_ptr<sogen::dyld_cache_data_source> source, uint64_t& total)
                : source_(std::move(source)),
                  total_(&total)
            {
            }

            std::vector<std::byte> read(const uint64_t offset, const uint64_t size) override
            {
                auto data = this->source_->read(offset, size);
                *this->total_ += data.size();
                return data;
            }

          private:
            std::unique_ptr<sogen::dyld_cache_data_source> source_{};
            uint64_t* total_{};
        };

        uint64_t total_bytes = 0;

        const auto reader = sogen::dyld_shared_cache_reader::parse(
            path, [&total_bytes](const std::filesystem::path& file) -> std::unique_ptr<sogen::dyld_cache_data_source> {
                auto source = sogen::open_dyld_cache_file(file);
                if (!source)
                {
                    return nullptr;
                }

                return std::make_unique<counting_source>(std::move(source), total_bytes);
            });

        EXPECT_EQ(reader.files().size(), 13u);
        EXPECT_LT(total_bytes, 8u * 1024 * 1024) << "the reader is faulting in far more than cache metadata";
    }

    std::filesystem::path write_synthetic_cache(const synthetic_cache& synthetic, const std::filesystem::path& directory)
    {
        for (const auto& [path, data] : synthetic.files)
        {
            if (!sogen::utils::io::write_file(directory / path.filename(), data))
            {
                throw std::runtime_error("Failed to write synthetic cache file: " + path.filename().string());
            }
        }

        return directory / synthetic.main_path.filename();
    }

    TEST(DyldSharedCacheReader, ReadsASyntheticCacheFromRealFiles)
    {
        const sogen::test::temp_directory directory{"dyld-cache-from-files"};

        const auto synthetic = make_synthetic_cache();
        const auto main_path = write_synthetic_cache(synthetic, directory.path());

        const auto reader = sogen::dyld_shared_cache_reader::open(main_path);

        EXPECT_EQ(reader.architecture(), "arm64e");
        EXPECT_EQ(reader.platform(), sogen::macho::PLATFORM_MACOS);
        EXPECT_EQ(reader.shared_region_start(), SYNTHETIC_REGION_START);
        EXPECT_EQ(reader.dyld_in_cache_address(), SYNTHETIC_DYLD_ADDRESS);

        ASSERT_EQ(reader.files().size(), 3u);
        EXPECT_EQ(reader.files()[0].path, main_path);
        EXPECT_EQ(reader.files()[1].path, directory.path() / (main_path.filename().string() + ".01"));
        EXPECT_EQ(reader.files()[2].path, directory.path() / (main_path.filename().string() + ".02.dylddata"));
        EXPECT_EQ(reader.files()[1].vm_offset, 0x10000u);
        EXPECT_EQ(reader.files()[2].vm_offset, 0x20000u);
        EXPECT_EQ(reader.files()[0].uuid, make_uuid(0xa0));
        EXPECT_EQ(reader.files()[2].uuid, make_uuid(0xc2));

        ASSERT_EQ(reader.files()[0].regions.size(), 2u);
        ASSERT_EQ(reader.files()[1].regions.size(), 1u);
        ASSERT_EQ(reader.files()[2].regions.size(), 2u);
        EXPECT_EQ(reader.files()[0].regions[0].address, SYNTHETIC_REGION_START);
        EXPECT_EQ(reader.files()[0].regions[0].initial_permissions, sogen::memory_permission::read_exec);
        EXPECT_FALSE(reader.files()[0].regions[0].has_slide_info);
        EXPECT_TRUE(reader.files()[0].regions[1].has_slide_info);
        EXPECT_TRUE(reader.files()[2].regions[1].has_slide_info);

        ASSERT_EQ(reader.images().size(), 3u);
        EXPECT_EQ(reader.images()[0].path, "/usr/lib/liba.dylib");
        EXPECT_EQ(reader.images()[1].path, "/usr/lib/dyld");
        EXPECT_EQ(reader.images()[2].path, "/usr/lib/libz.dylib");

        const auto* dyld = reader.find_image_by_address(SYNTHETIC_DYLD_ADDRESS + 0x1fff);
        ASSERT_NE(dyld, nullptr);
        EXPECT_EQ(dyld->path, "/usr/lib/dyld");
        EXPECT_EQ(reader.find_image_by_address(SYNTHETIC_REGION_START + 0x1000), nullptr);
    }

    TEST(DyldSharedCacheReader, RejectsATruncatedCacheFileOnDisk)
    {
        const sogen::test::temp_directory directory{"dyld-cache-truncated"};

        auto synthetic = make_synthetic_cache();
        auto& data = synthetic.files.at(synthetic.main_path);
        data.resize(0x100);

        const auto main_path = write_synthetic_cache(synthetic, directory.path());

        EXPECT_THROW((void)sogen::dyld_shared_cache_reader::open(main_path), std::runtime_error);
    }

    TEST(DyldSharedCacheReader, ClampsRangedFileReadsToTheFileContents)
    {
        const sogen::test::temp_directory directory{"dyld-cache-ranged-reads"};
        const auto path = directory.path() / "ranged.bin";

        std::vector<std::byte> content(64);
        for (size_t i = 0; i < content.size(); ++i)
        {
            content[i] = static_cast<std::byte>(i);
        }

        ASSERT_TRUE(sogen::utils::io::write_file(path, content));

        const auto source = sogen::open_dyld_cache_file(path);
        ASSERT_NE(source, nullptr);

        const std::vector<std::byte> tail{content.end() - 16, content.end()};

        EXPECT_EQ(source->read(0, content.size()), content);
        EXPECT_EQ(source->read(48, 16), tail) << "a read ending exactly at the end of the file";
        EXPECT_EQ(source->read(48, 4096), tail) << "a read running past the end must clamp to what exists";
        EXPECT_EQ(source->read(0, 1ULL << 40), content) << "a terabyte request must neither allocate nor over-report";

        EXPECT_TRUE(source->read(content.size(), 16).empty()) << "an offset exactly at the end holds no bytes";
        EXPECT_TRUE(source->read(1ULL << 40, 16).empty());
        EXPECT_TRUE(source->read(0, 0).empty());

        const std::vector<std::byte> middle{content.begin() + 16, content.begin() + 24};
        EXPECT_EQ(source->read(16, 8), middle) << "the source stays usable after out-of-range reads";

        ASSERT_TRUE(sogen::utils::io::write_file(directory.path() / "empty.bin", {}));
        EXPECT_EQ(sogen::open_dyld_cache_file(directory.path() / "empty.bin"), nullptr);
        EXPECT_EQ(sogen::open_dyld_cache_file(directory.path()), nullptr);
    }
}

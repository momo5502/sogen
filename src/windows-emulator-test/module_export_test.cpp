#include <std_include.hpp>
#include <module/module_mapping.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr size_t k_page_size = 0x1000;

        class page_memory final : public memory_interface
        {
          public:
            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!this->try_read_memory(address, data, size))
                {
                    throw std::runtime_error{"unmapped guest read"};
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                auto* destination = static_cast<std::byte*>(data);

                for (size_t i = 0; i < size; ++i)
                {
                    const auto page = this->pages_.find(page_base(address + i));
                    if (page == this->pages_.end())
                    {
                        return false;
                    }

                    destination[i] = page->second[page_offset(address + i)];
                }

                return true;
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!this->try_write_memory(address, data, size))
                {
                    throw std::runtime_error{"unmapped guest write"};
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                const auto* source = static_cast<const std::byte*>(data);

                for (size_t i = 0; i < size; ++i)
                {
                    const auto page = this->pages_.find(page_base(address + i));
                    if (page == this->pages_.end())
                    {
                        return false;
                    }

                    page->second[page_offset(address + i)] = source[i];
                }

                return true;
            }

          private:
            static uint64_t page_base(const uint64_t address)
            {
                return address & ~static_cast<uint64_t>(k_page_size - 1);
            }

            static size_t page_offset(const uint64_t address)
            {
                return static_cast<size_t>(address & (k_page_size - 1));
            }

            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
            }

            void map_memory(const uint64_t address, const size_t size, memory_permission) override
            {
                for (uint64_t offset = 0; offset < size; offset += k_page_size)
                {
                    this->pages_.try_emplace(page_base(address + offset));
                }
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                for (uint64_t offset = 0; offset < size; offset += k_page_size)
                {
                    this->pages_.erase(page_base(address + offset));
                }
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
            }

            std::map<uint64_t, std::array<std::byte, k_page_size>> pages_{};
        };

        constexpr uint64_t k_image_base = 0x180000000ULL;
        constexpr uint32_t k_size_of_headers = 0x1000;
        constexpr uint32_t k_section_rva = 0x1000;
        constexpr uint32_t k_section_size = 0x1000;
        // The image is a page larger than its only section, so [0x2000, 0x3000) is reserved but never
        // committed - the gap an in-bounds RVA can fall into.
        constexpr uint32_t k_size_of_image = 0x3000;
        constexpr uint32_t k_gap_rva = 0x2100;

        constexpr uint32_t k_export_dir_rva = 0x1000;
        constexpr uint32_t k_functions_rva = 0x1100;
        constexpr uint32_t k_names_rva = 0x1120;
        constexpr uint32_t k_ordinals_rva = 0x1140;
        constexpr uint32_t k_alpha_name_rva = 0x1160;
        constexpr uint32_t k_beta_name_rva = 0x1170;
        constexpr uint32_t k_unterminated_name_rva = k_size_of_image - 8;

        IMAGE_EXPORT_DIRECTORY default_export_directory()
        {
            IMAGE_EXPORT_DIRECTORY directory{};
            directory.Base = 1;
            directory.NumberOfFunctions = 2;
            directory.NumberOfNames = 2;
            directory.AddressOfFunctions = k_functions_rva;
            directory.AddressOfNames = k_names_rva;
            directory.AddressOfNameOrdinals = k_ordinals_rva;
            return directory;
        }

        template <typename T>
        void poke(std::vector<std::byte>& image, const uint64_t offset, const T& value)
        {
            std::memcpy(image.data() + offset, &value, sizeof(value));
        }

        struct image_options
        {
            IMAGE_EXPORT_DIRECTORY export_directory = default_export_directory();
            std::array<uint32_t, 2> name_rvas{k_alpha_name_rva, k_beta_name_rva};
            std::array<uint16_t, 2> ordinals{0, 1};
        };

        // A minimal PE64 whose file offsets equal their RVAs, so the same bytes serve both the file-backed
        // and the already-mapped code paths.
        std::vector<std::byte> build_image(const image_options& options)
        {
            std::vector<std::byte> image(k_size_of_image, std::byte{});

            constexpr uint32_t nt_offset = 0x80;
            using nt_headers_t = PENTHeaders_t<std::uint64_t>;

            nt_headers_t nt_headers{};
            nt_headers.Signature = nt_headers_t::k_Signature;
            nt_headers.FileHeader.Machine = PEMachineType::AMD64;
            nt_headers.FileHeader.NumberOfSections = 1;
            nt_headers.FileHeader.SizeOfOptionalHeader = sizeof(PEOptionalHeader_t<std::uint64_t>);
            nt_headers.FileHeader.Characteristics = IMAGE_FILE_DLL;

            auto& optional_header = nt_headers.OptionalHeader;
            optional_header.Magic = PEOptionalHeader_t<std::uint64_t>::k_Magic;
            optional_header.AddressOfEntryPoint = k_section_rva;
            optional_header.ImageBase = k_image_base;
            optional_header.SectionAlignment = k_page_size;
            optional_header.FileAlignment = k_page_size;
            optional_header.SizeOfImage = k_size_of_image;
            optional_header.SizeOfHeaders = k_size_of_headers;
            optional_header.NumberOfRvaAndSizes = PEOptionalHeaderBasePart1_t<std::uint64_t>::k_NumberOfDataDirectors;
            optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {.VirtualAddress = k_export_dir_rva, .Size = 0x100};

            PEDosHeader_t dos_header{};
            dos_header.e_magic = PEDosHeader_t::k_Magic;
            dos_header.e_lfanew = nt_offset;

            IMAGE_SECTION_HEADER section{};
            std::memcpy(section.Name, ".text", 5);
            section.Misc.VirtualSize = k_section_size;
            section.VirtualAddress = k_section_rva;
            section.SizeOfRawData = k_section_size;
            section.PointerToRawData = k_section_rva;
            section.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;

            const auto optional_header_offset = static_cast<uint64_t>(reinterpret_cast<const std::byte*>(&optional_header) -
                                                                      reinterpret_cast<const std::byte*>(&nt_headers));

            poke(image, 0, dos_header);
            poke(image, nt_offset, nt_headers);
            poke(image, nt_offset + optional_header_offset + nt_headers.FileHeader.SizeOfOptionalHeader, section);

            poke(image, k_export_dir_rva, options.export_directory);
            poke(image, k_functions_rva, std::array<uint32_t, 2>{k_section_rva, k_section_rva + 4});
            poke(image, k_names_rva, options.name_rvas);
            poke(image, k_ordinals_rva, options.ordinals);
            std::memcpy(image.data() + k_alpha_name_rva, "alpha", 6);
            std::memcpy(image.data() + k_beta_name_rva, "beta", 5);
            std::memset(image.data() + k_unterminated_name_rva, 'A', k_size_of_image - k_unterminated_name_rva);

            return image;
        }

        mapped_module map_from_file_image(memory_manager& memory, const std::vector<std::byte>& image)
        {
            const std::span file_data{image.data(), k_size_of_headers + k_section_size};
            return map_module_from_data<std::uint64_t>(memory, file_data, "test.dll", windows_path{"C:\\test.dll"});
        }

        mapped_module map_from_mapped_image(memory_manager& memory, const std::vector<std::byte>& image)
        {
            const auto base = memory.allocate_memory(k_image_base, k_size_of_image, memory_permission::read_write);
            EXPECT_TRUE(base);
            memory.write_memory(k_image_base, image.data(), image.size());

            return map_module_from_memory<std::uint64_t>(memory, k_image_base, k_size_of_image, windows_path{"C:\\test.dll"});
        }

        std::vector<std::string> export_names(const mapped_module& binary)
        {
            std::vector<std::string> names{};
            for (const auto& symbol : binary.exports)
            {
                names.push_back(symbol.name);
            }
            return names;
        }
    }

    class ModuleExportTest : public testing::TestWithParam<bool>
    {
      protected:
        mapped_module map(const image_options& options)
        {
            const auto image = build_image(options);
            return GetParam() ? map_from_file_image(this->memory_, image) : map_from_mapped_image(this->memory_, image);
        }

      private:
        page_memory backend_{};
        memory_manager memory_{backend_};
    };

    INSTANTIATE_TEST_SUITE_P(MappingPath, ModuleExportTest, testing::Values(false, true),
                             [](const testing::TestParamInfo<bool>& info) { return info.param ? "FromData" : "FromMemory"; });

    TEST_P(ModuleExportTest, ResolvesWellFormedExports)
    {
        const auto binary = this->map({});

        ASSERT_EQ(export_names(binary), (std::vector<std::string>{"alpha", "beta"}));
        ASSERT_EQ(binary.exports.size(), 2u);
        EXPECT_EQ(binary.exports[0].ordinal, 1u);
        EXPECT_EQ(binary.exports[0].rva, k_section_rva);
        EXPECT_EQ(binary.exports[0].address, binary.image_base + k_section_rva);
        EXPECT_EQ(binary.exports[1].ordinal, 2u);
        EXPECT_EQ(binary.exports[1].rva, k_section_rva + 4);
    }

    TEST_P(ModuleExportTest, MapsImageWhoseExportDirectoryIsGarbage)
    {
        image_options options{};
        options.export_directory.NumberOfNames = 0xCD1A9E9B;
        options.export_directory.NumberOfFunctions = 0x4E1B2C3D;
        options.export_directory.AddressOfFunctions = 0x10DC3527;
        options.export_directory.AddressOfNames = 0x7F2A1100;
        options.export_directory.AddressOfNameOrdinals = 0xA3B40080;

        const auto binary = this->map(options);

        EXPECT_TRUE(binary.exports.empty());
        EXPECT_EQ(binary.sections.size(), 1u);
    }

    TEST_P(ModuleExportTest, MapsImageWhoseExportDirectoryItselfIsOutOfBounds)
    {
        auto image = build_image({});
        auto nt_headers = PENTHeaders_t<std::uint64_t>{};
        std::memcpy(&nt_headers, image.data() + 0x80, sizeof(nt_headers));
        nt_headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {.VirtualAddress = k_size_of_image - 4, .Size = 0x100};
        poke(image, 0x80, nt_headers);

        page_memory backend{};
        memory_manager memory{backend};
        const auto binary = GetParam() ? map_from_file_image(memory, image) : map_from_mapped_image(memory, image);

        EXPECT_TRUE(binary.exports.empty());
    }

    TEST_P(ModuleExportTest, SkipsTruncatedFunctionTable)
    {
        image_options options{};
        options.export_directory.NumberOfFunctions = 0x1000;
        options.export_directory.AddressOfFunctions = k_size_of_image - 8;

        const auto binary = this->map(options);

        EXPECT_TRUE(binary.exports.empty());
    }

    TEST_P(ModuleExportTest, DropsTableWithOrdinalPastTheFunctionTable)
    {
        image_options options{};
        options.ordinals = {0, 0x4242};

        const auto binary = this->map(options);

        EXPECT_TRUE(binary.exports.empty());
    }

    TEST_P(ModuleExportTest, DropsTableWithNameRvaOutsideTheImage)
    {
        image_options options{};
        options.name_rvas = {k_alpha_name_rva, k_size_of_image + 0x100};

        const auto binary = this->map(options);

        EXPECT_TRUE(binary.exports.empty());
    }

    TEST_P(ModuleExportTest, DropsTableWithUnterminatedName)
    {
        image_options options{};
        options.name_rvas = {k_alpha_name_rva, k_unterminated_name_rva};

        const auto binary = this->map(options);

        EXPECT_TRUE(binary.exports.empty());
    }

    TEST(ModuleExportGapTest, DropsTableWithArraysInAnUncommittedGap)
    {
        image_options options{};
        options.export_directory.AddressOfNames = k_gap_rva;

        page_memory backend{};
        memory_manager memory{backend};
        const auto binary = map_from_file_image(memory, build_image(options));

        EXPECT_TRUE(binary.exports.empty());
        EXPECT_EQ(binary.sections.size(), 1u);
    }

    TEST(ModuleExportGapTest, DropsTableWithNameInAnUncommittedGap)
    {
        image_options options{};
        options.name_rvas = {k_alpha_name_rva, k_gap_rva};

        page_memory backend{};
        memory_manager memory{backend};
        const auto binary = map_from_file_image(memory, build_image(options));

        EXPECT_TRUE(binary.exports.empty());
    }
} // namespace sogen::test

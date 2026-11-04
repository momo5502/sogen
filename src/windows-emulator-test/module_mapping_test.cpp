#include <std_include.hpp>
#include <module/module_mapping.hpp>

#include "page_memory.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t k_image_base = 0x180000000ULL;
        constexpr uint32_t k_page = 0x1000;
        constexpr uint32_t k_nt_offset = 0x40;
        constexpr std::byte k_text_byte{0xC3};
        constexpr std::byte k_data_byte{0x5A};
        constexpr std::byte k_neighbour_byte{0xEE};

        constexpr uint32_t k_code = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        constexpr uint32_t k_rdata = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        constexpr uint32_t k_data = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

        struct section_layout
        {
            uint32_t rva{};
            uint32_t virtual_size{};
            uint32_t raw_offset{};
            uint32_t raw_size{};
            uint32_t characteristics{};
            std::byte fill{};
        };

        struct image_layout
        {
            uint32_t section_alignment{};
            uint32_t file_alignment{};
            uint32_t size_of_image{};
            uint32_t size_of_headers{};
            std::vector<section_layout> sections{};
        };

        template <typename T>
        void poke(std::vector<std::byte>& image, const size_t offset, const T& value)
        {
            std::memcpy(image.data() + offset, &value, sizeof(value));
        }

        std::vector<std::byte> build_image(const image_layout& layout)
        {
            const auto size_of_headers = layout.size_of_headers ? layout.size_of_headers : layout.file_alignment;
            size_t file_size = size_of_headers;
            for (const auto& section : layout.sections)
            {
                file_size = std::max<size_t>(file_size, section.raw_offset + section.raw_size);
            }

            std::vector<std::byte> image(file_size, std::byte{});

            PEDosHeader_t dos_header{};
            dos_header.e_magic = PEDosHeader_t::k_Magic;
            dos_header.e_lfanew = k_nt_offset;

            using nt_headers_t = PENTHeaders_t<std::uint64_t>;
            nt_headers_t nt_headers{};
            nt_headers.Signature = nt_headers_t::k_Signature;
            nt_headers.FileHeader.Machine = PEMachineType::AMD64;
            nt_headers.FileHeader.NumberOfSections = static_cast<uint16_t>(layout.sections.size());
            nt_headers.FileHeader.SizeOfOptionalHeader = sizeof(PEOptionalHeader_t<std::uint64_t>);
            nt_headers.FileHeader.Characteristics = IMAGE_FILE_DLL;

            auto& optional_header = nt_headers.OptionalHeader;
            optional_header.Magic = PEOptionalHeader_t<std::uint64_t>::k_Magic;
            optional_header.ImageBase = k_image_base;
            optional_header.SectionAlignment = layout.section_alignment;
            optional_header.FileAlignment = layout.file_alignment;
            optional_header.SizeOfImage = layout.size_of_image;
            optional_header.SizeOfHeaders = size_of_headers;
            optional_header.NumberOfRvaAndSizes = PEOptionalHeaderBasePart1_t<std::uint64_t>::k_NumberOfDataDirectors;

            poke(image, 0, dos_header);
            poke(image, k_nt_offset, nt_headers);

            auto section_offset =
                k_nt_offset + sizeof(nt_headers.Signature) + sizeof(nt_headers.FileHeader) + nt_headers.FileHeader.SizeOfOptionalHeader;
            for (const auto& layout_section : layout.sections)
            {
                IMAGE_SECTION_HEADER section{};
                section.Misc.VirtualSize = layout_section.virtual_size;
                section.VirtualAddress = layout_section.rva;
                section.SizeOfRawData = layout_section.raw_size;
                section.PointerToRawData = layout_section.raw_offset;
                section.Characteristics = layout_section.characteristics;
                poke(image, section_offset, section);
                section_offset += sizeof(section);

                std::memset(image.data() + layout_section.raw_offset, std::to_integer<int>(layout_section.fill), layout_section.raw_size);
            }

            return image;
        }

        class ModuleMappingTest : public testing::Test
        {
          protected:
            mapped_module map(const image_layout& layout)
            {
                this->file_ = build_image(layout);
                return map_module_from_data<std::uint64_t>(this->memory_, this->file_, "test.dll", windows_path{"C:\\test.dll"});
            }

            void map_neighbour(const uint64_t rva)
            {
                const auto address = k_image_base + rva;
                ASSERT_TRUE(
                    this->memory_.allocate_memory(address, k_page, memory_permission::read_write, true, memory_region_kind::section_image));
                ASSERT_TRUE(this->memory_.commit_image_memory(address, k_page, memory_permission::read_write));

                const std::vector<std::byte> fill(k_page, k_neighbour_byte);
                this->memory_.write_memory(address, fill.data(), fill.size());
            }

            region_info region(const uint64_t rva)
            {
                return this->memory_.get_region_info(k_image_base + rva);
            }

            std::vector<std::byte> read(const uint64_t rva, const size_t size)
            {
                std::vector<std::byte> bytes(size);
                this->memory_.read_memory(k_image_base + rva, bytes.data(), bytes.size());
                return bytes;
            }

            std::vector<std::byte> file_bytes(const size_t offset, const size_t size) const
            {
                return {this->file_.begin() + static_cast<ptrdiff_t>(offset), this->file_.begin() + static_cast<ptrdiff_t>(offset + size)};
            }

          private:
            page_memory backend_{};
            memory_manager memory_{backend_};
            std::vector<std::byte> file_{};
        };

        std::vector<std::byte> repeated(const std::byte value, const size_t count)
        {
            return std::vector<std::byte>(count, value);
        }

        image_layout zero_virtual_size_layout(const uint32_t alignment)
        {
            return {
                .section_alignment = alignment,
                .file_alignment = 0x200,
                .size_of_image = 3 * alignment,
                .sections =
                    {
                        {.rva = alignment,
                         .virtual_size = 0x10,
                         .raw_offset = 0x200,
                         .raw_size = 0x200,
                         .characteristics = k_code,
                         .fill = k_text_byte},
                        {.rva = 2 * alignment,
                         .virtual_size = 0,
                         .raw_offset = 0x400,
                         .raw_size = 0x400,
                         .characteristics = k_data,
                         .fill = k_data_byte},
                    },
            };
        }
    }

    TEST_F(ModuleMappingTest, SectionWithoutVirtualSizeIsMappedByItsRawSize)
    {
        const auto binary = this->map(zero_virtual_size_layout(k_page));
        ASSERT_EQ(binary.image_base, k_image_base);

        const auto data = this->region(2 * k_page);
        EXPECT_TRUE(data.is_committed);
        EXPECT_EQ(data.permissions.common, memory_permission::read_write);
        EXPECT_EQ(this->read(2 * k_page, 0x400), repeated(k_data_byte, 0x400));
        EXPECT_EQ(this->read((2 * k_page) + 0x400, k_page - 0x400), repeated(std::byte{}, k_page - 0x400));
    }

    TEST_F(ModuleMappingTest, SectionWithoutVirtualSizeKeepsTheRestOfA64KSlotReserved)
    {
        constexpr uint32_t alignment = 0x10000;
        const auto binary = this->map(zero_virtual_size_layout(alignment));
        ASSERT_EQ(binary.image_base, k_image_base);

        EXPECT_TRUE(this->region(2 * alignment).is_committed);
        EXPECT_EQ(this->read(2 * alignment, 0x400), repeated(k_data_byte, 0x400));

        const auto rest = this->region((2 * alignment) + k_page);
        EXPECT_TRUE(rest.is_reserved);
        EXPECT_FALSE(rest.is_committed);
    }

    TEST_F(ModuleMappingTest, SubPageAlignedImageIsOneReadWriteExecuteRegionLaidOutLikeTheFile)
    {
        const image_layout layout{
            .section_alignment = 0x200,
            .file_alignment = 0x200,
            .size_of_image = 0x1600,
            .sections =
                {
                    {.rva = 0x200,
                     .virtual_size = 0x10,
                     .raw_offset = 0x200,
                     .raw_size = 0x200,
                     .characteristics = k_code,
                     .fill = k_text_byte},
                    {.rva = 0x400,
                     .virtual_size = 0x800,
                     .raw_offset = 0x400,
                     .raw_size = 0x800,
                     .characteristics = k_rdata,
                     .fill = std::byte{0xA2}},
                    {.rva = 0xC00,
                     .virtual_size = 0xA00,
                     .raw_offset = 0xC00,
                     .raw_size = 0xA00,
                     .characteristics = k_data,
                     .fill = k_data_byte},
                },
        };
        const auto binary = this->map(layout);
        ASSERT_EQ(binary.image_base, k_image_base);

        for (const uint32_t page : {0u, k_page})
        {
            const auto info = this->region(page);
            EXPECT_TRUE(info.is_committed) << page;
            EXPECT_EQ(info.permissions.common, memory_permission::all) << page;
        }

        EXPECT_EQ(this->read(0, 0x1600), this->file_bytes(0, 0x1600));
        EXPECT_EQ(this->read(0x1600, 0x2000 - 0x1600), repeated(std::byte{}, 0x2000 - 0x1600));
    }

    TEST_F(ModuleMappingTest, SubPageAlignedSectionWithoutVirtualSizeIsMapped)
    {
        const image_layout layout{
            .section_alignment = 0x200,
            .file_alignment = 0x200,
            .size_of_image = 0x600,
            .sections =
                {
                    {.rva = 0x200,
                     .virtual_size = 0x10,
                     .raw_offset = 0x200,
                     .raw_size = 0x200,
                     .characteristics = k_code,
                     .fill = k_text_byte},
                    {.rva = 0x400,
                     .virtual_size = 0,
                     .raw_offset = 0x400,
                     .raw_size = 0x200,
                     .characteristics = k_data,
                     .fill = k_data_byte},
                },
        };
        const auto binary = this->map(layout);
        ASSERT_EQ(binary.image_base, k_image_base);

        const auto info = this->region(0);
        EXPECT_TRUE(info.is_committed);
        EXPECT_EQ(info.permissions.common, memory_permission::all);
        EXPECT_EQ(this->read(0x400, 0x200), repeated(k_data_byte, 0x200));
    }

    TEST_F(ModuleMappingTest, SectionPastTheImageEndFailsTheMap)
    {
        const image_layout layout{
            .section_alignment = k_page,
            .file_alignment = 0x200,
            .size_of_image = 2 * k_page,
            .sections =
                {
                    {.rva = k_page,
                     .virtual_size = 0x10,
                     .raw_offset = 0x200,
                     .raw_size = 0x200,
                     .characteristics = k_code,
                     .fill = k_text_byte},
                    {.rva = 2 * k_page,
                     .virtual_size = 0x10,
                     .raw_offset = 0x400,
                     .raw_size = 0x200,
                     .characteristics = k_data,
                     .fill = k_data_byte},
                },
        };
        this->map_neighbour(2 * k_page);

        EXPECT_THROW(this->map(layout), std::runtime_error);
        EXPECT_EQ(this->read(2 * k_page, k_page), repeated(k_neighbour_byte, k_page));
    }

    TEST_F(ModuleMappingTest, SectionWithoutDataAtTheImageEndFailsTheMapAndKeepsTheNeighbourProtection)
    {
        const image_layout layout{
            .section_alignment = k_page,
            .file_alignment = 0x200,
            .size_of_image = 2 * k_page,
            .sections =
                {
                    {.rva = k_page,
                     .virtual_size = 0x10,
                     .raw_offset = 0x200,
                     .raw_size = 0x200,
                     .characteristics = k_data,
                     .fill = k_data_byte},
                    {.rva = 2 * k_page, .virtual_size = 0x10, .characteristics = k_code},
                },
        };
        this->map_neighbour(2 * k_page);

        EXPECT_THROW(this->map(layout), std::runtime_error);

        const auto neighbour = this->region(2 * k_page);
        EXPECT_TRUE(neighbour.is_committed);
        EXPECT_EQ(neighbour.permissions.common, memory_permission::read_write);
        EXPECT_EQ(this->read(2 * k_page, k_page), repeated(k_neighbour_byte, k_page));
    }

    TEST_F(ModuleMappingTest, SubPageAlignedSectionPastTheImageEndFailsTheMap)
    {
        const image_layout layout{
            .section_alignment = 0x200,
            .file_alignment = 0x200,
            .size_of_image = 0x600,
            .sections =
                {
                    {.rva = 0x400,
                     .virtual_size = k_page,
                     .raw_offset = 0x400,
                     .raw_size = k_page,
                     .characteristics = k_data,
                     .fill = k_data_byte},
                },
        };
        this->map_neighbour(k_page);

        EXPECT_THROW(this->map(layout), std::runtime_error);
        EXPECT_EQ(this->read(k_page, k_page), repeated(k_neighbour_byte, k_page));
    }

    TEST_F(ModuleMappingTest, SubPageAlignedHeadersPastTheImageEndFailTheMap)
    {
        const image_layout layout{
            .section_alignment = 0x200,
            .file_alignment = 0x200,
            .size_of_image = 0x600,
            .size_of_headers = k_page + 0x200,
            .sections =
                {
                    {.rva = 0x200,
                     .virtual_size = 0x10,
                     .raw_offset = 0x200,
                     .raw_size = 0x200,
                     .characteristics = k_code,
                     .fill = k_text_byte},
                },
        };
        this->map_neighbour(k_page);

        EXPECT_THROW(this->map(layout), std::runtime_error);
        EXPECT_EQ(this->read(k_page, k_page), repeated(k_neighbour_byte, k_page));
    }
}

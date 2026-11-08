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

        template <typename Object, typename Member>
        size_t offset_of_member(const Member member)
        {
            const Object object{};
            const auto* start = reinterpret_cast<const std::byte*>(&object);
            const auto* field = reinterpret_cast<const std::byte*>(&(object.*member));
            return static_cast<size_t>(field - start);
        }

        template <typename Pointer>
        class ModuleRelocationTest : public testing::Test
        {
          protected:
            static constexpr Pointer preferred_base = sizeof(Pointer) == 4 ? 0x10000000ULL : 0x180000000ULL;
            static constexpr uint32_t pointer_rva = 0x1020;
            static constexpr uint32_t target_rva = 0x1234;

            static std::vector<std::byte> image(const uint16_t dll_characteristics = 0, const uint16_t characteristics = IMAGE_FILE_DLL,
                                                const uint32_t relocation_size = 12)
            {
                std::vector<std::byte> data(0x2200);
                PEDosHeader_t dos{};
                dos.e_magic = PEDosHeader_t::k_Magic;
                dos.e_lfanew = k_nt_offset;
                poke(data, 0, dos);

                PENTHeaders_t<Pointer> nt{};
                nt.Signature = decltype(nt)::k_Signature;
                nt.FileHeader.Machine = sizeof(Pointer) == 4 ? PEMachineType::I386 : PEMachineType::AMD64;
                nt.FileHeader.NumberOfSections = 1;
                nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
                nt.FileHeader.Characteristics = characteristics | (sizeof(Pointer) == 4 ? IMAGE_FILE_32BIT_MACHINE : 0);
                auto& optional = nt.OptionalHeader;
                optional.Magic = PEOptionalHeader_t<Pointer>::k_Magic;
                optional.ImageBase = preferred_base;
                optional.AddressOfEntryPoint = 0x1000;
                optional.SectionAlignment = 0x1000;
                optional.FileAlignment = 0x200;
                optional.SizeOfHeaders = 0x200;
                optional.SizeOfImage = 0x3000;
                optional.DllCharacteristics = dll_characteristics;
                optional.NumberOfRvaAndSizes = 16;
                optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = 0x1800;
                optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = relocation_size;
                poke(data, k_nt_offset, nt);

                IMAGE_SECTION_HEADER section{};
                section.VirtualAddress = 0x1000;
                section.Misc.VirtualSize = 0x2000;
                section.PointerToRawData = 0x200;
                section.SizeOfRawData = 0x2000;
                section.Characteristics = k_code;
                poke(data, k_nt_offset + sizeof(nt), section);
                poke(data, 0x220, static_cast<Pointer>(preferred_base + target_rva));

                IMAGE_BASE_RELOCATION relocations{};
                relocations.VirtualAddress = 0x1000;
                relocations.SizeOfBlock = 12;
                poke(data, 0xA00, relocations);
                constexpr auto type = sizeof(Pointer) == 4 ? IMAGE_REL_BASED_HIGHLOW : IMAGE_REL_BASED_DIR64;
                poke(data, 0xA08, static_cast<uint16_t>((type << 12) | 0x20));
                return data;
            }

            mapped_module map(const std::vector<std::byte>& data, const uint64_t relocation_base = 0,
                              const windows_path& path = windows_path{"C:\\test.dll"}, const uint64_t highest_address = 0,
                              NTSTATUS* status = nullptr)
            {
                return map_module_from_data<Pointer>(this->memory_, data, "test.dll", path, relocation_base, highest_address, status);
            }

            void expect_relocations(const mapped_module& module, const uint64_t relocation_base)
            {
                EXPECT_EQ(relocation_base + target_rva, this->memory_.template read_memory<Pointer>(module.image_base + pointer_rva));
                const auto header_offset = k_nt_offset + offset_of_member<PENTHeaders_t<Pointer>>(&PENTHeaders_t<Pointer>::OptionalHeader) +
                                           offset_of_member<PEOptionalHeader_t<Pointer>>(&PEOptionalHeader_t<Pointer>::ImageBase);
                EXPECT_EQ(relocation_base, this->memory_.template read_memory<Pointer>(module.image_base + header_offset));
                EXPECT_EQ(module.image_base + 0x1000, module.entry_point);
            }

            page_memory backend_{};
            memory_manager memory_{backend_};
        };

        using ModulePointerTypes = testing::Types<uint32_t, uint64_t>;
        TYPED_TEST_SUITE(ModuleRelocationTest, ModulePointerTypes);

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

    TYPED_TEST(ModuleRelocationTest, SecondaryViewPreservesTheOriginalRelocationBase)
    {
        const auto data = this->image();
        const auto original = this->map(data);
        const auto secondary = this->map(data, original.image_base);
        EXPECT_EQ(this->preferred_base, original.image_base);
        EXPECT_NE(original.image_base, secondary.image_base);
        this->expect_relocations(original, original.image_base);
        this->expect_relocations(secondary, original.image_base);
    }

    TYPED_TEST(ModuleRelocationTest, PlacementStatusDistinguishesPreferredAndFallbackAddresses)
    {
        const auto data = this->image();
        NTSTATUS status{};
        const auto original = this->map(data, 0, windows_path{"C:\\test.dll"}, 0, &status);
        EXPECT_EQ(status, STATUS_SUCCESS);
        this->expect_relocations(original, original.image_base);
        const auto relocated = this->map(data, 0, windows_path{"C:\\test.dll"}, 0, &status);
        EXPECT_EQ(status, STATUS_IMAGE_NOT_AT_BASE);
        EXPECT_NE(original.image_base, relocated.image_base);
        this->expect_relocations(relocated, relocated.image_base);
    }

    TYPED_TEST(ModuleRelocationTest, IndependentViewRelocatesToItsOwnBase)
    {
        const auto data = this->image();
        const auto original = this->map(data);
        const auto independent = this->map(data);
        EXPECT_NE(original.image_base, independent.image_base);
        this->expect_relocations(independent, independent.image_base);
    }

    TYPED_TEST(ModuleRelocationTest, DynamicDllsUseHighAddressesAndRelocateAbsolutePointers)
    {
        const auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
        const auto first = this->map(data);
        const auto second = this->map(data);
        EXPECT_NE(first.image_base, second.image_base);
        for (const auto* module : {&first, &second})
        {
            EXPECT_NE(this->preferred_base, module->image_base);
            EXPECT_EQ(this->preferred_base, module->image_base_file);
            if constexpr (sizeof(TypeParam) == 4)
            {
                EXPECT_GE(module->image_base, 0x70000000ULL);
                EXPECT_LT(module->image_base + module->size_of_image, 0x80000000ULL);
            }
            else
            {
                EXPECT_GE(module->image_base, 0x00007F0000000000ULL);
                EXPECT_LE(module->image_base + module->size_of_image - 1, MAX_ALLOCATION_ADDRESS);
            }
            this->expect_relocations(*module, module->image_base);
        }
    }

    TYPED_TEST(ModuleRelocationTest, DynamicPlacementRequiresARelocatableDynamicDll)
    {
        struct image_case
        {
            uint16_t dll_characteristics;
            uint16_t characteristics;
            uint32_t relocation_size;
        };

        const std::array cases{
            image_case{0, IMAGE_FILE_DLL, 12},
            image_case{IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, 0, 12},
            image_case{IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, IMAGE_FILE_DLL, 0},
            image_case{IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, IMAGE_FILE_DLL | IMAGE_FILE_RELOCS_STRIPPED, 12},
        };
        for (const auto& configuration : cases)
        {
            SCOPED_TRACE(configuration.characteristics);
            const auto data = this->image(configuration.dll_characteristics, configuration.characteristics, configuration.relocation_size);
            const auto module = this->map(data);
            EXPECT_EQ(this->preferred_base, module.image_base);
            this->expect_relocations(module, module.image_base);
            ASSERT_TRUE(unmap_module(this->memory_, module));
        }
    }

    TYPED_TEST(ModuleRelocationTest, SecondaryDynamicViewPreservesTheOriginalRelocationBase)
    {
        const auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
        const auto original = this->map(data);
        const auto secondary = this->map(data, original.image_base);
        EXPECT_NE(this->preferred_base, original.image_base);
        EXPECT_EQ(this->preferred_base, secondary.image_base);
        this->expect_relocations(secondary, original.image_base);
    }

    TYPED_TEST(ModuleRelocationTest, DynamicDllPlacementRespectsTheAddressCeiling)
    {
        constexpr uint64_t limit = 0x1FFFFFFF;
        constexpr TypeParam file_base = 0x10000000;
        auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
        const auto header_offset = k_nt_offset + offset_of_member<PENTHeaders_t<TypeParam>>(&PENTHeaders_t<TypeParam>::OptionalHeader) +
                                   offset_of_member<PEOptionalHeader_t<TypeParam>>(&PEOptionalHeader_t<TypeParam>::ImageBase);
        poke(data, header_offset, file_base);
        poke(data, 0x220, static_cast<TypeParam>(file_base + this->target_rva));
        const auto module = this->map(data, 0, windows_path{"C:\\test.dll"}, limit);
        EXPECT_LE(module.image_base + module.size_of_image - 1, limit);
        this->expect_relocations(module, module.image_base);
    }

    TYPED_TEST(ModuleRelocationTest, AConstrainedSecondaryViewKeepsTheOriginalRelocationBase)
    {
        constexpr uint64_t limit = 0x0FFFFFFF;
        const auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
        const auto original = this->map(data);
        const auto secondary = this->map(data, original.image_base, windows_path{"C:\\test.dll"}, limit);
        EXPECT_GT(original.image_base, limit);
        EXPECT_LE(secondary.image_base + secondary.size_of_image - 1, limit);
        this->expect_relocations(secondary, original.image_base);
    }

    TYPED_TEST(ModuleRelocationTest, TheWholeImageMustFitBelowTheAddressCeiling)
    {
        const auto data = this->image();
        const auto limit = this->preferred_base + 0x1000;
        const auto module = this->map(data, 0, windows_path{"C:\\test.dll"}, limit);
        EXPECT_NE(this->preferred_base, module.image_base);
        EXPECT_LE(module.image_base + module.size_of_image - 1, limit);
        this->expect_relocations(module, module.image_base);
    }

    TYPED_TEST(ModuleRelocationTest, AnImpossibleAddressCeilingDoesNotLeaveAnImageMapped)
    {
        const auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
        EXPECT_THROW(this->map(data, 0, windows_path{"C:\\test.dll"}, 0xFFFF), std::bad_alloc);
        EXPECT_FALSE(this->memory_.get_region_info(this->preferred_base).is_reserved);
    }

    TYPED_TEST(ModuleRelocationTest, StrippedImagesCannotMoveToSatisfyAnAddressCeiling)
    {
        const auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, IMAGE_FILE_DLL | IMAGE_FILE_RELOCS_STRIPPED, 0);
        EXPECT_THROW(this->map(data, 0, windows_path{"C:\\test.dll"}, this->preferred_base - 1), std::bad_alloc);
        EXPECT_FALSE(this->memory_.get_region_info(this->preferred_base).is_reserved);
        const auto original = this->map(data);
        this->expect_relocations(original, this->preferred_base);
        EXPECT_THROW(this->map(data), std::bad_alloc);
        const auto secondary = this->map(data, original.image_base);
        EXPECT_NE(original.image_base, secondary.image_base);
        this->expect_relocations(secondary, original.image_base);
    }

    TYPED_TEST(ModuleRelocationTest, UndeclaredDirectoriesDoNotCauseRelocations)
    {
        for (const bool short_header : {false, true})
        {
            SCOPED_TRACE(short_header);
            auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
            const auto optional_offset =
                k_nt_offset + offset_of_member<PENTHeaders_t<TypeParam>>(&PENTHeaders_t<TypeParam>::OptionalHeader);
            if (short_header)
            {
                const auto optional_size =
                    static_cast<uint16_t>(offset_of_member<PEOptionalHeader_t<TypeParam>>(&PEOptionalHeader_t<TypeParam>::DataDirectory));
                IMAGE_SECTION_HEADER section{};
                std::memcpy(&section, data.data() + k_nt_offset + sizeof(PENTHeaders_t<TypeParam>), sizeof(section));
                poke(data,
                     k_nt_offset + offset_of_member<PENTHeaders_t<TypeParam>>(&PENTHeaders_t<TypeParam>::FileHeader) +
                         offsetof(PEFileHeader_t, SizeOfOptionalHeader),
                     optional_size);
                poke(data, optional_offset + optional_size, section);
            }
            else
            {
                poke(data,
                     optional_offset + offset_of_member<PEOptionalHeader_t<TypeParam>>(&PEOptionalHeader_t<TypeParam>::NumberOfRvaAndSizes),
                     uint32_t{5});
            }
            const auto original = this->map(data);
            EXPECT_EQ(original.image_base, this->preferred_base);
            this->expect_relocations(original, this->preferred_base);
            const auto secondary = this->map(data, this->preferred_base + 0x10000);
            EXPECT_NE(original.image_base, secondary.image_base);
            EXPECT_EQ(this->preferred_base + this->target_rva,
                      this->memory_.template read_memory<TypeParam>(secondary.image_base + this->pointer_rva));
            ASSERT_TRUE(unmap_module(this->memory_, secondary));
            ASSERT_TRUE(unmap_module(this->memory_, original));
        }
    }

    TYPED_TEST(ModuleRelocationTest, AZeroImageSizeIsAnImageError)
    {
        auto data = this->image();
        const auto offset = k_nt_offset + offset_of_member<PENTHeaders_t<TypeParam>>(&PENTHeaders_t<TypeParam>::OptionalHeader) +
                            offset_of_member<PEOptionalHeader_t<TypeParam>>(&PEOptionalHeader_t<TypeParam>::SizeOfImage);
        poke(data, offset, uint32_t{0});
        EXPECT_THROW(this->map(data), std::runtime_error);
    }

    using ModuleRelocation32Test = ModuleRelocationTest<uint32_t>;
    using ModuleRelocation64Test = ModuleRelocationTest<uint64_t>;

    TEST_F(ModuleRelocation64Test, LowBasedDllsPreferLowAddressesWithoutRequiringThem)
    {
        constexpr uint64_t low_base = 0x10000000;
        constexpr uint64_t high_start = 0x100000000;
        for (const auto reservation : {0, 1, 2})
        {
            SCOPED_TRACE(reservation);
            auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
            const auto base_offset = k_nt_offset + offset_of_member<PENTHeaders_t<uint64_t>>(&PENTHeaders_t<uint64_t>::OptionalHeader) +
                                     offset_of_member<PEOptionalHeader_t<uint64_t>>(&PEOptionalHeader_t<uint64_t>::ImageBase);
            poke(data, base_offset, low_base);
            poke(data, 0x220, low_base + target_rva);
            const auto reserved_base = reservation == 2 ? uint64_t{0x10000} : low_base;
            const auto reserved_size = reservation == 2 ? static_cast<size_t>(high_start - reserved_base) : size_t{0x1000};
            if (reservation)
            {
                ASSERT_TRUE(this->memory_.allocate_memory(reserved_base, reserved_size, memory_permission::read_write, true));
            }
            const auto module = this->map(data);
            if (reservation == 2)
            {
                EXPECT_GE(module.image_base, high_start);
            }
            else
            {
                EXPECT_LT(module.image_base + module.size_of_image - 1, high_start);
            }
            this->expect_relocations(module, module.image_base);
            ASSERT_TRUE(unmap_module(this->memory_, module));
            if (reservation)
            {
                ASSERT_TRUE(this->memory_.release_memory(reserved_base, 0));
            }
        }
    }

    TEST_F(ModuleRelocation64Test, AnExhaustedHighRangeFallsBackBelow4GiB)
    {
        constexpr uint64_t high_start = 0x100000000;
        constexpr uint64_t ceiling = 0x1FFFFFFFF;
        constexpr size_t reservation_size = 0x80000000;
        ASSERT_TRUE(this->memory_.allocate_memory(high_start, reservation_size, memory_permission::read_write, true));
        ASSERT_TRUE(this->memory_.allocate_memory(high_start + reservation_size, reservation_size, memory_permission::read_write, true));
        const auto data = this->image();
        const auto module = this->map(data, 0, windows_path{"C:\\test.dll"}, ceiling);
        EXPECT_LT(module.image_base + module.size_of_image - 1, high_start);
        this->expect_relocations(module, module.image_base);
    }

    TEST_F(ModuleRelocation64Test, ASecondaryViewCanFallBackBelowAnExhaustedHighRange)
    {
        constexpr uint64_t high_start = 0x100000000;
        constexpr uint64_t ceiling = 0x1FFFFFFFF;
        const auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
        const auto original = this->map(data);
        constexpr size_t reservation_size = 0x80000000;
        ASSERT_TRUE(this->memory_.allocate_memory(high_start, reservation_size, memory_permission::read_write, true));
        ASSERT_TRUE(this->memory_.allocate_memory(high_start + reservation_size, reservation_size, memory_permission::read_write, true));
        const auto secondary = this->map(data, original.image_base, windows_path{"C:\\test.dll"}, ceiling);
        EXPECT_LT(secondary.image_base + secondary.size_of_image - 1, high_start);
        this->expect_relocations(secondary, original.image_base);
    }

    TEST_F(ModuleRelocation32Test, HighAndLowRelocationsAreAppliedWhenDynamicDllsMove)
    {
        auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
        poke(data, 0x220, uint16_t{0x1001});
        poke(data, 0x222, uint16_t{0x5678});
        poke(data, 0xA08, static_cast<uint16_t>((IMAGE_REL_BASED_HIGH << 12) | 0x20));
        poke(data, 0xA0A, static_cast<uint16_t>((IMAGE_REL_BASED_LOW << 12) | 0x22));
        const auto module = this->map(data);
        EXPECT_NE(preferred_base, module.image_base);
        EXPECT_EQ((module.image_base >> 16) + 1, this->memory_.read_memory<uint16_t>(module.image_base + 0x1020));
        EXPECT_EQ(0x5678u, this->memory_.read_memory<uint16_t>(module.image_base + 0x1022));
    }

    TEST_F(ModuleRelocation32Test, HighAdjConsumesItsSignedAddendBeforeTheNextRelocation)
    {
        for (const auto relocation_base : {uint64_t{0}, uint64_t{0x01000000}})
        {
            SCOPED_TRACE(relocation_base);
            auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, IMAGE_FILE_DLL, 16);
            poke(data, 0x220, uint16_t{0x1000});
            poke(data, 0x224, uint32_t{preferred_base + target_rva});
            poke(data, 0xA04, uint32_t{16});
            poke(data, 0xA08, static_cast<uint16_t>((IMAGE_REL_BASED_HIGHADJ << 12) | 0x20));
            poke(data, 0xA0A, uint16_t{0xF123});
            poke(data, 0xA0C, static_cast<uint16_t>((IMAGE_REL_BASED_HIGHLOW << 12) | 0x24));
            poke(data, 0xA0E, uint16_t{IMAGE_REL_BASED_ABSOLUTE});
            const auto module = this->map(data, relocation_base);
            const auto target_base = relocation_base ? relocation_base : module.image_base;
            EXPECT_EQ(target_base >> 16, this->memory_.read_memory<uint16_t>(module.image_base + 0x1020));
            EXPECT_EQ(target_base + target_rva, this->memory_.read_memory<uint32_t>(module.image_base + 0x1024));
            ASSERT_TRUE(unmap_module(this->memory_, module));
        }
    }

    TEST_F(ModuleRelocation32Test, HighAdjWithoutAnAddendFailsAndReleasesTheImage)
    {
        auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
        poke(data, 0xA08, uint16_t{IMAGE_REL_BASED_ABSOLUTE});
        poke(data, 0xA0A, static_cast<uint16_t>((IMAGE_REL_BASED_HIGHADJ << 12) | 0x20));
        EXPECT_THROW(this->map(data, 0x50000000), std::runtime_error);
        EXPECT_FALSE(this->memory_.get_region_info(preferred_base).is_reserved);
    }

    TEST_F(ModuleRelocation64Test, DynamicWow64CpuRemainsReachableFrom32BitCode)
    {
        for (const uint64_t file_base : {0x180000000ULL, 0x10000000ULL})
        {
            SCOPED_TRACE(file_base);
            auto data = this->image(IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
            const auto header_offset = k_nt_offset + offset_of_member<PENTHeaders_t<uint64_t>>(&PENTHeaders_t<uint64_t>::OptionalHeader) +
                                       offset_of_member<PEOptionalHeader_t<uint64_t>>(&PEOptionalHeader_t<uint64_t>::ImageBase);
            poke(data, header_offset, file_base);
            poke(data, 0x220, file_base + target_rva);
            const auto module = this->map(data, 0, windows_path{"C:\\wow64cpu.dll"});
            EXPECT_LT(module.image_base + module.size_of_image, uint64_t{1} << 32);
            this->expect_relocations(module, module.image_base);
            ASSERT_TRUE(unmap_module(this->memory_, module));
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

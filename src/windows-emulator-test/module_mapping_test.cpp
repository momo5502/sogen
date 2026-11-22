#include "std_include.hpp"

#include <module/module_mapping.hpp>

#include "page_memory.hpp"

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
            uint32_t file_size{};
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

        template <typename Pointer = std::uint64_t>
        std::vector<std::byte> build_image(const image_layout& layout)
        {
            const auto size_of_headers = layout.size_of_headers ? layout.size_of_headers : layout.file_alignment;
            size_t file_size = layout.file_size;
            if (!file_size)
            {
                file_size = size_of_headers;
                for (const auto& section : layout.sections)
                {
                    file_size = std::max<size_t>(file_size, static_cast<size_t>(section.raw_offset) + section.raw_size);
                }
            }

            std::vector<std::byte> image(file_size, std::byte{});

            PEDosHeader_t dos_header{};
            dos_header.e_magic = PEDosHeader_t::k_Magic;
            dos_header.e_lfanew = k_nt_offset;

            using nt_headers_t = PENTHeaders_t<Pointer>;
            nt_headers_t nt_headers{};
            nt_headers.Signature = nt_headers_t::k_Signature;
            nt_headers.FileHeader.Machine = sizeof(Pointer) == 4 ? PEMachineType::I386 : PEMachineType::AMD64;
            nt_headers.FileHeader.NumberOfSections = static_cast<uint16_t>(layout.sections.size());
            nt_headers.FileHeader.SizeOfOptionalHeader = sizeof(PEOptionalHeader_t<Pointer>);
            nt_headers.FileHeader.Characteristics = IMAGE_FILE_DLL;

            auto& optional_header = nt_headers.OptionalHeader;
            optional_header.Magic = PEOptionalHeader_t<Pointer>::k_Magic;
            optional_header.ImageBase = static_cast<Pointer>(sizeof(Pointer) == 4 ? 0x10000000 : k_image_base);
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

                if (layout_section.raw_offset && layout_section.raw_offset < image.size())
                {
                    std::memset(image.data() + layout_section.raw_offset, std::to_integer<int>(layout_section.fill),
                                std::min<size_t>(layout_section.raw_size, image.size() - layout_section.raw_offset));
                }
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

        constexpr NTSTATUS accepted = STATUS_SUCCESS;
        constexpr NTSTATUS rejected = STATUS_INVALID_IMAGE_FORMAT;
        constexpr NTSTATUS rejected_not_mz = STATUS_INVALID_IMAGE_NOT_MZ;

        struct layout_row
        {
            const char* name;
            NTSTATUS windows_status;
            image_layout layout;
        };

        section_layout text_section(const uint32_t rva, const uint32_t virtual_size, const uint32_t raw_offset, const uint32_t raw_size)
        {
            return {rva, virtual_size, raw_offset, raw_size, k_code, k_text_byte};
        }

        section_layout data_section(const uint32_t rva, const uint32_t virtual_size, const uint32_t raw_offset, const uint32_t raw_size)
        {
            return {rva, virtual_size, raw_offset, raw_size, k_data, k_data_byte};
        }

        struct sub_page_case
        {
            const char* name;
            bool windows_accepts;
            uint32_t section_alignment;
            uint32_t file_alignment;
            uint32_t text_raw_size;
            uint32_t data_virtual_size;
            uint32_t data_raw_size;
        };

        layout_row sub_page_row(const sub_page_case& c)
        {
            const auto text_rva = std::max<uint32_t>(c.section_alignment, 0x200);
            const auto data_rva = std::max<uint32_t>(2 * c.section_alignment, 0x400);
            const auto file_size = static_cast<uint32_t>(align_up(data_rva + c.data_raw_size, 0x200));
            return {c.name,
                    c.windows_accepts ? accepted : rejected,
                    {c.section_alignment,
                     c.file_alignment,
                     0x3000,
                     0x200,
                     file_size,
                     {text_section(text_rva, 0x10, text_rva, c.text_raw_size),
                      data_section(data_rva, c.data_virtual_size, data_rva, c.data_raw_size)}}};
        }

        struct raw_data_case
        {
            const char* name;
            bool windows_accepts;
            uint32_t section_alignment;
            uint32_t size_of_image;
            uint32_t data_virtual_size;
            uint32_t data_raw_size;
            uint32_t data_bytes_in_file;
        };

        layout_row raw_data_row(const raw_data_case& c)
        {
            return {c.name,
                    c.windows_accepts ? accepted : rejected,
                    {c.section_alignment,
                     0x200,
                     c.size_of_image,
                     0x200,
                     0x400 + c.data_bytes_in_file,
                     {text_section(c.section_alignment, 0x10, 0x200, 0x200),
                      data_section(2 * c.section_alignment, c.data_virtual_size, 0x400, c.data_raw_size)}}};
        }

        constexpr sub_page_case k_sub_page_cases[] = {
            {"sp200_r200_v1FF", true, 0x200, 0x200, 0x200, 0x1FF, 0x200},
            {"sp200_r200_v200", true, 0x200, 0x200, 0x200, 0x200, 0x200},
            {"sp200_r200_v201", false, 0x200, 0x200, 0x200, 0x201, 0x200},
            {"sp200_r200_v3FF", false, 0x200, 0x200, 0x200, 0x3FF, 0x200},
            {"sp200_r200_v400", false, 0x200, 0x200, 0x200, 0x400, 0x200},
            {"sp200_r200_v401", false, 0x200, 0x200, 0x200, 0x401, 0x200},
            {"sp200_r200_v7FF", false, 0x200, 0x200, 0x200, 0x7FF, 0x200},
            {"sp200_r200_v800", false, 0x200, 0x200, 0x200, 0x800, 0x200},
            {"sp200_r200_v1000", false, 0x200, 0x200, 0x200, 0x1000, 0x200},
            {"sp200_r200_v1800", false, 0x200, 0x200, 0x200, 0x1800, 0x200},
            {"sp200_r400_v1FF", true, 0x200, 0x200, 0x200, 0x1FF, 0x400},
            {"sp200_r400_v200", true, 0x200, 0x200, 0x200, 0x200, 0x400},
            {"sp200_r400_v201", true, 0x200, 0x200, 0x200, 0x201, 0x400},
            {"sp200_r400_v3FF", true, 0x200, 0x200, 0x200, 0x3FF, 0x400},
            {"sp200_r400_v400", true, 0x200, 0x200, 0x200, 0x400, 0x400},
            {"sp200_r400_v401", false, 0x200, 0x200, 0x200, 0x401, 0x400},
            {"sp200_r400_v7FF", false, 0x200, 0x200, 0x200, 0x7FF, 0x400},
            {"sp200_r400_v800", false, 0x200, 0x200, 0x200, 0x800, 0x400},
            {"sp200_r400_v1000", false, 0x200, 0x200, 0x200, 0x1000, 0x400},
            {"sp200_r400_v1800", false, 0x200, 0x200, 0x200, 0x1800, 0x400},
            {"sp400_r200_v1FF", true, 0x400, 0x400, 0x400, 0x1FF, 0x400},
            {"sp400_r200_v200", true, 0x400, 0x400, 0x400, 0x200, 0x400},
            {"sp400_r200_v201", true, 0x400, 0x400, 0x400, 0x201, 0x400},
            {"sp400_r200_v3FF", true, 0x400, 0x400, 0x400, 0x3FF, 0x400},
            {"sp400_r200_v400", true, 0x400, 0x400, 0x400, 0x400, 0x400},
            {"sp400_r200_v401", false, 0x400, 0x400, 0x400, 0x401, 0x400},
            {"sp400_r200_v7FF", false, 0x400, 0x400, 0x400, 0x7FF, 0x400},
            {"sp400_r200_v800", false, 0x400, 0x400, 0x400, 0x800, 0x400},
            {"sp400_r200_v1000", false, 0x400, 0x400, 0x400, 0x1000, 0x400},
            {"sp400_r200_v1800", false, 0x400, 0x400, 0x400, 0x1800, 0x400},
            {"sp400_r400_v1FF", true, 0x400, 0x400, 0x400, 0x1FF, 0x400},
            {"sp400_r400_v200", true, 0x400, 0x400, 0x400, 0x200, 0x400},
            {"sp400_r400_v201", true, 0x400, 0x400, 0x400, 0x201, 0x400},
            {"sp400_r400_v3FF", true, 0x400, 0x400, 0x400, 0x3FF, 0x400},
            {"sp400_r400_v400", true, 0x400, 0x400, 0x400, 0x400, 0x400},
            {"sp400_r400_v401", false, 0x400, 0x400, 0x400, 0x401, 0x400},
            {"sp400_r400_v7FF", false, 0x400, 0x400, 0x400, 0x7FF, 0x400},
            {"sp400_r400_v800", false, 0x400, 0x400, 0x400, 0x800, 0x400},
            {"sp400_r400_v1000", false, 0x400, 0x400, 0x400, 0x1000, 0x400},
            {"sp400_r400_v1800", false, 0x400, 0x400, 0x400, 0x1800, 0x400},
            {"spsa400_r200_v200", true, 0x400, 0x200, 0x200, 0x200, 0x200},
            {"spsa400_r200_v201", false, 0x400, 0x200, 0x200, 0x201, 0x200},
            {"spsa400_r200_v400", false, 0x400, 0x200, 0x200, 0x400, 0x200},
            {"spsa400_r200_v401", false, 0x400, 0x200, 0x200, 0x401, 0x200},
            {"spsa400_r200_v1000", false, 0x400, 0x200, 0x200, 0x1000, 0x200},
            {"sp200_r1A0_v1A0", true, 0x200, 0x200, 0x200, 0x1A0, 0x1A0},
            {"sp200_r1A0_v1A1", false, 0x200, 0x200, 0x200, 0x1A1, 0x1A0},
            {"sp200_r1A0_v1FF", false, 0x200, 0x200, 0x200, 0x1FF, 0x1A0},
            {"sp200_r1A0_v200", false, 0x200, 0x200, 0x200, 0x200, 0x1A0},
            {"sp200_r1A0_v201", false, 0x200, 0x200, 0x200, 0x201, 0x1A0},
            {"sp_fa180_sa200", false, 0x200, 0x180, 0x200, 0x100, 0x180},
            {"sp_sa300_fa300", false, 0x300, 0x300, 0x200, 0x100, 0x200},
            {"sp_sa400_fa300", false, 0x400, 0x300, 0x200, 0x100, 0x200},
            {"sp_sa100_fa100", true, 0x100, 0x100, 0x200, 0x100, 0x200},
            {"sp_sa20_fa20", true, 0x20, 0x20, 0x200, 0x100, 0x200},
        };

        constexpr raw_data_case k_raw_data_cases[] = {
            {"rx4k_full", true, 0x1000, 0x3000, 0x10, 0x3000, 0x3000},
            {"rx4k_a2_short1", false, 0x1000, 0x3000, 0x10, 0x3000, 0x2FFF},
            {"rx4k_a_in_excess", false, 0x1000, 0x3000, 0x10, 0x3000, 0x2000},
            {"rx4k_b1_slot_plus1", false, 0x1000, 0x3000, 0x10, 0x3000, 0x1001},
            {"rx4k_b_slot_end", false, 0x1000, 0x3000, 0x10, 0x3000, 0x1000},
            {"rx4k_b0_slot_minus1", false, 0x1000, 0x3000, 0x10, 0x3000, 0xFFF},
            {"rx4k_c_in_slot", false, 0x1000, 0x3000, 0x10, 0x3000, 0x800},
            {"rx64k_full", true, 0x10000, 0x30000, 0x10, 0x12000, 0x12000},
            {"rx64k_a2_past_slot", false, 0x10000, 0x30000, 0x10, 0x12000, 0x11000},
            {"rx64k_slot64_end", false, 0x10000, 0x30000, 0x10, 0x12000, 0x10000},
            {"rx64k_slot64_minus1", false, 0x10000, 0x30000, 0x10, 0x12000, 0xFFFF},
            {"rx64k_a_in_slot", false, 0x10000, 0x30000, 0x10, 0x12000, 0x2000},
            {"rx64k_b_page_end", false, 0x10000, 0x30000, 0x10, 0x12000, 0x1000},
            {"rx64k_c_in_page", false, 0x10000, 0x30000, 0x10, 0x12000, 0x800},
            {"rx_packed_huge", false, 0x1000, 0x3000, 0x10, 0x100000, 0x200},
            {"rx_packed_vs2_slot_end", false, 0x1000, 0x4000, 0x2000, 0x100000, 0x2000},
            {"rx_packed_vs2_slot_minus1", false, 0x1000, 0x4000, 0x2000, 0x100000, 0x1FFF},
            {"rx_packed_vs2_in_slot", false, 0x1000, 0x4000, 0x2000, 0x100000, 0x1000},
        };

        template <typename Pointer>
        void expect_layout(const layout_row& row)
        {
            const auto file = build_image<Pointer>(row.layout);
            EXPECT_EQ(check_image_layout(file), row.windows_status);

            page_memory backend{};
            memory_manager memory{backend};
            const auto map = [&] { return map_module_from_data<Pointer>(memory, file, "test.dll", windows_path{"C:\\test.dll"}); };
            if (row.windows_status == accepted)
            {
                EXPECT_NO_THROW(map());
                return;
            }

            try
            {
                map();
                ADD_FAILURE() << "the image was mapped";
            }
            catch (const invalid_image_format& e)
            {
                EXPECT_EQ(e.status, row.windows_status);
            }
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

    TEST_F(ModuleMappingTest, RawDataPastTheFileEndIsZeroFilled)
    {
        const image_layout layout{
            .section_alignment = k_page,
            .file_alignment = 0x200,
            .size_of_image = 4 * k_page,
            .size_of_headers = 0x400,
            .file_size = 0x1600,
            .sections =
                {
                    {.rva = k_page,
                     .virtual_size = 2 * k_page,
                     .raw_offset = 0x400,
                     .raw_size = 0x3000,
                     .characteristics = k_code,
                     .fill = k_text_byte},
                    {.rva = 3 * k_page,
                     .virtual_size = 0x10,
                     .raw_offset = 0x1400,
                     .raw_size = 0x200,
                     .characteristics = k_data,
                     .fill = k_data_byte},
                },
        };
        this->map(layout);

        EXPECT_EQ(this->read(k_page, k_page), this->file_bytes(0x400, k_page));
        EXPECT_EQ(this->read(2 * k_page, 0x200), this->file_bytes(0x1400, 0x200));
        EXPECT_EQ(this->read((2 * k_page) + 0x200, k_page - 0x200), repeated(std::byte{}, k_page - 0x200));
    }

    TEST(ModuleLayoutTest, ImagesAreAcceptedOrRejectedLikeWindows)
    {
        // Rows are Windows 11 captures that are identical for PE32 and PE32+, except the kernel_subpage_* rows, which follow the
        // sub-page branch of ntoskrnl's MiParseImageSectionHeaders.
        std::vector<layout_row> rows{
            {"ctl4k",
             accepted,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x800, 0x400, 0x200)}}},
            {"ctl64k",
             accepted,
             {0x10000, 0x200, 0x30000, 0x200, 0x0, {text_section(0x10000, 0x10, 0x200, 0x200), data_section(0x20000, 0x10, 0x400, 0x200)}}},
            {"midgap64k",
             rejected,
             {0x10000, 0x200, 0x40000, 0x200, 0x0, {text_section(0x10000, 0x10, 0x200, 0x200), data_section(0x30000, 0x10, 0x400, 0x200)}}},
            {"gap_subpage",
             accepted,
             {0x200, 0x200, 0x800, 0x200, 0x0, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x600, 0x100, 0x600, 0x200)}}},
            {"overlap4k",
             rejected,
             {0x1000, 0x200, 0x4000, 0x200, 0x0, {text_section(0x1000, 0x2000, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x200)}}},
            {"dup_va",
             rejected,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x1000, 0x10, 0x400, 0x200)}}},
            {"out_of_order",
             rejected,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x2000, 0x10, 0x200, 0x200), data_section(0x1000, 0x10, 0x400, 0x200)}}},
            {"va_in_headers",
             rejected,
             {0x1000, 0x200, 0x2000, 0x200, 0x0, {text_section(0x0, 0x10, 0x200, 0x200), data_section(0x1000, 0x10, 0x400, 0x200)}}},
            {"soh_gt_first_va",
             rejected,
             {0x1000, 0x200, 0x4000, 0x1400, 0x0, {text_section(0x1000, 0x10, 0x1400, 0x200), data_section(0x2000, 0x10, 0x1600, 0x200)}}},
            {"soh_gt_soi",
             rejected,
             {0x1000, 0x200, 0x3000, 0x4000, 0x0, {text_section(0x1000, 0x10, 0x4000, 0x200), data_section(0x2000, 0x10, 0x4200, 0x200)}}},
            {"soh_lt_tables",
             accepted,
             {0x1000, 0x200, 0x3000, 0x100, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x200)}}},
            {"soh_not_fa_multiple",
             accepted,
             {0x1000, 0x200, 0x3000, 0x300, 0x0, {text_section(0x1000, 0x10, 0x400, 0x200), data_section(0x2000, 0x10, 0x600, 0x200)}}},
            {"nsec0", rejected, {0x1000, 0x200, 0x1000, 0x200, 0x400, {}}},
            {"nsec0_soi_small", rejected, {0x1000, 0x200, 0x200, 0x200, 0x400, {}}},
            {"soi_zero",
             rejected,
             {0x1000, 0x200, 0x0, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x200)}}},
            {"va_eq_soi",
             rejected,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x3000, 0x10, 0x400, 0x200)}}},
            {"short_raw_only",
             accepted,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x3000)}}},
            {"short64k_2f000",
             rejected,
             {0x10000, 0x200, 0x2F000, 0x200, 0x0, {text_section(0x10000, 0x10, 0x200, 0x200), data_section(0x20000, 0x10, 0x400, 0x200)}}},
            {"short64k_21000",
             rejected,
             {0x10000, 0x200, 0x21000, 0x200, 0x0, {text_section(0x10000, 0x10, 0x200, 0x200), data_section(0x20000, 0x10, 0x400, 0x200)}}},
            {"long64k_31000",
             accepted,
             {0x10000, 0x200, 0x31000, 0x200, 0x0, {text_section(0x10000, 0x10, 0x200, 0x200), data_section(0x20000, 0x10, 0x400, 0x200)}}},
            {"va_unaligned_4k",
             rejected,
             {0x1000, 0x200, 0x4000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2200, 0x10, 0x400, 0x200)}}},
            {"va_unaligned_first",
             rejected,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1200, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x200)}}},
            {"va_page_not_64k",
             rejected,
             {0x10000, 0x200, 0x30000, 0x200, 0x0, {text_section(0x10000, 0x10, 0x200, 0x200), data_section(0x21000, 0x10, 0x400, 0x200)}}},
            {"lowalign_va_unaligned",
             accepted,
             {0x200, 0x200, 0x800, 0x200, 0x0, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x500, 0x10, 0x500, 0x200)}}},
            {"sa8k",
             accepted,
             {0x2000, 0x200, 0x6000, 0x200, 0x0, {text_section(0x2000, 0x10, 0x200, 0x200), data_section(0x4000, 0x10, 0x400, 0x200)}}},
            {"sa_lt_fa_4k",
             rejected,
             {0x1000,
              0x2000,
              0x3000,
              0x200,
              0x0,
              {text_section(0x1000, 0x10, 0x2000, 0x2000), data_section(0x2000, 0x10, 0x4000, 0x2000)}}},
            {"sa_lt_fa_sub",
             rejected,
             {0x200, 0x400, 0x800, 0x200, 0x0, {text_section(0x200, 0x10, 0x200, 0x400), data_section(0x400, 0x10, 0x400, 0x400)}}},
            {"sa_ne_fa_sub",
             accepted,
             {0x400, 0x200, 0xC00, 0x200, 0x0, {text_section(0x400, 0x10, 0x400, 0x200), data_section(0x800, 0x10, 0x800, 0x200)}}},
            {"sa_not_pow2",
             rejected,
             {0x1800, 0x200, 0x4800, 0x200, 0x0, {text_section(0x1800, 0x10, 0x200, 0x200), data_section(0x3000, 0x10, 0x400, 0x200)}}},
            {"sa_zero",
             rejected,
             {0x0, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x200)}}},
            {"fa_not_pow2",
             rejected,
             {0x1000, 0x300, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x200)}}},
            {"fa_0x100",
             rejected,
             {0x1000, 0x100, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x200)}}},
            {"fa_zero",
             rejected,
             {0x1000, 0x0, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x200)}}},
            {"fa_1000",
             accepted,
             {0x1000,
              0x1000,
              0x3000,
              0x1000,
              0x0,
              {text_section(0x1000, 0x10, 0x1000, 0x1000), data_section(0x2000, 0x10, 0x2000, 0x1000)}}},
            {"lowalign_ptr_ne_va",
             rejected,
             {0x200, 0x200, 0x600, 0x200, 0x0, {text_section(0x200, 0x10, 0x400, 0x200), data_section(0x400, 0x10, 0x600, 0x200)}}},
            {"lowalign_soi_odd",
             accepted,
             {0x200, 0x200, 0x510, 0x200, 0x0, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0x100, 0x400, 0x200)}}},
            {"lowalign_soi_short",
             accepted,
             {0x200, 0x200, 0x500, 0x200, 0x0, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0x200, 0x400, 0x200)}}},
            {"raw_ptr_odd",
             accepted,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x480, 0x200)}}},
            {"raw_past_eof",
             rejected,
             {0x1000, 0x200, 0x3000, 0x200, 0x500, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x400)}}},
            {"raw_ptr_past_eof",
             rejected,
             {0x1000, 0x200, 0x3000, 0x200, 0x400, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x2000, 0x200)}}},
            {"raw_size_odd",
             accepted,
             {0x1000, 0x200, 0x3000, 0x200, 0x600, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x1A0)}}},
            {"raw_shared",
             accepted,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"headgap4k",
             rejected,
             {0x1000, 0x200, 0x7000, 0x200, 0x0, {text_section(0x3000, 0x10, 0x200, 0x200), data_section(0x4000, 0x2800, 0x400, 0x200)}}},
            {"midgap4k",
             rejected,
             {0x1000, 0x200, 0x7000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x4000, 0x2800, 0x400, 0x200)}}},
            {"rx_upx_shape",
             accepted,
             {0x1000,
              0x200,
              0x13000,
              0x200,
              0x1200,
              {{0x1000, 0x10000, 0, 0, k_data, k_data_byte}, {0x11000, 0x2000, 0x200, 0x1000, k_code, k_text_byte}}}},
            {"lr4k_example",
             accepted,
             {0x1000,
              0x200,
              0x3000,
              0x400,
              0x1600,
              {text_section(0x1000, 0x10, 0x400, 0x3000), data_section(0x2000, 0x10, 0x1400, 0x200)}}},
            {"lr4k_last_plus1",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x400,
              0x1600,
              {text_section(0x1000, 0x10, 0x400, 0x3000), data_section(0x2000, 0x10, 0x1400, 0x201)}}},
            {"lr4k_last_minus1",
             accepted,
             {0x1000,
              0x200,
              0x3000,
              0x400,
              0x1600,
              {text_section(0x1000, 0x10, 0x400, 0x3000), data_section(0x2000, 0x10, 0x1400, 0x1FF)}}},
            {"lr4k_last_overruns",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x400,
              0x1600,
              {text_section(0x1000, 0x10, 0x400, 0x3000), data_section(0x2000, 0x10, 0x1400, 0x400)}}},
            {"lr64k_example",
             accepted,
             {0x10000,
              0x200,
              0x30000,
              0x400,
              0x1600,
              {text_section(0x10000, 0x10, 0x400, 0x3000), data_section(0x20000, 0x10, 0x1400, 0x200)}}},
            {"lr64k_text_raw_past_slot",
             accepted,
             {0x10000,
              0x200,
              0x30000,
              0x400,
              0x1600,
              {text_section(0x10000, 0x10, 0x400, 0x12000), data_section(0x20000, 0x10, 0x1400, 0x200)}}},
            {"lr4k_early_mapped_past_eof",
             accepted,
             {0x1000,
              0x200,
              0x4000,
              0x400,
              0x1600,
              {text_section(0x1000, 0x2000, 0x400, 0x3000), data_section(0x3000, 0x10, 0x1400, 0x200)}}},
            {"lr4k_early_mapped_to_eof",
             accepted,
             {0x1000,
              0x200,
              0x4000,
              0x400,
              0x2600,
              {text_section(0x1000, 0x2000, 0x400, 0x3000), data_section(0x3000, 0x10, 0x2400, 0x200)}}},
            {"lr4k_early_ptr_past_eof",
             accepted,
             {0x1000, 0x200, 0x3000, 0x200, 0x400, {text_section(0x1000, 0x10, 0x4000, 0x200), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lr4k_only_section_past_eof", rejected, {0x1000, 0x200, 0x2000, 0x200, 0x400, {text_section(0x1000, 0x10, 0x200, 0x400)}}},
            {"lrsp_last_overruns",
             accepted,
             {0x200, 0x200, 0x3000, 0x200, 0x600, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0x10, 0x400, 0x400)}}},
            {"lrsp_early_overruns",
             accepted,
             {0x200, 0x200, 0x3000, 0x200, 0x800, {text_section(0x200, 0x10, 0x200, 0x800), data_section(0x600, 0x10, 0x600, 0x200)}}},
            {"lrsp_last_not_highest",
             accepted,
             {0x200, 0x200, 0x3000, 0x200, 0x800, {data_section(0x600, 0x10, 0x600, 0x200), text_section(0x200, 0x10, 0x200, 0x800)}}},
            {"lw4k_last_ptr0_example",
             rejected,
             {0x1000, 0x200, 0x4000, 0x400, 0x1600, {text_section(0x1000, 0x10, 0x400, 0x200), data_section(0x2000, 0x2000, 0x0, 0x3000)}}},
            {"lw4k_last_ptr0_fits",
             accepted,
             {0x1000, 0x200, 0x4000, 0x400, 0x1600, {text_section(0x1000, 0x10, 0x400, 0x200), data_section(0x2000, 0x2000, 0x0, 0x200)}}},
            {"lw4k_last_ptr0_raw_eq_file",
             accepted,
             {0x1000, 0x200, 0x4000, 0x400, 0x1600, {text_section(0x1000, 0x10, 0x400, 0x200), data_section(0x2000, 0x2000, 0x0, 0x1600)}}},
            {"lw4k_last_ptr0_raw_plus1",
             rejected,
             {0x1000, 0x200, 0x4000, 0x400, 0x1600, {text_section(0x1000, 0x10, 0x400, 0x200), data_section(0x2000, 0x2000, 0x0, 0x1601)}}},
            {"lw4k_early_ptr0_raw_big",
             rejected_not_mz,
             {0x1000, 0x200, 0x4000, 0x400, 0x1600, {text_section(0x1000, 0x10, 0x0, 0x3000), data_section(0x2000, 0x2000, 0x400, 0x200)}}},
            {"lw64k_last_ptr0_example",
             rejected,
             {0x10000,
              0x200,
              0x30000,
              0x400,
              0x1600,
              {text_section(0x10000, 0x10, 0x400, 0x200), data_section(0x20000, 0x10, 0x0, 0x3000)}}},
            {"lw4k_last_raw0_ptr_past_eof",
             accepted,
             {0x1000, 0x200, 0x3000, 0x200, 0x400, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x4000, 0x0)}}},
            {"lw4k_last_raw0_ptr_at_eof",
             accepted,
             {0x1000, 0x200, 0x3000, 0x200, 0x400, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x400, 0x0)}}},
            {"lw4k_early_wrap",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0xFFFFFE00, 0x400), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lw4k_last_wrap",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0xFFFFFE00, 0x400)}}},
            {"lw4k_early_end_2p32_ptr",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0xFFFFFC00, 0x400), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lw4k_early_end_2p32_size",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x400, 0xFFFFFC00), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lw4k_early_end_max_ptr",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0xFFFFFC00, 0x3FF), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lw4k_early_end_max_size",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFEFFF), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lwsp_early_wrap",
             rejected,
             {0x200, 0x200, 0x3000, 0x200, 0x800, {text_section(0x200, 0x10, 0x200, 0xFFFFFE00), data_section(0x400, 0x10, 0x400, 0x200)}}},
            {"lwsp_last_wrap",
             rejected,
             {0x200, 0x200, 0x3000, 0x200, 0x800, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0x10, 0x400, 0xFFFFFC00)}}},
            {"lwsp_early_huge_nowrap",
             accepted,
             {0x200,
              0x200,
              0x3000,
              0x200,
              0x800,
              {data_section(0x2000, 0x10, 0x2000, 0xFFFFD000), text_section(0x200, 0x10, 0x200, 0x200)}}},
            {"lwsp_last_huge_nowrap",
             accepted,
             {0x200,
              0x200,
              0x3000,
              0x200,
              0x800,
              {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x2000, 0xFFFFD000)}}},
            {"lwsp_last_ptr0_va0",
             accepted,
             {0x200, 0x200, 0x3000, 0x200, 0x800, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x0, 0x10, 0x0, 0x3000)}}},
            {"lwsp_early_ptr0_va0",
             accepted,
             {0x200, 0x200, 0x3000, 0x200, 0x800, {data_section(0x0, 0x10, 0x0, 0x3000), text_section(0x200, 0x10, 0x200, 0x200)}}},
            {"lb4k_end_FFFFFFFE",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFEFFE), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lb4k_end_FFFFFE01",
             rejected,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFEE01), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lb4k_end_FFFFFE00",
             accepted,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFEE00), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lb4k_end_FFFFFDFF",
             accepted,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFEDFF), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lb4k_end_FFFFF001",
             accepted,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFE001), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lb4k_end_FFFFF000",
             accepted,
             {0x1000,
              0x200,
              0x3000,
              0x200,
              0x400,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFE000), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lbsp_end_FFFFFFFF",
             accepted,
             {0x200,
              0x200,
              0x3000,
              0x200,
              0x800,
              {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x2000, 0xFFFFDFFF)}}},
            {"lbsp_end_FFFFFFFE",
             accepted,
             {0x200,
              0x200,
              0x3000,
              0x200,
              0x800,
              {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x2000, 0x10, 0x2000, 0xFFFFDFFE)}}},
            {"lb4k_early_ptr0_fits",
             rejected_not_mz,
             {0x1000, 0x200, 0x3000, 0x200, 0x1600, {text_section(0x1000, 0x10, 0x0, 0x200), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lb4k_early_ptr0_eq_file",
             rejected_not_mz,
             {0x1000, 0x200, 0x3000, 0x200, 0x1600, {text_section(0x1000, 0x10, 0x0, 0x1600), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lb4k_early_ptr0_plus1",
             rejected_not_mz,
             {0x1000, 0x200, 0x3000, 0x200, 0x1600, {text_section(0x1000, 0x10, 0x0, 0x1601), data_section(0x2000, 0x10, 0x200, 0x200)}}},
            {"lb4k_early_ptr0_vs_big",
             rejected_not_mz,
             {0x1000, 0x200, 0x4000, 0x200, 0x1600, {text_section(0x1000, 0x2000, 0x0, 0x3000), data_section(0x3000, 0x10, 0x200, 0x200)}}},
            {"lb4k_early_ptr0_big_last_overruns",
             rejected_not_mz,
             {0x1000, 0x200, 0x4000, 0x200, 0x1600, {text_section(0x1000, 0x10, 0x0, 0x3000), data_section(0x2000, 0x10, 0x1400, 0x400)}}},
            {"lf_fa400_end_FFFFFC00",
             accepted,
             {0x1000,
              0x400,
              0x3000,
              0x400,
              0x800,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFEC00), data_section(0x2000, 0x10, 0x400, 0x400)}}},
            {"lf_fa400_end_FFFFFC01",
             rejected,
             {0x1000,
              0x400,
              0x3000,
              0x400,
              0x800,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFEC01), data_section(0x2000, 0x10, 0x400, 0x400)}}},
            {"lf_fa400_end_FFFFFE01",
             rejected,
             {0x1000,
              0x400,
              0x3000,
              0x400,
              0x800,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFEE01), data_section(0x2000, 0x10, 0x400, 0x400)}}},
            {"lf_fa1000_end_FFFFF000",
             accepted,
             {0x1000,
              0x1000,
              0x3000,
              0x200,
              0x2000,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFE000), data_section(0x2000, 0x10, 0x1000, 0x1000)}}},
            {"lf_fa1000_end_FFFFF001",
             rejected,
             {0x1000,
              0x1000,
              0x3000,
              0x200,
              0x2000,
              {text_section(0x1000, 0x10, 0x1000, 0xFFFFE001), data_section(0x2000, 0x10, 0x1000, 0x1000)}}},
            {"lf_fa10000_end_FFFF0000",
             accepted,
             {0x10000,
              0x10000,
              0x30000,
              0x200,
              0x20000,
              {text_section(0x10000, 0x10, 0x10000, 0xFFFE0000), data_section(0x20000, 0x10, 0x10000, 0x10000)}}},
            {"lf_fa10000_end_FFFF0001",
             rejected,
             {0x10000,
              0x10000,
              0x30000,
              0x200,
              0x20000,
              {text_section(0x10000, 0x10, 0x10000, 0xFFFE0001), data_section(0x20000, 0x10, 0x10000, 0x10000)}}},
            {"kernel_subpage_no_raw_data",
             rejected,
             {0x200, 0x200, 0x3000, 0x200, 0x600, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0x10, 0x400, 0)}}},
            {"kernel_subpage_no_raw_data_pointer_0",
             rejected,
             {0x200, 0x200, 0x3000, 0x200, 0x600, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0x10, 0, 0)}}},
            {"kernel_subpage_bss_like",
             rejected,
             {0x200, 0x200, 0x3000, 0x200, 0x600, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0x1000, 0x400, 0)}}},
            {"kernel_subpage_empty_pointer_differs",
             rejected,
             {0x200, 0x200, 0x3000, 0x200, 0x600, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0, 0x800, 0)}}},
            {"kernel_subpage_empty_at_its_address",
             accepted,
             {0x200, 0x200, 0x3000, 0x200, 0x600, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0, 0x400, 0)}}},
            {"size_unaligned",
             accepted,
             {0x1000, 0x200, 0x2800, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x800, 0x400, 0x200)}}},
            {"tail4k",
             accepted,
             {0x1000, 0x200, 0x8000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x2800, 0x400, 0x200)}}},
            {"short4k",
             rejected,
             {0x1000, 0x200, 0x4000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x2800, 0x400, 0x200)}}},
            {"bighead4k",
             accepted,
             {0x1000,
              0x200,
              0x6000,
              0x1400,
              0x0,
              {text_section(0x2000, 0x10, 0x1400, 0x200), data_section(0x3000, 0x2800, 0x1600, 0x200)}}},
            {"headgap64k",
             rejected,
             {0x10000,
              0x200,
              0x50000,
              0x200,
              0x0,
              {text_section(0x30000, 0x10, 0x200, 0x200), data_section(0x40000, 0x2800, 0x400, 0x200)}}},
            {"lowalign_bss",
             rejected,
             {0x200, 0x200, 0x2000, 0x200, 0x0, {text_section(0x200, 0x10, 0x200, 0x200), data_section(0x400, 0x1800, 0x400, 0x200)}}},
            {"vsize0_raw0",
             rejected,
             {0x1000, 0x200, 0x3000, 0x200, 0x0, {text_section(0x1000, 0x10, 0x200, 0x200), data_section(0x2000, 0x0, 0x0, 0x0)}}},
            {"align64k_size_unaligned",
             rejected,
             {0x10000, 0x200, 0x28000, 0x200, 0x0, {text_section(0x10000, 0x10, 0x200, 0x200), data_section(0x20000, 0x10, 0x400, 0x200)}}},
        };

        std::ranges::transform(k_sub_page_cases, std::back_inserter(rows), sub_page_row);
        std::ranges::transform(k_raw_data_cases, std::back_inserter(rows), raw_data_row);

        for (const auto& row : rows)
        {
            SCOPED_TRACE(row.name);
            expect_layout<std::uint32_t>(row);
            expect_layout<std::uint64_t>(row);
        }
    }
}

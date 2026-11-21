#include "std_include.hpp"

#include "../windows-emulator/smbios.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, uint32_t info_class, uint64_t system_information,
                                             uint32_t system_information_length, emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t raw_smbios_provider = 0x52534D42;

        struct firmware_table_information
        {
            uint32_t provider_signature;
            uint32_t action;
            uint32_t table_id;
            uint32_t table_buffer_length;
        };

        struct smbios_structure
        {
            uint8_t type{};
            uint8_t length{};
            uint16_t handle{};
            size_t string_count{};
        };

        // Walks a DSP0134 structure table: formatted area, then strings ending in a double NUL.
        std::vector<smbios_structure> parse_structures(const std::span<const uint8_t> table)
        {
            std::vector<smbios_structure> structures{};
            size_t offset = 0;
            while (offset + 4 <= table.size())
            {
                smbios_structure structure{.type = table[offset], .length = table[offset + 1]};
                std::memcpy(&structure.handle, table.data() + offset + 2, sizeof(structure.handle));
                if (structure.length < 4 || offset + structure.length + 2 > table.size())
                {
                    return {};
                }

                auto strings = offset + structure.length;
                if (table[strings] == 0)
                {
                    strings += 2;
                }
                else
                {
                    while (table[strings] != 0)
                    {
                        strings += std::strlen(reinterpret_cast<const char*>(table.data() + strings)) + 1;
                        ++structure.string_count;
                        if (strings >= table.size())
                        {
                            return {};
                        }
                    }
                    ++strings;
                }

                structures.push_back(structure);
                offset = strings;
                if (structure.type == 127)
                {
                    break;
                }
            }
            return offset == table.size() ? structures : std::vector<smbios_structure>{};
        }

        class FirmwareTableTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
            }

            NTSTATUS query(const firmware_table_information& request, const uint32_t length)
            {
                this->emu->memory.write_memory(this->buffer(), &request, sizeof(request));
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtQuerySystemInformation(context, SystemFirmwareTableInformation, this->buffer(), length,
                                                                 {this->emu->memory, this->storage});
            }

            uint64_t buffer() const
            {
                return this->storage + 0x100;
            }

            uint32_t returned_length() const
            {
                return this->emu->memory.read_memory<uint32_t>(this->storage);
            }

            firmware_table_information header() const
            {
                return this->emu->memory.read_memory<firmware_table_information>(this->buffer());
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    // The status and length contract ntoskrnl follows for the raw SMBIOS provider.
    TEST_F(FirmwareTableTest, RawSmbiosQueryFollowsTheNativeBufferContract)
    {
        const auto table_size = static_cast<uint32_t>(smbios::raw_smbios_data().size());
        const firmware_table_information request{
            .provider_signature = raw_smbios_provider, .action = 1, .table_id = 0, .table_buffer_length = 0xAAAA};

        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(request, 8));
        EXPECT_EQ(16u, this->returned_length());
        EXPECT_EQ(0xAAAAu, this->header().table_buffer_length);

        EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(request, 16));
        EXPECT_EQ(16 + table_size, this->returned_length());
        EXPECT_EQ(table_size, this->header().table_buffer_length);

        EXPECT_EQ(STATUS_SUCCESS,
                  this->query({.provider_signature = raw_smbios_provider, .action = 1, .table_id = 5, .table_buffer_length = 0}, 0x1000));
        EXPECT_EQ(16 + table_size, this->returned_length());
        std::vector<uint8_t> table(table_size);
        this->emu->memory.read_memory(this->buffer() + 16, table.data(), table.size());
        EXPECT_EQ(smbios::raw_smbios_data(), table);
    }

    TEST_F(FirmwareTableTest, RawSmbiosEnumeratesItsSingleTable)
    {
        EXPECT_EQ(STATUS_SUCCESS,
                  this->query({.provider_signature = raw_smbios_provider, .action = 0, .table_id = 0, .table_buffer_length = 0}, 0x100));
        EXPECT_EQ(20u, this->returned_length());
        EXPECT_EQ(4u, this->header().table_buffer_length);
        EXPECT_EQ(0u, this->emu->memory.read_memory<uint32_t>(this->buffer() + 16));
    }

    TEST_F(FirmwareTableTest, UnknownProvidersAndActionsAreRejected)
    {
        this->emu->memory.write_memory(this->storage, 0xBBBBu);
        EXPECT_EQ(STATUS_NOT_IMPLEMENTED,
                  this->query({.provider_signature = 0x41424344, .action = 1, .table_id = 0, .table_buffer_length = 0}, 0x100));
        EXPECT_EQ(0u, this->returned_length());

        this->emu->memory.write_memory(this->storage, 0xBBBBu);
        EXPECT_EQ(STATUS_INVALID_PARAMETER_1,
                  this->query({.provider_signature = raw_smbios_provider, .action = 2, .table_id = 0, .table_buffer_length = 0}, 0x100));
        EXPECT_EQ(0x100u, this->returned_length());
    }

    // RawSMBIOSData: calling method, SMBIOS major and minor version, DMI revision, table length, the DSP0134 table.
    TEST(SmbiosTest, RawDataIsAWellFormedSmbios3StructureTable)
    {
        const auto& raw = smbios::raw_smbios_data();
        ASSERT_GE(raw.size(), 8u);
        EXPECT_EQ(3, raw[1]);
        uint32_t length{};
        std::memcpy(&length, raw.data() + 4, sizeof(length));
        ASSERT_EQ(raw.size() - 8, length);

        const auto structures = parse_structures(std::span{raw}.subspan(8));
        ASSERT_FALSE(structures.empty());
        EXPECT_EQ(127, structures.back().type);

        std::set<uint16_t> handles{};
        std::map<uint8_t, smbios_structure> by_type{};
        for (const auto& structure : structures)
        {
            EXPECT_TRUE(handles.insert(structure.handle).second);
            by_type.emplace(structure.type, structure);
        }

        // The minimum formatted lengths of the BIOS, system, baseboard and chassis structures DSP0134 defines.
        for (const auto& [type, minimum] : std::initializer_list<std::pair<uint8_t, uint8_t>>{{0, 0x1A}, {1, 0x1B}, {2, 0x08}, {3, 0x09}})
        {
            ASSERT_TRUE(by_type.contains(type)) << static_cast<int>(type);
            EXPECT_GE(by_type[type].length, minimum) << static_cast<int>(type);
        }
    }
}

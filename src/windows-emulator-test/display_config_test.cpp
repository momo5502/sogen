#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtUserQueryDisplayConfig(const syscall_context& c, UINT32 flags, emulator_object<UINT32> num_path_array_elements,
                                             emulator_pointer path_array, emulator_object<UINT32> current_topology_id,
                                             emulator_pointer reserved);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint8_t sentinel = 0xab;
        constexpr size_t internal_path_size = 200;
        constexpr uint32_t qdc_only_active_paths = 2;
        constexpr uint32_t qdc_database_current = 4;
        constexpr uint32_t error_noaccess = 998;

        class DisplayConfigTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                // The failed probes set the calling thread's last error.
                this->emu.setup_process_if_necessary();

                // DISPLAYCONFIG_PATH_INFO_INTERNAL is 200 bytes on Windows 10 19041-19045, whatever the emulation root is.
                this->emu.version.set_windows_build_number(19045);

                this->paths = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->count = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->set_last_error(0);
            }

            NTSTATUS query(const uint32_t flags, const uint64_t path_array, const uint64_t topology_id = 0)
            {
                return syscalls::handle_NtUserQueryDisplayConfig(this->context, flags, {this->emu.memory, this->count}, path_array,
                                                                 {this->emu.memory, topology_id}, 0);
            }

            uint32_t last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            void set_last_error(const uint32_t error)
            {
                this->emu.vcpu(0).active_thread->teb64->access([&](TEB64& teb) { teb.LastErrorValue = error; });
            }

            windows_emulator emu = create_sample_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t paths{};
            uint64_t count{};
        };
    }

    TEST_F(DisplayConfigTest, WritesOneInternalPathOfTheBuildsSize)
    {
        this->emu.memory.set_memory(this->paths, sentinel, 0x1000);
        this->emu.memory.write_memory(this->count, uint32_t{1});

        ASSERT_EQ(STATUS_SUCCESS, this->query(qdc_only_active_paths, this->paths));
        EXPECT_EQ(1u, this->emu.memory.read_memory<uint32_t>(this->count));
        EXPECT_EQ(this->emu.process.display_width, this->emu.memory.read_memory<uint32_t>(this->paths + 0x7C));
        EXPECT_EQ(this->emu.process.display_height, this->emu.memory.read_memory<uint32_t>(this->paths + 0x80));

        std::array<uint8_t, 0x40> tail{};
        this->emu.memory.read_memory(this->paths + internal_path_size, tail.data(), tail.size());
        for (const auto value : tail)
        {
            EXPECT_EQ(sentinel, value);
        }
    }

    // win32kbase probes the whole declared path array for write before it writes any buffer, a null array included. Its
    // exception handler then sets the last error and fails the call with STATUS_INVALID_PARAMETER (19041 and 26100).
    TEST_F(DisplayConfigTest, PathArrayThatCannotBeWrittenFailsTheCall)
    {
        const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
        const auto unmapped = this->emu.memory.find_free_allocation_base(0x10000);
        ASSERT_NE(0u, unmapped);

        for (const uint32_t build : {19045u, 26100u})
        {
            this->emu.version.set_windows_build_number(build);
            for (const auto array : {read_only, unmapped, uint64_t{0}})
            {
                this->set_last_error(0);
                this->emu.memory.write_memory(this->count, uint32_t{3});

                EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(qdc_only_active_paths, array)) << build << std::hex << " " << array;
                EXPECT_EQ(error_noaccess, this->last_error()) << build << std::hex << " " << array;
                EXPECT_EQ(3u, this->emu.memory.read_memory<uint32_t>(this->count));
            }
        }
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint64_t>(read_only));
    }

    TEST_F(DisplayConfigTest, CountOfZeroFailsWithNothingWritten)
    {
        this->emu.memory.set_memory(this->paths, sentinel, 0x1000);
        this->emu.memory.write_memory(this->count, uint32_t{0});

        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(qdc_only_active_paths, this->paths));
        EXPECT_EQ(0u, this->emu.memory.read_memory<uint32_t>(this->count));
        EXPECT_EQ(sentinel, this->emu.memory.read_memory<uint8_t>(this->paths));
        EXPECT_EQ(0u, this->last_error());
    }

    TEST_F(DisplayConfigTest, OnlyTheCurrentDatabaseQueryReportsATopology)
    {
        const auto topology_id = this->count + 0x10;
        this->emu.memory.set_memory(this->paths, sentinel, 0x1000);
        this->emu.memory.write_memory(this->count, uint32_t{1});
        this->emu.memory.write_memory(topology_id, uint32_t{0xFFFFFFFF});

        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(qdc_only_active_paths, this->paths, topology_id));
        EXPECT_EQ(0xFFFFFFFFu, this->emu.memory.read_memory<uint32_t>(topology_id));
        EXPECT_EQ(sentinel, this->emu.memory.read_memory<uint8_t>(this->paths));
        EXPECT_EQ(0u, this->last_error());

        EXPECT_EQ(STATUS_SUCCESS, this->query(qdc_database_current, this->paths, topology_id));
        EXPECT_EQ(1u, this->emu.memory.read_memory<uint32_t>(topology_id));

        // The current database query probes the topology id, so it cannot do without one.
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(qdc_database_current, this->paths));
        EXPECT_EQ(error_noaccess, this->last_error());
    }

    TEST_F(DisplayConfigTest, FlagsNeedExactlyOneQuery)
    {
        for (const uint32_t flags : {0x0u, 0x3u, 0x5u, 0x6u, 0x7u, 0x42u})
        {
            this->emu.memory.write_memory(this->count, uint32_t{5});
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(flags, this->paths)) << std::hex << flags;
            EXPECT_EQ(5u, this->emu.memory.read_memory<uint32_t>(this->count));
        }

        // QDC_VIRTUAL_MODE_AWARE and QDC_INCLUDE_HMD may come with any of them.
        EXPECT_EQ(STATUS_SUCCESS, this->query(qdc_only_active_paths | 0x30, this->paths));
    }

    // Windows 11 adds QDC_VIRTUAL_REFRESH_RATE_AWARE (0x40), which 26100's win32kbase accepts with the others.
    TEST_F(DisplayConfigTest, Windows11AcceptsVirtualRefreshRateAwareness)
    {
        this->emu.version.set_windows_build_number(26100);

        this->emu.memory.write_memory(this->count, uint32_t{1});
        EXPECT_EQ(STATUS_SUCCESS, this->query(qdc_only_active_paths | 0x40, this->paths));

        this->emu.memory.write_memory(this->count, uint32_t{1});
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query(qdc_only_active_paths | 0x80, this->paths));
    }
}

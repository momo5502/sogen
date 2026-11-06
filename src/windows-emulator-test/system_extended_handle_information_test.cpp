#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, uint32_t info_class, uint64_t system_information,
                                             uint32_t system_information_length, emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        using entry_t = SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX<EmulatorTraits<Emu64>>;

        constexpr uint8_t sentinel = 0xab;
        constexpr uint32_t header_size = 0x10;
        constexpr uint32_t minimum_size = header_size + sizeof(entry_t);

        class SystemExtendedHandleInformationTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                // Object type indices are pinned to Windows 10 19041-19045, whatever the emulation root is.
                this->emu.version.set_windows_build_number(19045);

                this->buffer = this->emu.memory.allocate_memory(0x10000, memory_permission::read_write);
                this->return_length = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            NTSTATUS query(const uint64_t address, const uint32_t length, const uint64_t length_address)
            {
                return syscalls::handle_NtQuerySystemInformation(this->context, SystemExtendedHandleInformation, address, length,
                                                                 {this->emu.memory, length_address});
            }

            void reset()
            {
                this->emu.memory.set_memory(this->buffer, sentinel, 0x10000);
                this->emu.memory.write_memory(this->return_length, 0xDEADBEEFu);
            }

            uint32_t reported_length()
            {
                return this->emu.memory.read_memory<uint32_t>(this->return_length);
            }

            uint64_t number_of_handles()
            {
                return this->emu.memory.read_memory<uint64_t>(this->buffer);
            }

            entry_t entry(const size_t index)
            {
                return this->emu.memory.read_memory<entry_t>(this->buffer + header_size + index * sizeof(entry_t));
            }

            std::optional<entry_t> find_entry(const handle h)
            {
                for (size_t index = 0; index < this->number_of_handles(); ++index)
                {
                    const auto candidate = this->entry(index);
                    if (candidate.HandleValue == static_cast<uint32_t>(h.bits))
                    {
                        return candidate;
                    }
                }

                return std::nullopt;
            }

            windows_emulator emu = create_empty_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t buffer{};
            uint64_t return_length{};
        };
    }

    TEST_F(SystemExtendedHandleInformationTest, BufferThatCannotHoldOneEntryReportsTheMinimumSize)
    {
        for (const uint32_t length : {0u, 8u, 16u, minimum_size - 1})
        {
            this->reset();
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(this->buffer, length, this->return_length));
            EXPECT_EQ(minimum_size, this->reported_length());
            EXPECT_EQ(sentinel, this->emu.memory.read_memory<uint8_t>(this->buffer));
        }
    }

    TEST_F(SystemExtendedHandleInformationTest, ProbesTheBufferBeforeTheLengthCheck)
    {
        this->reset();
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->query(this->buffer + 2, 16, this->return_length));
        EXPECT_EQ(0xDEADBEEFu, this->reported_length());

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query(0x12340000, 16, this->return_length));
        EXPECT_EQ(0xDEADBEEFu, this->reported_length());

        const auto read_only = this->emu.memory.allocate_memory(0x1000, memory_permission::read);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query(read_only, 16, this->return_length));
        EXPECT_EQ(0xDEADBEEFu, this->reported_length());

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query(this->buffer, 0x1000, 0x12340000));
        EXPECT_EQ(sentinel, this->emu.memory.read_memory<uint8_t>(this->buffer));
    }

    TEST_F(SystemExtendedHandleInformationTest, RequiresPointerAlignmentOnceTheBufferHoldsAnEntry)
    {
        this->reset();
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->query(this->buffer + 4, 0x1000, this->return_length));
        EXPECT_EQ(0xDEADBEEFu, this->reported_length());
        EXPECT_EQ(sentinel, this->emu.memory.read_memory<uint8_t>(this->buffer + 4));
    }

    TEST_F(SystemExtendedHandleInformationTest, ShortBufferReportsEveryHandleAndKeepsReservedFields)
    {
        this->emu.process.events.store(event{});
        this->emu.process.mutants.store(mutant{});

        this->reset();
        EXPECT_EQ(STATUS_SUCCESS, this->query(this->buffer, 0x10000, this->return_length));
        const auto total = this->number_of_handles();
        ASSERT_GE(total, 2u);
        EXPECT_EQ(header_size + total * sizeof(entry_t), this->reported_length());
        const auto first = this->entry(0);

        this->reset();
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->query(this->buffer, minimum_size, this->return_length));
        EXPECT_EQ(header_size + total * sizeof(entry_t), this->reported_length());
        EXPECT_EQ(total, this->number_of_handles());
        EXPECT_EQ(0xababababababababULL, this->emu.memory.read_memory<uint64_t>(this->buffer + 8));

        const auto written = this->entry(0);
        EXPECT_EQ(first.HandleValue, written.HandleValue);
        EXPECT_EQ(first.Object, written.Object);
        EXPECT_EQ(0xababababu, written.Reserved);
        EXPECT_EQ(sentinel, this->emu.memory.read_memory<uint8_t>(this->buffer + minimum_size));
    }

    TEST_F(SystemExtendedHandleInformationTest, ReportsKernelObjectIdentityTypeAndAttributes)
    {
        event inheritable{};
        inheritable.inherit = true;
        const auto event_handle = this->emu.process.events.store(std::move(inheritable));

        mutant protected_mutant{};
        protected_mutant.protect_from_close = true;
        const auto mutant_handle = this->emu.process.mutants.store(std::move(protected_mutant));

        this->reset();
        ASSERT_EQ(STATUS_SUCCESS, this->query(this->buffer, 0x10000, this->return_length));

        const auto event_entry = this->find_entry(event_handle);
        ASSERT_TRUE(event_entry.has_value());
        EXPECT_EQ(16u, event_entry->ObjectTypeIndex);
        EXPECT_EQ(0x1F0003u, event_entry->GrantedAccess);
        EXPECT_EQ(static_cast<ULONG>(OBJ_INHERIT), event_entry->HandleAttributes);
        EXPECT_EQ(this->emu.process.process_id, event_entry->UniqueProcessId);
        EXPECT_EQ(0u, event_entry->CreatorBackTraceIndex);
        EXPECT_GE(event_entry->Object, 0xFFFF800000000000ULL);

        const auto mutant_entry = this->find_entry(mutant_handle);
        ASSERT_TRUE(mutant_entry.has_value());
        EXPECT_EQ(17u, mutant_entry->ObjectTypeIndex);
        EXPECT_EQ(0x1F0001u, mutant_entry->GrantedAccess);
        EXPECT_EQ(static_cast<ULONG>(OBJ_PROTECT_CLOSE), mutant_entry->HandleAttributes);
        EXPECT_NE(event_entry->Object, mutant_entry->Object);

        for (size_t index = 1; index < this->number_of_handles(); ++index)
        {
            EXPECT_LT(this->entry(index - 1).HandleValue, this->entry(index).HandleValue);
        }

        this->reset();
        ASSERT_EQ(STATUS_SUCCESS, this->query(this->buffer, 0x10000, this->return_length));
        EXPECT_EQ(event_entry->Object, this->find_entry(event_handle)->Object);
    }

    // Window stations, the fixed interactive one NtUserGetProcessWindowStation returns as handle 1 included, and debug
    // objects are executive objects too. Their type indices and full access as Windows 10 19045 reports them for
    // GetProcessWindowStation's handle and NtCreateDebugObject(DEBUG_ALL_ACCESS).
    TEST_F(SystemExtendedHandleInformationTest, ReportsWindowStationAndDebugObjectHandles)
    {
        const auto station_handle = this->emu.process.window_stations.store(window_station{});
        const auto debug_handle = this->emu.process.debug_objects.store(debug_object{});
        ASSERT_EQ(1u, this->emu.process.process_window_station);

        this->reset();
        ASSERT_EQ(STATUS_SUCCESS, this->query(this->buffer, 0x10000, this->return_length));

        for (const auto h : {station_handle, make_handle(this->emu.process.process_window_station)})
        {
            const auto station_entry = this->find_entry(h);
            ASSERT_TRUE(station_entry.has_value()) << h.bits;
            EXPECT_EQ(24u, station_entry->ObjectTypeIndex);
            EXPECT_EQ(0xF037Fu, station_entry->GrantedAccess);
            EXPECT_EQ(0u, station_entry->HandleAttributes);
        }

        const auto debug_entry = this->find_entry(debug_handle);
        ASSERT_TRUE(debug_entry.has_value());
        EXPECT_EQ(15u, debug_entry->ObjectTypeIndex);
        EXPECT_EQ(0x1F000Fu, debug_entry->GrantedAccess);
    }
}

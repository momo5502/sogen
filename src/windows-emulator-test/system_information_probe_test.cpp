#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, uint32_t info_class, uint64_t system_information,
                                             uint32_t system_information_length, emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, uint32_t info_class, uint64_t input_buffer,
                                               uint32_t input_buffer_length, uint64_t system_information,
                                               uint32_t system_information_length, emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t page_size = 0x1000;
        constexpr uint8_t sentinel = 0xab;
        constexpr uint32_t unwritten = 0xDEADBEEF;
        constexpr uint64_t unmapped = 0x12340000;
        constexpr uint32_t past_the_last_class = 0x1FF;
        constexpr auto basic_size = static_cast<uint32_t>(sizeof(SYSTEM_BASIC_INFORMATION64));
        constexpr auto time_of_day_size = static_cast<uint32_t>(sizeof(SYSTEM_TIMEOFDAY_INFORMATION64));
        constexpr auto kernel_debugger_size = static_cast<uint32_t>(sizeof(SYSTEM_KERNEL_DEBUGGER_INFORMATION));
        constexpr uint64_t user_probe_address = 0x7FFFFFFF0000; // MM_USER_PROBE_ADDRESS

        class SystemInformationProbeTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->buffer = this->page(memory_permission::read_write);
                this->return_length = this->page(memory_permission::read_write);
                this->emu.memory.write_memory(this->return_length, unwritten);
            }

            NTSTATUS query(const uint32_t info_class, const uint64_t address, const uint32_t length, const uint64_t length_address)
            {
                return syscalls::handle_NtQuerySystemInformation(this->context, info_class, address, length,
                                                                 {this->emu.memory, length_address});
            }

            NTSTATUS query_ex(const uint32_t info_class, const uint64_t input, const uint32_t input_length, const uint64_t address,
                              const uint32_t length)
            {
                return syscalls::handle_NtQuerySystemInformationEx(this->context, info_class, input, input_length, address, length,
                                                                   {this->emu.memory, this->return_length});
            }

            uint64_t page(const memory_permission permission)
            {
                const auto address = this->emu.memory.allocate_memory(page_size, memory_permission::read_write);
                this->emu.memory.set_memory(address, sentinel, page_size);
                EXPECT_TRUE(this->emu.memory.protect_memory(address, page_size, permission));
                return address;
            }

            uint64_t guarded_page()
            {
                const auto address = this->page(memory_permission::read_write);
                EXPECT_TRUE(this->emu.memory.protect_memory(
                    address, page_size, nt_memory_permission(memory_permission::read_write, memory_permission_ext::guard)));
                return address;
            }

            windows_emulator emu = create_empty_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t buffer{};
            uint64_t return_length{};
        };
    }

    // ExpQuerySystemInformation probes a user caller's buffers for every class: a guard page under the buffer or the returned
    // length loses its guard and fails the query with STATUS_GUARD_PAGE_VIOLATION before anything is written, and the next
    // query finds an ordinary page (19041 on the host).
    TEST_F(SystemInformationProbeTest, AGuardPageFailsAQueryOfAnyClassWithItsGuard)
    {
        const auto guarded = this->guarded_page();
        EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, this->query(SystemBasicInformation, guarded, basic_size, this->return_length));
        EXPECT_EQ(unwritten, this->emu.memory.read_memory<uint32_t>(this->return_length));
        EXPECT_FALSE(this->emu.memory.is_guard_page(guarded));
        EXPECT_EQ(STATUS_SUCCESS, this->query(SystemBasicInformation, guarded, basic_size, this->return_length));

        const auto guarded_length = this->guarded_page();
        EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, this->query(SystemTimeOfDayInformation, this->buffer, time_of_day_size, guarded_length));
        EXPECT_EQ(sentinel, this->emu.memory.read_memory<uint8_t>(this->buffer));
        EXPECT_FALSE(this->emu.memory.is_guard_page(guarded_length));
        EXPECT_EQ(STATUS_SUCCESS, this->query(SystemTimeOfDayInformation, this->buffer, time_of_day_size, guarded_length));
    }

    // The buffer is probed at its class's alignment, 8 for SystemLocksInformation, 1 for SystemKernelDebuggerInformation
    // and the other classes of single bytes, 4 for the rest, and for writing; an empty buffer is not probed, and the
    // returned length has no alignment of its own (19041 on the host).
    TEST_F(SystemInformationProbeTest, ProbesTheBufferAtItsClasssAlignmentForWriting)
    {
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->query(SystemBasicInformation, this->buffer + 2, basic_size, 0));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->query(SystemLocksInformation, this->buffer + 4, basic_size, 0));
        EXPECT_EQ(STATUS_SUCCESS, this->query(SystemKernelDebuggerInformation, this->buffer + 1, kernel_debugger_size, 0));

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query(SystemBasicInformation, this->page(memory_permission::read), basic_size, 0));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query(SystemBasicInformation, unmapped, basic_size, 0));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                  this->query(SystemBasicInformation, this->buffer, basic_size, this->page(memory_permission::read)));
        EXPECT_EQ(STATUS_SUCCESS, this->query(SystemBasicInformation, this->buffer, basic_size, this->buffer + page_size - 7));

        const auto empty = this->query(SystemBasicInformation, 1, 0, 0);
        EXPECT_NE(STATUS_DATATYPE_MISALIGNMENT, empty);
        EXPECT_NE(STATUS_ACCESS_VIOLATION, empty);
    }

    // The returned length is read in full before it is written back: one that runs from a read-only page into a guard page
    // fails on the guard, which it loses, and then on the write to the read-only page (19041).
    TEST_F(SystemInformationProbeTest, ReadsAllOfTheReturnedLengthBeforeWritingItBack)
    {
        const auto pages = this->emu.memory.allocate_memory(2 * page_size, memory_permission::read_write);
        this->emu.memory.set_memory(pages, sentinel, 2 * page_size);
        ASSERT_TRUE(this->emu.memory.protect_memory(pages, page_size, memory_permission::read));
        ASSERT_TRUE(this->emu.memory.protect_memory(pages + page_size, page_size,
                                                    nt_memory_permission(memory_permission::read_write, memory_permission_ext::guard)));
        const auto length = pages + page_size - sizeof(ULONG) / 2;

        EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, this->query(SystemTimeOfDayInformation, this->buffer, time_of_day_size, length));
        EXPECT_FALSE(this->emu.memory.is_guard_page(pages + page_size));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query(SystemTimeOfDayInformation, this->buffer, time_of_day_size, length));
        EXPECT_EQ(sentinel, this->emu.memory.read_memory<uint8_t>(this->buffer));
    }

    // The probe comes before the class is looked at, so a class past the last one fails on a bad buffer's probe first
    // (19041 on the host).
    TEST_F(SystemInformationProbeTest, ProbesTheBufferBeforeLookingAtTheClass)
    {
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query(past_the_last_class, unmapped, basic_size, 0));
        EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query(past_the_last_class, this->buffer, basic_size, 0));
    }

    // NtQuerySystemInformation turns the classes that take an input away before ExpQuerySystemInformation, so none of
    // their buffers is probed: a guard page under the buffer keeps its guard (19041).
    TEST_F(SystemInformationProbeTest, ClassesThatTakeAnInputFailBeforeTheProbe)
    {
        const auto guarded = this->guarded_page();
        for (const auto info_class :
             {SystemLogicalProcessorAndGroupInformation, SystemNodeDistanceInformation, SystemInterruptSteeringInformation,
              SystemFeatureConfigurationInformation, SystemFeatureConfigurationSectionInformation})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query(info_class, guarded, basic_size, this->return_length)) << info_class;
        }
        EXPECT_TRUE(this->emu.memory.is_guard_page(guarded));
        EXPECT_EQ(unwritten, this->emu.memory.read_memory<uint32_t>(this->return_length));
    }

    // NtQuerySystemInformationEx checks its input and class before ExpQuerySystemInformation's probe: an input, a class it
    // takes, then the input's alignment for that class and its range. A call these turn away probes no buffer (19041).
    TEST_F(SystemInformationProbeTest, ExChecksItsInputAndClassBeforeTheProbe)
    {
        const auto guarded = this->guarded_page();
        const auto input = this->buffer;
        constexpr auto relationship = static_cast<uint32_t>(sizeof(LOGICAL_PROCESSOR_RELATIONSHIP));
        EXPECT_EQ(STATUS_INVALID_PARAMETER,
                  this->query_ex(SystemLogicalProcessorAndGroupInformation, 0, relationship, guarded, basic_size));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->query_ex(SystemLogicalProcessorAndGroupInformation, input, 0, guarded, basic_size));
        EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_ex(SystemBasicInformation, input, relationship, guarded, basic_size));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT,
                  this->query_ex(SystemLogicalProcessorAndGroupInformation, input + sizeof(USHORT), relationship, guarded, basic_size));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_ex(SystemLogicalProcessorAndGroupInformation, user_probe_address - relationship,
                                                          2 * relationship, guarded, basic_size));
        EXPECT_TRUE(this->emu.memory.is_guard_page(guarded));
        EXPECT_EQ(unwritten, this->emu.memory.read_memory<uint32_t>(this->return_length));
    }
}

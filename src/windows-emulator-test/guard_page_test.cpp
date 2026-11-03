#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <array>
#include <span>
#include <gtest/gtest.h>

namespace sogen::syscalls
{
    NTSTATUS handle_NtAllocateVirtualMemoryEx(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                              emulator_object<uint64_t> bytes_to_allocate, uint32_t allocation_type,
                                              uint32_t page_protection, emulator_object<MEM_EXTENDED_PARAMETER64> extended_parameters,
                                              ULONG extended_parameter_count);
    NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                           emulator_object<uint32_t> bytes_to_protect, uint32_t protection,
                                           emulator_object<uint32_t> old_protection);
    NTSTATUS handle_NtQueryVirtualMemory(const syscall_context& c, handle process_handle, uint64_t base_address, uint32_t info_class,
                                         uint64_t memory_information, uint64_t memory_information_length,
                                         emulator_object<uint64_t> return_length);
    NTSTATUS handle_NtReadVirtualMemory(const syscall_context& c, handle process, emulator_pointer address, emulator_pointer buffer,
                                        uint64_t size, emulator_object<uint64_t> transferred);
    NTSTATUS handle_NtWriteVirtualMemory(const syscall_context& c, handle process, emulator_pointer address, emulator_pointer buffer,
                                         uint64_t size, emulator_object<uint64_t> transferred);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t page_size = 0x1000;
        constexpr uint32_t untouched = 0xFFFFFFFF;

        struct allocation_result
        {
            NTSTATUS status;
            uint64_t base;
        };

        struct protect_result
        {
            NTSTATUS status;
            uint32_t old_protection;
        };

        class GuardPageTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->create_emulator(create_x86_64_emulator_from_environment(), {});
            }

            void create_emulator(std::unique_ptr<x86_64_emulator> backend, emulator_callbacks callbacks)
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    std::move(backend),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    std::move(callbacks), std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(page_size, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            allocation_result allocate(const uint64_t address, const uint64_t size, const uint32_t type, const uint32_t protection)
            {
                const emulator_object<uint64_t> base{this->emu->memory, this->storage};
                const emulator_object<uint64_t> bytes{this->emu->memory, this->storage + 8};
                base.write(address);
                bytes.write(size);
                const auto status = syscalls::handle_NtAllocateVirtualMemoryEx(this->context(), CURRENT_PROCESS, base, bytes, type,
                                                                               protection, {this->emu->memory}, 0);
                return {.status = status, .base = base.read()};
            }

            protect_result protect(const uint64_t address, const uint32_t size, const uint32_t protection)
            {
                const emulator_object<uint64_t> base{this->emu->memory, this->storage};
                const emulator_object<uint32_t> bytes{this->emu->memory, this->storage + 8};
                const emulator_object<uint32_t> old_protection{this->emu->memory, this->storage + 0x10};
                base.write(address);
                bytes.write(size);
                old_protection.write(untouched);
                const auto status =
                    syscalls::handle_NtProtectVirtualMemory(this->context(), CURRENT_PROCESS, base, bytes, protection, old_protection);
                return {.status = status, .old_protection = old_protection.read()};
            }

            EMU_MEMORY_BASIC_INFORMATION64 query(const uint64_t address)
            {
                const auto information = this->storage + 0x100;
                EXPECT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtQueryVirtualMemory(this->context(), CURRENT_PROCESS, address, MemoryBasicInformation,
                                                                information, sizeof(EMU_MEMORY_BASIC_INFORMATION64), {this->emu->memory}));
                return this->emu->memory.read_memory<EMU_MEMORY_BASIC_INFORMATION64>(information);
            }

            uint64_t committed(const uint64_t size, const uint32_t protection)
            {
                const auto result = this->allocate(0, size, MEM_RESERVE | MEM_COMMIT, protection);
                EXPECT_EQ(STATUS_SUCCESS, result.status);
                return result.base;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };

        class GuardPageFaultTest : public GuardPageTest
        {
          protected:
            void SetUp() override
            {
                auto backend = create_x86_64_emulator_from_environment();
                auto* hook = backend->hook_instruction(x86_hookable_instructions::rdtsc,
                                                       [] { return instruction_hook_continuation::run_instruction; });
                if (!hook)
                {
                    GTEST_SKIP() << "Backend does not support timestamp instruction hooks";
                }
                backend->delete_hook(hook);

                this->cpu = backend.get();
                emulator_callbacks callbacks{};
                callbacks.on_rdtsc = [this] { this->cpu->stop(); };
                this->create_emulator(std::move(backend), std::move(callbacks));

                this->thread = std::make_unique<emulator_thread>(this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true);
                this->emu->vcpu(0).active_thread = this->thread.get();

                this->dispatcher = this->emu->memory.allocate_memory(page_size, memory_permission::all);
                this->emu->memory.write_memory(this->dispatcher, rdtsc.data(), rdtsc.size());
                this->emu->process.ki_user_exception_dispatcher = this->dispatcher;
                this->stack = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->code = this->emu->memory.allocate_memory(page_size, memory_permission::all);
            }

            EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>> run(const std::span<const uint8_t> instruction, const uint64_t rcx)
            {
                this->emu->memory.write_memory(this->code, instruction.data(), instruction.size());
                this->emu->memory.write_memory(this->code + instruction.size(), rdtsc.data(), rdtsc.size());

                this->cpu->reg(x86_register::rip, this->code);
                this->cpu->reg(x86_register::rsp, this->initial_stack_pointer());
                this->cpu->reg(x86_register::rcx, rcx);
                do
                {
                    this->cpu->start();
                } while (this->cpu->has_violation());

                constexpr uint64_t context_record_size = 0x4F0;
                return this->emu->memory.read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(this->cpu->reg(x86_register::rsp) +
                                                                                                  context_record_size);
            }

            bool runs_without_exception(const std::span<const uint8_t> instruction, const uint64_t rcx)
            {
                this->run(instruction, rcx);
                return this->cpu->reg(x86_register::rsp) == this->initial_stack_pointer();
            }

            uint64_t initial_stack_pointer() const
            {
                return this->stack + 0x8000;
            }

            static constexpr std::array<uint8_t, 2> rdtsc{0x0F, 0x31};

            x86_64_emulator* cpu{};
            std::unique_ptr<emulator_thread> thread{};
            uint64_t dispatcher{};
            uint64_t stack{};
            uint64_t code{};
        };

        class GuardPageExecuteFaultTest : public GuardPageFaultTest
        {
          protected:
            void SetUp() override
            {
                if (get_x86_64_emulator_backend_from_environment() == backend_type::kvm)
                {
                    GTEST_SKIP() << "KVM reports instruction fetches as reads";
                }

                GuardPageFaultTest::SetUp();
            }
        };

        void expect_access_violation(const EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>& record, const uint64_t access_type,
                                     const uint64_t address)
        {
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, static_cast<NTSTATUS>(record.ExceptionCode));
            ASSERT_EQ(2u, record.NumberParameters);
            EXPECT_EQ(access_type, record.ExceptionInformation[0]);
            EXPECT_EQ(address, record.ExceptionInformation[1]);
        }

        void expect_guard_violation(const EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>& record, const uint64_t access_type,
                                    const uint64_t address)
        {
            EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, static_cast<NTSTATUS>(record.ExceptionCode));
            ASSERT_EQ(2u, record.NumberParameters);
            EXPECT_EQ(access_type, record.ExceptionInformation[0]);
            EXPECT_EQ(address, record.ExceptionInformation[1]);
        }
    }

    TEST_F(GuardPageTest, GuardAloneAndGuardWithNoAccessAreInvalidProtections)
    {
        constexpr std::array<uint32_t, 2> invalid_protections{PAGE_GUARD, PAGE_GUARD | PAGE_NOACCESS};
        for (const auto protection : invalid_protections)
        {
            EXPECT_EQ(STATUS_INVALID_PAGE_PROTECTION, this->allocate(0, page_size, MEM_RESERVE | MEM_COMMIT, protection).status);
            EXPECT_EQ(STATUS_INVALID_PAGE_PROTECTION, this->allocate(0, page_size, MEM_RESERVE, protection).status);

            const auto page = this->committed(page_size, PAGE_READWRITE);
            EXPECT_EQ(STATUS_INVALID_PAGE_PROTECTION, this->protect(page, page_size, protection).status);
            EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), this->query(page).Protect);
        }
    }

    TEST_F(GuardPageTest, QueriesReportTheGuardOfAllocatedAndProtectedPages)
    {
        constexpr std::array<uint32_t, 4> guarded_protections{PAGE_READONLY | PAGE_GUARD, PAGE_READWRITE | PAGE_GUARD,
                                                              PAGE_EXECUTE_READ | PAGE_GUARD, PAGE_EXECUTE_READWRITE | PAGE_GUARD};
        for (const auto protection : guarded_protections)
        {
            const auto allocated = this->query(this->committed(page_size, protection));
            EXPECT_EQ(protection, allocated.Protect);
            EXPECT_EQ(protection, allocated.AllocationProtect);

            const auto reserved = this->allocate(0, page_size, MEM_RESERVE, protection);
            ASSERT_EQ(STATUS_SUCCESS, reserved.status);
            EXPECT_EQ(protection, this->query(reserved.base).AllocationProtect);

            const auto page = this->committed(page_size, PAGE_READWRITE);
            const auto result = this->protect(page, page_size, protection);
            EXPECT_EQ(STATUS_SUCCESS, result.status);
            EXPECT_EQ(static_cast<uint32_t>(PAGE_READWRITE), result.old_protection);
            EXPECT_EQ(protection, this->query(page).Protect);
        }
    }

    TEST_F(GuardPageTest, OldProtectionOfAGuardPageIncludesTheGuard)
    {
        const auto base = this->committed(2 * page_size, PAGE_READWRITE | PAGE_GUARD);
        EXPECT_EQ(static_cast<uint32_t>(PAGE_READWRITE | PAGE_GUARD),
                  this->protect(base, page_size, PAGE_READWRITE | PAGE_GUARD).old_protection);
        EXPECT_EQ(static_cast<uint32_t>(PAGE_READWRITE | PAGE_GUARD), this->protect(base, 2 * page_size, PAGE_READWRITE).old_protection);
    }

    TEST_F(GuardPageTest, AllocationProtectKeepsTheGuardAfterACommit)
    {
        const auto reserved = this->allocate(0, 2 * page_size, MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD);
        ASSERT_EQ(STATUS_SUCCESS, reserved.status);
        ASSERT_EQ(STATUS_SUCCESS, this->allocate(reserved.base, page_size, MEM_COMMIT, PAGE_READWRITE).status);

        const auto committed_page = this->query(reserved.base);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), committed_page.Protect);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), committed_page.AllocationProtect);

        const auto reserved_page = this->query(reserved.base + page_size);
        EXPECT_EQ(static_cast<DWORD>(MEM_RESERVE), reserved_page.State);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), reserved_page.AllocationProtect);
    }

    TEST_F(GuardPageTest, ExecuteScanReachesAGuardPageAfterAnAccessiblePageWithoutDep)
    {
        constexpr size_t move_size = 5;
        const auto read_write = this->committed(2 * page_size, PAGE_READWRITE);
        const auto read_only = this->committed(2 * page_size, PAGE_READONLY);
        ASSERT_EQ(STATUS_SUCCESS, this->protect(read_write + page_size, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        ASSERT_EQ(STATUS_SUCCESS, this->protect(read_only + page_size, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        this->emu->memory.set_dep_enabled(false);

        EXPECT_EQ(std::optional{read_write + page_size},
                  this->emu->memory.find_guard_page_violation_address(read_write + page_size - 2, move_size, memory_permission::exec));
        EXPECT_EQ(std::optional{read_only + page_size},
                  this->emu->memory.find_guard_page_violation_address(read_only + page_size - 2, move_size, memory_permission::exec));
    }

    TEST_F(GuardPageTest, ExecuteScanStopsAtANonExecutablePageWithDep)
    {
        constexpr size_t move_size = 5;
        const auto base = this->committed(2 * page_size, PAGE_READWRITE);
        ASSERT_EQ(STATUS_SUCCESS, this->protect(base + page_size, page_size, PAGE_READWRITE | PAGE_GUARD).status);

        EXPECT_FALSE(
            this->emu->memory.find_guard_page_violation_address(base + page_size - 2, move_size, memory_permission::exec).has_value());
    }

    TEST_F(GuardPageTest, GuardScanStopsAtTheEndOfTheAddressSpace)
    {
        EXPECT_FALSE(this->emu->memory.find_guard_page_violation_address(0xFFFFFFFFFFFFF000, 1, memory_permission::read).has_value());
        EXPECT_FALSE(this->emu->memory.find_guard_page_violation_address(0xFFFFFFFFFFFFFFF8, 8, memory_permission::read).has_value());
        EXPECT_FALSE(this->emu->memory.find_guard_page_violation_address(0xFFFFFFFFFFFFFFFC, 8, memory_permission::read).has_value());
    }

    TEST_F(GuardPageTest, ReadingVirtualMemoryFromAGuardPageStopsAndKeepsTheGuard)
    {
        const auto source = this->committed(2 * page_size, PAGE_READWRITE);
        const auto destination = this->committed(page_size, PAGE_READWRITE);
        ASSERT_EQ(STATUS_SUCCESS, this->protect(source + page_size, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        this->emu->memory.write_memory(destination, uint64_t{0xABABABABABABABAB});

        const emulator_object<uint64_t> transferred{this->emu->memory, this->storage + 0x200};
        EXPECT_EQ(STATUS_PARTIAL_COPY, syscalls::handle_NtReadVirtualMemory(this->context(), CURRENT_PROCESS, source + page_size + 0x10,
                                                                            destination, 16, transferred));
        EXPECT_EQ(0u, transferred.read());
        EXPECT_EQ(0xABABABABABABABABu, this->emu->memory.read_memory<uint64_t>(destination));
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), this->query(source + page_size).Protect);
    }

    TEST_F(GuardPageTest, CopyingVirtualMemoryIntoAGuardPageStopsAndReleasesTheGuard)
    {
        const auto source = this->committed(page_size, PAGE_READWRITE);
        const auto destination = this->committed(2 * page_size, PAGE_READWRITE);
        const auto guard_page = destination + page_size;
        const emulator_object<uint64_t> transferred{this->emu->memory, this->storage + 0x200};

        ASSERT_EQ(STATUS_SUCCESS, this->protect(guard_page, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        EXPECT_EQ(STATUS_PARTIAL_COPY,
                  syscalls::handle_NtReadVirtualMemory(this->context(), CURRENT_PROCESS, source, guard_page + 0x10, 16, transferred));
        EXPECT_EQ(0u, transferred.read());
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), this->query(guard_page).Protect);

        ASSERT_EQ(STATUS_SUCCESS, this->protect(guard_page, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        EXPECT_EQ(STATUS_PARTIAL_COPY,
                  syscalls::handle_NtWriteVirtualMemory(this->context(), CURRENT_PROCESS, guard_page + 0x10, source, 16, transferred));
        EXPECT_EQ(0u, transferred.read());
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), this->query(guard_page).Protect);
    }

    TEST_F(GuardPageFaultTest, FaultReleasesOnlyTheTouchedPage)
    {
        const auto base = this->committed(4 * page_size, PAGE_READWRITE | PAGE_GUARD);
        constexpr std::array<uint8_t, 2> read_byte{0x8A, 0x01};

        expect_guard_violation(this->run(read_byte, base + page_size + 8), 0, base + page_size + 8);

        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), this->query(base).Protect);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), this->query(base + page_size).Protect);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), this->query(base + 2 * page_size).Protect);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), this->query(base + 3 * page_size).Protect);

        expect_guard_violation(this->run(read_byte, base + 8), 0, base + 8);
        expect_guard_violation(this->run(read_byte, base + 2 * page_size + 8), 0, base + 2 * page_size + 8);
        EXPECT_TRUE(this->runs_without_exception(read_byte, base + page_size + 8));
    }

    TEST_F(GuardPageFaultTest, FaultReleasesAGuardPageAwayFromTheAllocationBase)
    {
        const auto base = this->committed(4 * page_size, PAGE_READWRITE);
        const auto guard_page = base + 2 * page_size;
        ASSERT_EQ(STATUS_SUCCESS, this->protect(guard_page, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        constexpr std::array<uint8_t, 2> write_byte{0x88, 0x01};

        expect_guard_violation(this->run(write_byte, guard_page + 0x30), 1, guard_page + 0x30);

        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), this->query(guard_page).Protect);
        EXPECT_TRUE(this->runs_without_exception(write_byte, guard_page + 0x30));
    }

    TEST_F(GuardPageExecuteFaultTest, ExecutingAGuardPageReportsAnExecuteAccess)
    {
        const auto page = this->committed(page_size, PAGE_EXECUTE_READWRITE);
        this->emu->memory.write_memory(page + 0x20, rdtsc.data(), rdtsc.size());
        ASSERT_EQ(STATUS_SUCCESS, this->protect(page, page_size, PAGE_EXECUTE_READ | PAGE_GUARD).status);
        constexpr std::array<uint8_t, 2> jump_rcx{0xFF, 0xE1};

        expect_guard_violation(this->run(jump_rcx, page + 0x20), 8, page + 0x20);

        EXPECT_EQ(static_cast<DWORD>(PAGE_EXECUTE_READ), this->query(page).Protect);
    }

    TEST_F(GuardPageFaultTest, AccessStraddlingIntoAGuardPageReportsItsFirstByte)
    {
        const auto base = this->committed(2 * page_size, PAGE_READWRITE);
        const auto guard_page = base + page_size;
        ASSERT_EQ(STATUS_SUCCESS, this->protect(guard_page, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        constexpr std::array<uint8_t, 3> write_qword{0x48, 0x89, 0x01};

        expect_guard_violation(this->run(write_qword, guard_page - 4), 1, guard_page);

        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), this->query(guard_page).Protect);
    }

    TEST_F(GuardPageFaultTest, AccessStraddlingTwoGuardPagesFiresEachGuardOnce)
    {
        const auto base = this->committed(2 * page_size, PAGE_READWRITE | PAGE_GUARD);
        const auto boundary = base + page_size;
        constexpr std::array<uint8_t, 3> write_qword{0x48, 0x89, 0x01};

        expect_guard_violation(this->run(write_qword, boundary - 4), 1, boundary - 4);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), this->query(boundary).Protect);

        expect_guard_violation(this->run(write_qword, boundary - 4), 1, boundary);
        EXPECT_TRUE(this->runs_without_exception(write_qword, boundary - 4));
    }

    TEST_F(GuardPageFaultTest, AccessFromANoAccessPageIntoAGuardPageIsAnAccessViolation)
    {
        const auto base = this->committed(2 * page_size, PAGE_READWRITE);
        const auto boundary = base + page_size;
        ASSERT_EQ(STATUS_SUCCESS, this->protect(base, page_size, PAGE_NOACCESS).status);
        ASSERT_EQ(STATUS_SUCCESS, this->protect(boundary, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        constexpr std::array<uint8_t, 3> read_qword{0x48, 0x8B, 0x01};

        expect_access_violation(this->run(read_qword, boundary - 4), 0, boundary - 4);

        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), this->query(boundary).Protect);
    }

    TEST_F(GuardPageFaultTest, AccessFromAReservedPageIntoAGuardPageIsAnAccessViolation)
    {
        const auto reserved = this->allocate(0, 2 * page_size, MEM_RESERVE, PAGE_READWRITE);
        ASSERT_EQ(STATUS_SUCCESS, reserved.status);
        const auto boundary = reserved.base + page_size;
        ASSERT_EQ(STATUS_SUCCESS, this->allocate(boundary, page_size, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD).status);
        constexpr std::array<uint8_t, 3> read_qword{0x48, 0x8B, 0x01};

        expect_access_violation(this->run(read_qword, boundary - 4), 0, boundary - 4);

        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), this->query(boundary).Protect);
    }

    TEST_F(GuardPageFaultTest, WriteFromAReadOnlyPageIntoAGuardPageIsAnAccessViolation)
    {
        const auto base = this->committed(2 * page_size, PAGE_READWRITE);
        const auto boundary = base + page_size;
        ASSERT_EQ(STATUS_SUCCESS, this->protect(base, page_size, PAGE_READONLY).status);
        ASSERT_EQ(STATUS_SUCCESS, this->protect(boundary, page_size, PAGE_READWRITE | PAGE_GUARD).status);
        constexpr std::array<uint8_t, 3> write_qword{0x48, 0x89, 0x01};

        expect_access_violation(this->run(write_qword, boundary - 4), 1, boundary - 4);

        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), this->query(boundary).Protect);
    }

    TEST_F(GuardPageExecuteFaultTest, ExecutingANoAccessPageReportsAnExecuteAccessViolation)
    {
        const auto page = this->committed(page_size, PAGE_EXECUTE_READWRITE);
        this->emu->memory.write_memory(page + 0x40, rdtsc.data(), rdtsc.size());
        ASSERT_EQ(STATUS_SUCCESS, this->protect(page, page_size, PAGE_NOACCESS).status);
        constexpr std::array<uint8_t, 2> jump_rcx{0xFF, 0xE1};

        expect_access_violation(this->run(jump_rcx, page + 0x40), 8, page + 0x40);
    }

    TEST_F(GuardPageExecuteFaultTest, ExecutingAReadWritePageReportsAnExecuteAccessViolation)
    {
        const auto page = this->committed(page_size, PAGE_READWRITE);
        this->emu->memory.write_memory(page + 0x40, rdtsc.data(), rdtsc.size());
        constexpr std::array<uint8_t, 2> jump_rcx{0xFF, 0xE1};

        expect_access_violation(this->run(jump_rcx, page + 0x40), 8, page + 0x40);
    }

    TEST_F(GuardPageExecuteFaultTest, ExecutingAReleasedPageReportsAnExecuteAccessViolation)
    {
        const auto page = this->committed(page_size, PAGE_EXECUTE_READWRITE);
        ASSERT_TRUE(this->emu->memory.release_memory(page, 0));
        constexpr std::array<uint8_t, 2> jump_rcx{0xFF, 0xE1};

        expect_access_violation(this->run(jump_rcx, page + 0x40), 8, page + 0x40);
    }

    TEST_F(GuardPageFaultTest, ReadingTheTopOfTheAddressSpaceReportsAReadAccessViolation)
    {
        constexpr std::array<uint8_t, 2> read_byte{0x8A, 0x01};

        const auto record = this->run(read_byte, 0xFFFFFFFFFFFFFFF8);

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, static_cast<NTSTATUS>(record.ExceptionCode));
        EXPECT_EQ(0u, record.ExceptionInformation[0]);
    }
}

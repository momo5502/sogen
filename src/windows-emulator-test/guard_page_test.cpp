#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <array>
#include <cstring>
#include <map>
#include <span>
#include <utility>
#include <vector>
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

        class coarse_page_memory final : public memory_interface
        {
          public:
            void read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                if (!this->try_read_memory(address, data, size))
                {
                    throw std::runtime_error{"unmapped read"};
                }
            }

            bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
            {
                return this->access(address, static_cast<std::byte*>(data), size, false);
            }

            void write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                if (!this->try_write_memory(address, data, size))
                {
                    throw std::runtime_error{"unmapped write"};
                }
            }

            bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
            {
                return this->access(address, static_cast<std::byte*>(const_cast<void*>(data)), size, true);
            }

            bool supports_page_granular_protection() const override
            {
                return this->page_granular_protection;
            }

            bool supports_restartable_faults() const override
            {
                return this->restartable_faults;
            }

            bool page_granular_protection{false};
            bool restartable_faults{true};

          private:
            using page = std::array<std::byte, page_size>;

            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
                throw std::runtime_error{"no mmio"};
            }

            void map_memory(const uint64_t address, const size_t size, memory_permission) override
            {
                for (uint64_t offset = 0; offset < size; offset += page_size)
                {
                    this->pages_[address + offset] = {};
                }
            }

            void unmap_memory(const uint64_t address, const size_t size) override
            {
                for (uint64_t offset = 0; offset < size; offset += page_size)
                {
                    this->pages_.erase(address + offset);
                }
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
            }

            bool access(const uint64_t address, std::byte* data, const size_t size, const bool is_write) const
            {
                for (size_t done = 0; done < size;)
                {
                    const auto current = address + done;
                    const auto entry = this->pages_.find(current & ~(page_size - 1));
                    if (entry == this->pages_.end())
                    {
                        return false;
                    }

                    const auto offset = static_cast<size_t>(current & (page_size - 1));
                    const auto chunk = std::min(size - done, static_cast<size_t>(page_size) - offset);
                    auto* page_data = const_cast<std::byte*>(entry->second.data()) + offset;
                    if (is_write)
                    {
                        std::memcpy(page_data, data + done, chunk);
                    }
                    else
                    {
                        std::memcpy(data + done, page_data, chunk);
                    }

                    done += chunk;
                }

                return true;
            }

            std::map<uint64_t, page> pages_{};
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

            void expect_stack_layout(const uint64_t deallocation, const uint64_t limit, const uint64_t base, const uint64_t guard_size)
            {
                const auto committed_part = this->query(limit);
                EXPECT_EQ(limit, committed_part.BaseAddress);
                EXPECT_EQ(base - limit, committed_part.RegionSize);
                EXPECT_EQ(static_cast<DWORD>(MEM_COMMIT), committed_part.State);
                EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), committed_part.Protect);
                EXPECT_EQ(deallocation, committed_part.AllocationBase);
                EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), committed_part.AllocationProtect);

                const auto guard_region = this->query(limit - guard_size);
                EXPECT_EQ(limit - guard_size, guard_region.BaseAddress);
                EXPECT_EQ(guard_size, guard_region.RegionSize);
                EXPECT_EQ(static_cast<DWORD>(MEM_COMMIT), guard_region.State);
                EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), guard_region.Protect);

                const auto reserved_part = this->query(deallocation);
                EXPECT_EQ(deallocation, reserved_part.BaseAddress);
                EXPECT_EQ(limit - guard_size - deallocation, reserved_part.RegionSize);
                EXPECT_EQ(static_cast<DWORD>(MEM_RESERVE), reserved_part.State);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };

        class StackLayoutTest : public GuardPageTest
        {
          protected:
            void SetUp() override
            {
                GuardPageTest::SetUp();
                if (!this->emu->memory.supports_guarded_stack_growth())
                {
                    GTEST_SKIP() << "Backend commits thread stacks in full without guarded stack growth";
                }
            }
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
                this->initial_stack_pointer = this->stack + 0x8000;
                this->code = this->emu->memory.allocate_memory(page_size, memory_permission::all);
            }

            EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>> run(const std::span<const uint8_t> instruction, const uint64_t rcx)
            {
                this->emu->memory.write_memory(this->code, instruction.data(), instruction.size());
                this->emu->memory.write_memory(this->code + instruction.size(), rdtsc.data(), rdtsc.size());

                this->cpu->reg(x86_register::rip, this->code);
                this->cpu->reg(x86_register::rsp, this->initial_stack_pointer);
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
                return this->cpu->reg(x86_register::rsp) == this->initial_stack_pointer;
            }

            static constexpr std::array<uint8_t, 2> rdtsc{0x0F, 0x31};

            x86_64_emulator* cpu{};
            std::unique_ptr<emulator_thread> thread{};
            uint64_t dispatcher{};
            uint64_t stack{};
            uint64_t initial_stack_pointer{};
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

        class StackGuardFaultTest : public GuardPageFaultTest
        {
          protected:
            void SetUp() override
            {
                GuardPageFaultTest::SetUp();
                if (IsSkipped())
                {
                    return;
                }

                if (!this->emu->memory.supports_guarded_stack_growth())
                {
                    GTEST_SKIP() << "Backend commits thread stacks in full without guarded stack growth";
                }

                this->stack_thread =
                    std::make_unique<emulator_thread>(this->emu->memory, this->emu->process, 0, 0, stack_reserve, 0, 12, false, page_size);
                this->emu->vcpu(0).active_thread = this->stack_thread.get();
            }

            TEB64 teb() const
            {
                return this->stack_thread->teb64->read();
            }

            std::vector<std::pair<uint64_t, EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>> read_each_page_below_the_stack_limit()
            {
                const auto start = this->teb();
                std::vector<std::pair<uint64_t, EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>> exceptions{};
                for (auto address = start.NtTib.StackLimit - 1; address > start.DeallocationStack; address -= page_size)
                {
                    const auto record = this->run(read_byte, address);
                    if (this->cpu->reg(x86_register::rsp) != this->initial_stack_pointer)
                    {
                        exceptions.emplace_back(address, record);
                    }
                }

                return exceptions;
            }

            static constexpr uint64_t stack_reserve = 0x40000;
            static constexpr std::array<uint8_t, 2> read_byte{0x8A, 0x01};
            static constexpr std::array<uint8_t, 2> write_byte{0x88, 0x01};

            std::unique_ptr<emulator_thread> stack_thread{};
        };

        void expect_stack_overflow(const EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>& record, const uint64_t access_type,
                                   const uint64_t address)
        {
            EXPECT_EQ(STATUS_STACK_OVERFLOW, static_cast<NTSTATUS>(record.ExceptionCode));
            ASSERT_EQ(2u, record.NumberParameters);
            EXPECT_EQ(access_type, record.ExceptionInformation[0]);
            EXPECT_EQ(address, record.ExceptionInformation[1]);
        }

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

    TEST_F(StackLayoutTest, NativeThreadStackHasAThreePageGuardRegionBelowItsCommittedPart)
    {
        const emulator_thread thread{this->emu->memory, this->emu->process, 0, 0, 0x40000, 0, 12, false, page_size};
        const auto teb = thread.teb64->read();

        EXPECT_EQ(0x40000u, teb.NtTib.StackBase - teb.DeallocationStack);
        EXPECT_EQ(0u, teb.GuaranteedStackBytes);
        this->expect_stack_layout(teb.DeallocationStack, teb.NtTib.StackLimit, teb.NtTib.StackBase, 3 * page_size);
    }

    TEST_F(StackLayoutTest, Wow64ThreadStackCommitsOnePageAboveATwoPageGuardRegion)
    {
        this->emu->process.is_wow64_process = true;
        const emulator_thread thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 12, false, page_size};
        const auto teb = thread.teb32->read();

        EXPECT_EQ(0x100000u, teb.NtTib.StackBase - teb.DeallocationStack);
        EXPECT_EQ(teb.NtTib.StackBase - page_size, teb.NtTib.StackLimit);
        this->expect_stack_layout(teb.DeallocationStack, teb.NtTib.StackLimit, teb.NtTib.StackBase, 2 * page_size);
    }

    TEST_F(StackLayoutTest, Wow64StackOverflowsWhenItsGuardRegionReachesTheFirstPage)
    {
        this->emu->process.is_wow64_process = true;
        const emulator_thread thread{this->emu->memory, this->emu->process, 0, 0, 0x40000, 0, 12, false, page_size};
        const auto start = thread.teb32->read();
        const uint64_t deallocation = start.DeallocationStack;

        std::vector<uint64_t> overflowed_pages{};
        for (uint64_t page = start.NtTib.StackLimit - page_size; page > deallocation; page -= page_size)
        {
            if (!this->emu->memory.release_guard_page(page))
            {
                continue;
            }

            if (thread.handle_stack_guard_fault(page + 0xFFF) == stack_guard_fault::overflow)
            {
                overflowed_pages.push_back(page);
            }
            else
            {
                const auto teb = thread.teb32->read();
                this->expect_stack_layout(deallocation, teb.NtTib.StackLimit, teb.NtTib.StackBase, 2 * page_size);
            }
        }

        EXPECT_EQ(std::vector<uint64_t>{deallocation + 2 * page_size}, overflowed_pages);
        EXPECT_EQ(deallocation + page_size, thread.teb32->read().NtTib.StackLimit);
        EXPECT_EQ(start.NtTib.StackBase - deallocation - page_size, this->query(deallocation + page_size).RegionSize);
        EXPECT_EQ(static_cast<DWORD>(MEM_RESERVE), this->query(deallocation).State);
    }

    TEST_F(StackGuardFaultTest, TouchingTheStackGuardGrowsTheStackByOnePage)
    {
        const auto before = this->teb();

        EXPECT_TRUE(this->runs_without_exception(read_byte, before.NtTib.StackLimit - 1));

        const auto after = this->teb();
        EXPECT_EQ(before.NtTib.StackLimit - page_size, after.NtTib.StackLimit);
        this->expect_stack_layout(after.DeallocationStack, after.NtTib.StackLimit, after.NtTib.StackBase, 3 * page_size);
    }

    TEST_F(StackGuardFaultTest, WalkingDownTheStackOverflowsWhenTheGuardRegionReachesTheFirstPage)
    {
        const auto deallocation = this->teb().DeallocationStack;

        const auto exceptions = this->read_each_page_below_the_stack_limit();

        ASSERT_EQ(2u, exceptions.size());
        EXPECT_EQ(deallocation + 0x3FFF, exceptions[0].first);
        expect_stack_overflow(exceptions[0].second, 0, deallocation + 0x3FFF);
        EXPECT_EQ(deallocation + 0xFFF, exceptions[1].first);
        expect_access_violation(exceptions[1].second, 0, deallocation + 0xFFF);

        EXPECT_EQ(deallocation + page_size, this->teb().NtTib.StackLimit);
        const auto committed_part = this->query(deallocation + page_size);
        EXPECT_EQ(stack_reserve - page_size, committed_part.RegionSize);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), committed_part.Protect);
        EXPECT_EQ(static_cast<DWORD>(MEM_RESERVE), this->query(deallocation).State);
    }

    TEST_F(StackGuardFaultTest, GuardRearmedOnAnOverflowedPageOverflowsAgain)
    {
        const auto deallocation = this->teb().DeallocationStack;
        this->read_each_page_below_the_stack_limit();
        const auto overflow_page = deallocation + 3 * page_size;
        ASSERT_EQ(STATUS_SUCCESS, this->protect(overflow_page, page_size, PAGE_READWRITE | PAGE_GUARD).status);

        expect_stack_overflow(this->run(write_byte, overflow_page + 0xFFF), 1, overflow_page + 0xFFF);

        EXPECT_EQ(deallocation + page_size, this->teb().NtTib.StackLimit);
        EXPECT_EQ(static_cast<DWORD>(PAGE_READWRITE), this->query(overflow_page).Protect);
    }

    TEST_F(StackGuardFaultTest, StackGuaranteeWidensTheGuardRegionAndMovesTheOverflow)
    {
        constexpr uint64_t guarantee = 0x10000;
        constexpr uint64_t guard_size = guarantee + page_size;
        const auto start = this->teb();
        this->stack_thread->teb64->access([&](TEB64& thread_block) { thread_block.GuaranteedStackBytes = static_cast<ULONG>(guarantee); });
        ASSERT_EQ(STATUS_SUCCESS,
                  this->allocate(start.NtTib.StackLimit - guard_size, guard_size, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD).status);

        EXPECT_TRUE(this->runs_without_exception(read_byte, start.NtTib.StackLimit - 1));
        const auto grown = this->teb();
        this->expect_stack_layout(grown.DeallocationStack, grown.NtTib.StackLimit, grown.NtTib.StackBase, guard_size);

        const auto exceptions = this->read_each_page_below_the_stack_limit();

        ASSERT_FALSE(exceptions.empty());
        const auto overflow_address = start.DeallocationStack + 17 * page_size + 0xFFF;
        EXPECT_EQ(overflow_address, exceptions[0].first);
        expect_stack_overflow(exceptions[0].second, 0, overflow_address);
        EXPECT_EQ(start.DeallocationStack + page_size, this->teb().NtTib.StackLimit);
    }

    TEST_F(StackGuardFaultTest, ExceptionFramePushedAcrossTheStackGuardGrowsTheStack)
    {
        const auto before = this->teb();
        this->initial_stack_pointer = before.NtTib.StackLimit + 0x100;

        expect_access_violation(this->run(read_byte, 0), 0, 0);

        const auto after = this->teb();
        EXPECT_EQ(before.NtTib.StackLimit - page_size, after.NtTib.StackLimit);
        this->expect_stack_layout(after.DeallocationStack, after.NtTib.StackLimit, after.NtTib.StackBase, 3 * page_size);
    }

    TEST_F(GuardPageTest, ThreadStacksAreCommittedInFullWithoutPageGranularProtection)
    {
        coarse_page_memory backend{};
        memory_manager memory{backend};

        const emulator_thread native_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 12, false, page_size};
        const auto native_teb = native_thread.teb64->read();
        EXPECT_EQ(native_teb.DeallocationStack, native_teb.NtTib.StackLimit);
        const auto native_stack = memory.get_region_info(native_teb.DeallocationStack);
        EXPECT_TRUE(native_stack.is_committed);
        EXPECT_EQ(0x40000u, native_stack.length);
        EXPECT_FALSE(native_stack.permissions.is_guarded());

        this->emu->process.is_wow64_process = true;
        const emulator_thread wow64_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 16, false, page_size};
        const auto wow64_teb = wow64_thread.teb32->read();
        EXPECT_EQ(wow64_teb.DeallocationStack, wow64_teb.NtTib.StackLimit);
        const auto wow64_stack = memory.get_region_info(wow64_teb.DeallocationStack);
        EXPECT_TRUE(wow64_stack.is_committed);
        EXPECT_EQ(0x40000u, wow64_stack.length);
        EXPECT_FALSE(wow64_stack.permissions.is_guarded());
    }

    TEST_F(GuardPageTest, ThreadStacksAreCommittedInFullWithoutRestartableFaults)
    {
        coarse_page_memory backend{};
        backend.page_granular_protection = true;
        backend.restartable_faults = false;
        memory_manager memory{backend};

        const emulator_thread native_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 12, false, page_size};
        const auto native_teb = native_thread.teb64->read();
        EXPECT_EQ(native_teb.DeallocationStack, native_teb.NtTib.StackLimit);
        EXPECT_FALSE(memory.get_region_info(native_teb.DeallocationStack).permissions.is_guarded());

        this->emu->process.is_wow64_process = true;
        const emulator_thread wow64_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 16, false, page_size};
        const auto wow64_teb = wow64_thread.teb32->read();
        EXPECT_EQ(wow64_teb.DeallocationStack, wow64_teb.NtTib.StackLimit);
        EXPECT_FALSE(memory.get_region_info(wow64_teb.DeallocationStack).permissions.is_guarded());
    }

    TEST_F(GuardPageTest, GuardedStacksAreCommittedInFullOncePageGranularProtectionIsLost)
    {
        coarse_page_memory backend{};
        backend.page_granular_protection = true;
        memory_manager memory{backend};
        const emulator_thread native_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 12, false, page_size};
        this->emu->process.is_wow64_process = true;
        const emulator_thread wow64_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 16, false, page_size};
        ASSERT_LT(native_thread.teb64->read().DeallocationStack, native_thread.teb64->read().NtTib.StackLimit);
        ASSERT_LT(wow64_thread.teb32->read().DeallocationStack, wow64_thread.teb32->read().NtTib.StackLimit);

        backend.page_granular_protection = false;
        EXPECT_FALSE(memory.supports_page_granular_protection());
        backend.page_granular_protection = true;
        EXPECT_FALSE(memory.supports_page_granular_protection());

        native_thread.commit_stacks_in_full();
        wow64_thread.commit_stacks_in_full();

        const auto native_teb = native_thread.teb64->read();
        EXPECT_EQ(native_teb.DeallocationStack, native_teb.NtTib.StackLimit);
        const auto native_stack = memory.get_region_info(native_teb.DeallocationStack);
        EXPECT_TRUE(native_stack.is_committed);
        EXPECT_EQ(0x40000u, native_stack.length);
        EXPECT_FALSE(native_stack.permissions.is_guarded());

        const auto wow64_teb = wow64_thread.teb32->read();
        EXPECT_EQ(wow64_teb.DeallocationStack, wow64_teb.NtTib.StackLimit);
        const auto wow64_stack = memory.get_region_info(wow64_teb.DeallocationStack);
        EXPECT_TRUE(wow64_stack.is_committed);
        EXPECT_EQ(0x40000u, wow64_stack.length);
        EXPECT_FALSE(wow64_stack.permissions.is_guarded());

        const emulator_thread later_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 20, false, page_size};
        const auto later_teb = later_thread.teb32->read();
        EXPECT_EQ(later_teb.DeallocationStack, later_teb.NtTib.StackLimit);
    }

    TEST_F(GuardPageTest, ThreadStacksOnAFiberAreCommittedInFullOncePageGranularProtectionIsLost)
    {
        constexpr uint64_t fiber_reserve = 0x40000;
        coarse_page_memory backend{};
        backend.page_granular_protection = true;
        memory_manager memory{backend};

        const auto allocate_fiber_stack = [&] {
            const auto base = memory.allocate_memory(fiber_reserve, memory_permission::read_write, true);
            const auto limit = base + fiber_reserve - page_size;
            memory.commit_memory(limit, page_size, memory_permission::read_write);
            memory.commit_memory(limit - 3 * page_size, 3 * page_size,
                                 nt_memory_permission{memory_permission::read_write, memory_permission_ext::guard});
            return std::pair{base, limit};
        };

        const auto expect_committed_in_full = [&](const uint64_t base, const uint64_t size) {
            const auto region = memory.get_region_info(base);
            EXPECT_TRUE(region.is_committed);
            EXPECT_EQ(size, region.length);
            EXPECT_FALSE(region.permissions.is_guarded());
        };

        const emulator_thread native_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 12, false, page_size};
        const auto [native_fiber, native_fiber_limit] = allocate_fiber_stack();
        native_thread.teb64->access([&](TEB64& thread_block) {
            thread_block.DeallocationStack = native_fiber;
            thread_block.NtTib.StackBase = native_fiber + fiber_reserve;
            thread_block.NtTib.StackLimit = native_fiber_limit;
        });

        this->emu->process.is_wow64_process = true;
        const emulator_thread wow64_thread{memory, this->emu->process, 0, 0, 0x40000, 0, 16, false, page_size};
        const auto [wow64_fiber, wow64_fiber_limit] = allocate_fiber_stack();
        wow64_thread.teb32->access([&](TEB32& thread_block) {
            thread_block.DeallocationStack = static_cast<uint32_t>(wow64_fiber);
            thread_block.NtTib.StackBase = static_cast<uint32_t>(wow64_fiber + fiber_reserve);
            thread_block.NtTib.StackLimit = static_cast<uint32_t>(wow64_fiber_limit);
        });

        backend.page_granular_protection = false;
        ASSERT_FALSE(memory.supports_page_granular_protection());
        native_thread.commit_stacks_in_full();
        wow64_thread.commit_stacks_in_full();

        EXPECT_EQ(native_fiber, native_thread.teb64->read().NtTib.StackLimit);
        expect_committed_in_full(native_fiber, fiber_reserve);
        expect_committed_in_full(native_thread.stack_base, native_thread.stack_size);

        EXPECT_EQ(wow64_fiber, wow64_thread.teb32->read().NtTib.StackLimit);
        expect_committed_in_full(wow64_fiber, fiber_reserve);
        expect_committed_in_full(*wow64_thread.wow64_stack_base, *wow64_thread.wow64_stack_size);
    }

    TEST_F(StackGuardFaultTest, StoreThatGrowsTheStackLandsInTheGrownPage)
    {
        constexpr uint64_t value = 0x1122334455667788;
        const auto limit = this->teb().NtTib.StackLimit;
        constexpr std::array<uint8_t, 13> store_rax{0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x48, 0x89, 0x01};

        EXPECT_TRUE(this->runs_without_exception(store_rax, limit - 8));

        EXPECT_EQ(value, this->emu->memory.read_memory<uint64_t>(limit - 8));
        EXPECT_EQ(limit - page_size, this->teb().NtTib.StackLimit);
    }

    TEST_F(StackGuardFaultTest, SixteenByteStoreThatGrowsTheStackLandsInTheGrownPage)
    {
        const auto limit = this->teb().NtTib.StackLimit;
        constexpr std::array<uint8_t, 22> store_rax_twice_from_xmm0{0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x66,
                                                                    0x48, 0x0F, 0x6E, 0xC0, 0x66, 0x0F, 0x6C, 0xC0, 0x0F, 0x11, 0x01};

        EXPECT_TRUE(this->runs_without_exception(store_rax_twice_from_xmm0, limit - 16));

        EXPECT_EQ(0x1122334455667788u, this->emu->memory.read_memory<uint64_t>(limit - 16));
        EXPECT_EQ(0x1122334455667788u, this->emu->memory.read_memory<uint64_t>(limit - 8));
    }

    TEST_F(StackGuardFaultTest, LoadThatGrowsTheStackReadsTheGuardPage)
    {
        constexpr uint64_t value = 0x8877665544332211;
        const auto limit = this->teb().NtTib.StackLimit;
        this->emu->memory.write_memory(limit - 8, value);
        this->cpu->reg(x86_register::rbx, 0);
        constexpr std::array<uint8_t, 3> load_rbx{0x48, 0x8B, 0x19};

        EXPECT_TRUE(this->runs_without_exception(load_rbx, limit - 8));

        EXPECT_EQ(value, this->cpu->reg(x86_register::rbx));
    }

    TEST(MemoryViolationRestartTest, AccessStoppedAtARestartCompletesBeforeOtherCodeRuns)
    {
        constexpr uint64_t value = 0x0123456789ABCDEF;
        auto emu = create_x86_64_emulator_from_environment();
        memory_manager memory{*emu};
        memory.reserve_host_memory_ranges();
        const auto code = memory.allocate_memory(page_size, memory_permission::read_write);
        const auto gdt = memory.allocate_memory(page_size, memory_permission::read_write);
        const auto data = memory.allocate_memory(page_size, memory_permission::read_write);
        ASSERT_NE(0u, code);
        ASSERT_NE(0u, gdt);
        ASSERT_NE(0u, data);

        constexpr uint16_t user_data_selector = 0x2B;
        constexpr uint16_t user_code_selector = 0x33;
        emu->write_memory<uint64_t>(gdt + 0x28, 0x00CFF3000000FFFF);
        emu->write_memory<uint64_t>(gdt + 0x30, 0x00AFFB000000FFFF);
        emu->load_gdt(gdt, page_size);
        emu->reg<uint16_t>(x86_register::ss, user_data_selector);
        emu->reg<uint16_t>(x86_register::cs, user_code_selector);

        constexpr uint64_t other_code_offset = 0x40;
        constexpr std::array<uint8_t, 5> load_then_halt{0x48, 0x8B, 0x01, 0xF4, 0xF4};
        constexpr std::array<uint8_t, 9> other_code{0x48, 0xC7, 0xC3, 0x34, 0x12, 0x00, 0x00, 0xF4, 0xF4};
        memory.write_memory(code, load_then_halt.data(), load_then_halt.size());
        memory.write_memory(code + other_code_offset, other_code.data(), other_code.size());
        ASSERT_TRUE(memory.protect_memory(code, page_size, memory_permission::read_exec));
        memory.write_memory(data, value);
        ASSERT_TRUE(memory.protect_memory(data, page_size, memory_permission::none));

        emu->hook_memory_violation([&](cpu_interface& cpu, uint64_t, size_t, memory_operation, memory_violation_type) {
            memory.protect_memory(data, page_size, memory_permission::read_write);
            cpu.stop();
            return memory_violation_continuation::restart;
        });
        emu->hook_interrupt([](cpu_interface& cpu, int) { cpu.stop(); });

        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rcx, data);
        emu->reg(x86_register::rax, 0);
        emu->reg(x86_register::rbx, 0);
        emu->start(0);
        while (emu->has_violation())
        {
            emu->start(0);
        }

        const auto stopped_rip = emu->reg(x86_register::rip);
        const auto stopped_rax = emu->reg(x86_register::rax);
        emu->reg(x86_register::rax, 0);
        emu->reg(x86_register::rip, code + other_code_offset);
        emu->start(0);

        EXPECT_EQ(0u, emu->reg(x86_register::rax));
        EXPECT_EQ(0x1234u, emu->reg(x86_register::rbx));

        emu->reg(x86_register::rip, stopped_rip);
        emu->reg(x86_register::rax, stopped_rax);
        emu->reg(x86_register::rcx, data);
        emu->start(0);

        EXPECT_EQ(value, emu->reg(x86_register::rax));
    }
}

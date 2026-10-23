#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                            uint64_t zero_bits, emulator_object<uint64_t> bytes_to_allocate, uint32_t allocation_type,
                                            uint32_t page_protection);
    NTSTATUS handle_NtAllocateVirtualMemoryEx(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                              emulator_object<uint64_t> bytes_to_allocate, uint32_t allocation_type,
                                              uint32_t page_protection, emulator_object<MEM_EXTENDED_PARAMETER64> extended_parameters,
                                              ULONG extended_parameter_count);
    NTSTATUS handle_NtGetWriteWatch(const syscall_context& c, handle process_handle, ULONG flags, uint64_t base_address,
                                    uint64_t region_size, uint64_t addresses, emulator_object<uint64_t> count,
                                    emulator_object<ULONG> granularity);
    NTSTATUS handle_NtResetWriteWatch(const syscall_context& c, handle process_handle, uint64_t base_address, uint64_t region_size);
    NTSTATUS handle_NtGetCachedSigningLevel(const syscall_context& c, handle file_handle, emulator_object<ULONG> flags,
                                            emulator_object<UCHAR> signing_level, uint64_t thumbprint,
                                            emulator_object<ULONG> thumbprint_size, emulator_object<ULONG> thumbprint_algorithm);
    NTSTATUS handle_NtSetCachedSigningLevel();
    NTSTATUS handle_NtCompareSigningLevels(const syscall_context& c, UCHAR current_level, UCHAR required_level);
}

namespace sogen::test
{
    namespace
    {
        class UnsupportedRuntimeFeatureTest : public testing::Test
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
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(UnsupportedRuntimeFeatureTest, WriteWatchAllocationsDoNotReserveMemoryOrModifyOutputs)
    {
        const emulator_object<uint64_t> address{this->emu->memory, this->storage};
        const emulator_object<uint64_t> size{this->emu->memory, this->storage + 8};
        const auto regions = this->emu->memory.get_reserved_regions().size();
        for (const auto flags : {MEM_RESERVE, MEM_RESERVE | MEM_COMMIT, MEM_COMMIT, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                 MEM_RESERVE | MEM_REPLACE_PLACEHOLDER})
        {
            address.write(0);
            size.write(0x12345);
            EXPECT_EQ(STATUS_NOT_SUPPORTED, syscalls::handle_NtAllocateVirtualMemory(this->context(), CURRENT_PROCESS, address, 0, size,
                                                                                     flags | MEM_WRITE_WATCH, PAGE_READWRITE));
            EXPECT_EQ(0u, address.read());
            EXPECT_EQ(0x12345u, size.read());
            EXPECT_EQ(STATUS_NOT_SUPPORTED,
                      syscalls::handle_NtAllocateVirtualMemoryEx(this->context(), CURRENT_PROCESS, address, size, flags | MEM_WRITE_WATCH,
                                                                 PAGE_READWRITE, {this->emu->memory}, 0));
            EXPECT_EQ(0u, address.read());
            EXPECT_EQ(0x12345u, size.read());
            EXPECT_EQ(regions, this->emu->memory.get_reserved_regions().size());
        }
        EXPECT_EQ(STATUS_NOT_SUPPORTED,
                  syscalls::handle_NtAllocateVirtualMemoryEx(this->context(), {}, {this->emu->memory}, {this->emu->memory}, MEM_WRITE_WATCH,
                                                             0, {this->emu->memory}, 0));
    }

    TEST_F(UnsupportedRuntimeFeatureTest, OrdinaryAllocationStillSucceeds)
    {
        const emulator_object<uint64_t> address{this->emu->memory, this->storage};
        const emulator_object<uint64_t> size{this->emu->memory, this->storage + 8};
        address.write(0);
        size.write(0x1000);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtAllocateVirtualMemory(this->context(), CURRENT_PROCESS, address, 0, size,
                                                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        ASSERT_NE(0u, address.read());
        this->emu->memory.write_memory(address.read(), uint32_t{0xabcdef01});
        EXPECT_EQ(0xabcdef01u, this->emu->memory.read_memory<uint32_t>(address.read()));
    }

    TEST_F(UnsupportedRuntimeFeatureTest, WriteWatchQueriesAndResetLeaveOutputsUntouched)
    {
        this->emu->memory.set_memory(this->storage, 0x5a, 0x1000);
        const auto before = this->emu->memory.read_memory(this->storage, 0x1000);
        for (const auto flags : {0u, 1u, 0xffffffffu})
        {
            EXPECT_EQ(STATUS_NOT_SUPPORTED, syscalls::handle_NtGetWriteWatch(this->context(), CURRENT_PROCESS, flags, this->storage, 0x1000,
                                                                             this->storage, {this->emu->memory, this->storage + 0x100},
                                                                             {this->emu->memory, this->storage + 0x108}));
        }
        EXPECT_EQ(STATUS_NOT_SUPPORTED, syscalls::handle_NtResetWriteWatch(this->context(), CURRENT_PROCESS, this->storage, 0x1000));
        EXPECT_EQ(before, this->emu->memory.read_memory(this->storage, 0x1000));
        EXPECT_EQ(STATUS_NOT_SUPPORTED,
                  syscalls::handle_NtGetWriteWatch(this->context(), {}, 0, 0, 0, 0, {this->emu->memory}, {this->emu->memory}));
        EXPECT_EQ(STATUS_NOT_SUPPORTED, syscalls::handle_NtResetWriteWatch(this->context(), {}, 0, 0));
    }

    TEST_F(UnsupportedRuntimeFeatureTest, SigningCacheOperationsAreUnsupportedWithoutChangingOutputBuffers)
    {
        this->emu->memory.set_memory(this->storage, 0x5a, 0x1000);
        const auto before = this->emu->memory.read_memory(this->storage, 0x1000);
        const auto file_handle = this->emu->process.files.store(file{});
        EXPECT_EQ(STATUS_NOT_SUPPORTED,
                  syscalls::handle_NtGetCachedSigningLevel(
                      this->context(), file_handle, {this->emu->memory, this->storage}, {this->emu->memory, this->storage + 0x100},
                      this->storage + 0x200, {this->emu->memory, this->storage + 0x300}, {this->emu->memory, this->storage + 0x400}));
        EXPECT_EQ(before, this->emu->memory.read_memory(this->storage, 0x1000));
        EXPECT_EQ(STATUS_NOT_SUPPORTED,
                  syscalls::handle_NtGetCachedSigningLevel(this->context(), {}, {this->emu->memory}, {this->emu->memory}, 0,
                                                           {this->emu->memory}, {this->emu->memory}));
        EXPECT_EQ(STATUS_NOT_SUPPORTED, syscalls::handle_NtSetCachedSigningLevel());
    }

    TEST_F(UnsupportedRuntimeFeatureTest, SigningComparisonIsUnsupportedForAllLevelValues)
    {
        for (unsigned current = 0; current <= 0xff; ++current)
        {
            for (unsigned required = 0; required <= 0xff; ++required)
            {
                EXPECT_EQ(STATUS_NOT_SUPPORTED, syscalls::handle_NtCompareSigningLevels(this->context(), static_cast<UCHAR>(current),
                                                                                        static_cast<UCHAR>(required)));
            }
        }
    }
}

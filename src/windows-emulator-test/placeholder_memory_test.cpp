#include <backend_selection.hpp>
#include <syscall_utils.hpp>
#include <gtest/gtest.h>

namespace sogen::syscalls
{
    NTSTATUS handle_NtAllocateVirtualMemoryEx(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                              emulator_object<uint64_t> bytes_to_allocate, uint32_t allocation_type,
                                              uint32_t page_protection, emulator_object<MEM_EXTENDED_PARAMETER64> extended_parameters,
                                              ULONG extended_parameter_count);
    NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                        emulator_object<uint64_t> bytes_to_allocate, uint32_t free_type);
}

namespace sogen::test
{
    namespace
    {
        class PlaceholderMemoryTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                               emulator_settings{.disable_logging = true, .load_registry = false},
                                                               emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
            }

            NTSTATUS allocate(uint64_t& address, uint64_t size, uint32_t flags, uint32_t protection = PAGE_NOACCESS)
            {
                const emulator_object<uint64_t> base{this->emu->memory, this->storage};
                const emulator_object<uint64_t> bytes{this->emu->memory, this->storage + 8};
                base.write(address);
                bytes.write(size);
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                const auto result = syscalls::handle_NtAllocateVirtualMemoryEx(context, CURRENT_PROCESS, base, bytes, flags, protection,
                                                                               {this->emu->memory}, 0);
                address = base.read();
                return result;
            }

            NTSTATUS free(uint64_t address, uint64_t size, uint32_t flags)
            {
                const emulator_object<uint64_t> base{this->emu->memory, this->storage};
                const emulator_object<uint64_t> bytes{this->emu->memory, this->storage + 8};
                base.write(address);
                bytes.write(size);
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtFreeVirtualMemory(context, CURRENT_PROCESS, base, bytes, flags);
            }

            std::unique_ptr<windows_emulator> emu;
            uint64_t storage{};
        };
    }

    TEST_F(PlaceholderMemoryTest, ReserveSplitReplacePreserveAndCoalesce)
    {
        constexpr uint64_t size = 0x200000000;
        constexpr uint64_t segment = 0x10000;
        uint64_t base{};
        ASSERT_EQ(this->allocate(base, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER), STATUS_SUCCESS);
        EXPECT_EQ(this->emu->memory.get_region_kind(base), memory_region_kind::placeholder);
        EXPECT_FALSE(this->emu->memory.get_region_info(base).is_committed);
        ASSERT_EQ(this->free(base, segment, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), STATUS_SUCCESS);
        EXPECT_EQ(this->emu->memory.get_region_info(base).allocation_length, segment);
        EXPECT_EQ(this->emu->memory.get_region_info(base + segment).allocation_length, size - segment);
        auto replaced = base;
        ASSERT_EQ(this->allocate(replaced, segment, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE), STATUS_SUCCESS);
        constexpr uint64_t pattern = 0x1234;
        this->emu->memory.write_memory(base, &pattern, sizeof(pattern));
        EXPECT_EQ(this->emu->memory.read_memory<uint64_t>(base), 0x1234u);
        ASSERT_EQ(this->free(base, segment, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), STATUS_SUCCESS);
        EXPECT_FALSE(this->emu->memory.get_region_info(base).is_committed);
        ASSERT_EQ(this->free(base, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS), STATUS_SUCCESS);
        EXPECT_EQ(this->emu->memory.get_region_info(base).allocation_length, size);
        EXPECT_EQ(this->free(base, 0, MEM_RELEASE), STATUS_SUCCESS);
        EXPECT_FALSE(this->emu->memory.get_region_info(base).is_reserved);
    }

    TEST_F(PlaceholderMemoryTest, ReplacementMustMatchAnEntirePlaceholder)
    {
        uint64_t base{};
        ASSERT_EQ(this->allocate(base, 0x30000, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER), STATUS_SUCCESS);
        auto partial = base;
        EXPECT_EQ(this->allocate(partial, 0x10000, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER), STATUS_CONFLICTING_ADDRESSES);
        EXPECT_EQ(this->emu->memory.get_region_info(base).allocation_length, 0x30000u);
        ASSERT_EQ(this->free(base, 0x10000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), STATUS_SUCCESS);
        auto replacement = base;
        ASSERT_EQ(this->allocate(replacement, 0x10000, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER), STATUS_SUCCESS);
        EXPECT_EQ(this->free(base, 0x30000, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS), STATUS_CONFLICTING_ADDRESSES);
        EXPECT_EQ(this->free(base, 0, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), STATUS_SUCCESS);
        EXPECT_EQ(this->free(base, 0x30000, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS), STATUS_SUCCESS);
    }

    TEST_F(PlaceholderMemoryTest, PlaceholderCannotBeCommittedWithoutReplacement)
    {
        uint64_t base{};
        ASSERT_EQ(this->allocate(base, 0x10000, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER), STATUS_SUCCESS);
        auto address = base;
        EXPECT_NE(this->allocate(address, 0x10000, MEM_COMMIT, PAGE_READWRITE), STATUS_SUCCESS);
        EXPECT_FALSE(this->emu->memory.get_region_info(base).is_committed);
        uint64_t invalid{};
        EXPECT_EQ(this->allocate(invalid, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_RESERVE_PLACEHOLDER), STATUS_INVALID_PARAMETER);
        invalid = 0;
        EXPECT_EQ(this->allocate(invalid, 0x10000, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_READWRITE), STATUS_INVALID_PARAMETER);
    }
}

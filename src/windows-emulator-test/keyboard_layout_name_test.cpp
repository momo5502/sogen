#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <cwctype>

namespace sogen::syscalls
{
    BOOL handle_NtUserGetKeyboardLayoutName(const syscall_context& c, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> name);
}

namespace sogen::test
{
    namespace
    {
        constexpr char16_t untouched_char = u'x';
        constexpr uint64_t untouched_guard = 0xAAAAAAAAAAAAAAAA;
        constexpr size_t name_capacity = 9; // KL_NAMELENGTH

        class KeyboardLayoutNameTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->string_address = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->buffer_address = this->string_address + 0x100;
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            // Lays the string out the way user32!GetKeyboardLayoutNameW does: empty, describing the caller's buffer.
            BOOL get_name(const USHORT maximum_length)
            {
                std::array<char16_t, name_capacity> buffer{};
                buffer.fill(untouched_char);
                this->emu.memory.write_memory(this->buffer_address, buffer.data(), sizeof(buffer));

                UNICODE_STRING<EmulatorTraits<Emu64>> string{};
                string.MaximumLength = maximum_length;
                string.Buffer = this->buffer_address;
                this->emu.memory.write_memory(this->string_address, string);
                this->emu.memory.write_memory(this->string_address + sizeof(string), untouched_guard);

                return syscalls::handle_NtUserGetKeyboardLayoutName(this->context(), {this->emu.memory, this->string_address});
            }

            std::array<char16_t, name_capacity> buffer()
            {
                std::array<char16_t, name_capacity> result{};
                this->emu.memory.read_memory(this->buffer_address, result.data(), sizeof(result));
                return result;
            }

            UNICODE_STRING<EmulatorTraits<Emu64>> string()
            {
                return this->emu.memory.read_memory<UNICODE_STRING<EmulatorTraits<Emu64>>>(this->string_address);
            }

            uint64_t guard()
            {
                return this->emu.memory.read_memory<uint64_t>(this->string_address + sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>));
            }

            windows_emulator emu = create_sample_emulator();
            uint64_t string_address{};
            uint64_t buffer_address{};
        };

        TEST_F(KeyboardLayoutNameTest, FillsTheBufferTheStringDescribes)
        {
            ASSERT_TRUE(this->get_name(name_capacity * sizeof(char16_t)));

            const auto name = this->buffer();
            EXPECT_EQ(name.back(), u'\0');
            for (size_t i = 0; i + 1 < name.size(); ++i)
            {
                EXPECT_TRUE(std::iswxdigit(static_cast<wint_t>(name[i]))) << "at " << i;
            }

            const auto string = this->string();
            EXPECT_EQ(string.Length, (name_capacity - 1) * sizeof(char16_t));
            EXPECT_EQ(string.MaximumLength, name_capacity * sizeof(char16_t));
            EXPECT_EQ(string.Buffer, this->buffer_address);
            EXPECT_EQ(this->guard(), untouched_guard);
        }

        TEST_F(KeyboardLayoutNameTest, RefusesABufferTooSmallForTheTerminatedName)
        {
            EXPECT_FALSE(this->get_name((name_capacity - 1) * sizeof(char16_t)));

            const auto name = this->buffer();
            EXPECT_TRUE(std::ranges::all_of(name, [](const char16_t c) { return c == untouched_char; }));
            EXPECT_EQ(this->string().Length, 0);
            EXPECT_EQ(this->guard(), untouched_guard);
        }
    }
}

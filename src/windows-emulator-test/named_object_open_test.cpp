#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtOpenEvent(const syscall_context& c, emulator_object<uint64_t> event_handle, ACCESS_MASK desired_access,
                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
    NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK desired_access,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG initial_count,
                                      ULONG maximum_count);
    NTSTATUS handle_NtOpenSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK desired_access,
                                    emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes);
    NTSTATUS handle_NtReleaseSemaphore(const syscall_context& c, handle semaphore_handle, ULONG release_count,
                                       emulator_object<LONG> previous_count);
}

namespace sogen::test
{
    namespace
    {
        class NamedObjectOpenTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
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

            emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> named_attributes(const std::u16string_view name)
            {
                emulator_allocator allocator{this->emu->memory, this->storage, 0x1000};
                const auto string = allocator.make_unicode_string(name);
                const auto attributes = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> values{};
                values.Length = sizeof(values);
                values.ObjectName = string.value();
                attributes.write(values);
                return attributes;
            }

            emulator_object<handle> handle_slot()
            {
                return {this->emu->memory, this->storage + 0x800};
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(NamedObjectOpenTest, SemaphoreOpenedByNameOutlivesTheHandlesItWasCreatedWith)
    {
        const auto attributes = this->named_attributes(u"\\BaseNamedObjects\\sogen_named_semaphore");

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateSemaphore(this->context(), this->handle_slot(), 0, attributes, 1, 5));
        const auto creator = this->handle_slot().read();
        ASSERT_EQ(STATUS_OBJECT_NAME_EXISTS, syscalls::handle_NtCreateSemaphore(this->context(), this->handle_slot(), 0, attributes, 1, 5));
        const auto second_creator = this->handle_slot().read();
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtOpenSemaphore(this->context(), this->handle_slot(), 0, attributes));
        const auto opened = this->handle_slot().read();

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), creator));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), second_creator));

        const emulator_object<LONG> previous{this->emu->memory, this->storage + 0x900};
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtReleaseSemaphore(this->context(), opened, 1, previous));
        EXPECT_EQ(1, previous.read());

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), opened));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, syscalls::handle_NtOpenSemaphore(this->context(), this->handle_slot(), 0, attributes));
    }

    TEST_F(NamedObjectOpenTest, OpeningAnEventThatDoesNotExistReportsTheNameAsNotFound)
    {
        const auto attributes = this->named_attributes(u"\\BaseNamedObjects\\sogen_missing_event");
        const emulator_object<uint64_t> output{this->emu->memory, this->storage + 0x800};

        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, syscalls::handle_NtOpenEvent(this->context(), output, 0, attributes));
    }
}

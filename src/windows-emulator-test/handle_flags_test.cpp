#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtQueryObject(const syscall_context& c, handle h, OBJECT_INFORMATION_CLASS information_class,
                                  emulator_pointer information, ULONG information_length, emulator_object<ULONG> return_length);
    NTSTATUS handle_NtSetInformationObject(const syscall_context& c, handle h, OBJECT_INFORMATION_CLASS information_class,
                                           emulator_pointer information, ULONG information_length);
    NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                  BOOLEAN initial_state);
    NTSTATUS handle_NtCreateMutant(const syscall_context& c, emulator_object<handle> mutant_handle, ACCESS_MASK desired_access,
                                   emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, BOOLEAN initial_owner);
    NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK desired_access,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, LONG initial_count,
                                      LONG maximum_count);
    NTSTATUS handle_NtDuplicateObject(const syscall_context& c, handle source_process_handle, handle source_handle,
                                      handle target_process_handle, emulator_object<handle> target_handle, ACCESS_MASK desired_access,
                                      ULONG handle_attributes, ULONG options);
}

namespace sogen::test
{
    namespace
    {
        class HandleFlagsTest : public testing::Test
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

            NTSTATUS set_flags(handle h, bool inherit, bool protect)
            {
                emulator_object<OBJECT_HANDLE_FLAG_INFORMATION>{this->emu->memory, this->storage}.write(
                    {.Inherit = static_cast<BOOLEAN>(inherit), .ProtectFromClose = static_cast<BOOLEAN>(protect)});
                return syscalls::handle_NtSetInformationObject(this->context(), h, ObjectHandleFlagInformation, this->storage,
                                                               sizeof(OBJECT_HANDLE_FLAG_INFORMATION));
            }

            handle duplicate(handle source, ULONG handle_attributes, ULONG options)
            {
                const emulator_object<handle> output{this->emu->memory, this->storage + 0x300};
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtDuplicateObject(this->context(), CURRENT_PROCESS, source, CURRENT_PROCESS,
                                                                             output, 0, handle_attributes, options));
                return output.read();
            }

            void expect_flags(handle h, bool inherit, bool protect)
            {
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryObject(this->context(), h, ObjectHandleFlagInformation,
                                                                         this->storage + 0x100, sizeof(OBJECT_HANDLE_FLAG_INFORMATION),
                                                                         {this->emu->memory, this->storage + 0x200}));
                const auto flags = emulator_object<OBJECT_HANDLE_FLAG_INFORMATION>{this->emu->memory, this->storage + 0x100}.read();
                EXPECT_EQ(inherit, flags.Inherit != 0);
                EXPECT_EQ(protect, flags.ProtectFromClose != 0);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(HandleFlagsTest, ProtectsFileHandleUntilExplicitlyCleared)
    {
        const auto h = this->emu->process.files.store(file{});
        this->expect_flags(h, false, false);
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(h, true, true));
        this->expect_flags(h, true, true);
        EXPECT_EQ(STATUS_HANDLE_NOT_CLOSABLE, syscalls::handle_NtClose(this->context(), h));
        EXPECT_NE(nullptr, this->emu->process.files.get(h));
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(h, false, false));
        this->expect_flags(h, false, false);
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), h));
        EXPECT_EQ(nullptr, this->emu->process.files.get(h));
    }

    TEST_F(HandleFlagsTest, ProtectsOtherKernelObjects)
    {
        const auto h = this->emu->process.events.store(event{});
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(h, false, true));
        this->expect_flags(h, false, true);
        EXPECT_EQ(STATUS_HANDLE_NOT_CLOSABLE, syscalls::handle_NtClose(this->context(), h));
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(h, false, false));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), h));
    }

    TEST_F(HandleFlagsTest, DuplicatesKeepIndependentFlags)
    {
        const auto original = this->emu->process.events.store(event{});
        const auto duplicate = this->emu->process.events.duplicate(original);
        ASSERT_TRUE(duplicate);
        EXPECT_NE(original, *duplicate);

        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(*duplicate, true, true));
        this->expect_flags(original, false, false);
        this->expect_flags(*duplicate, true, true);

        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), original));
        EXPECT_EQ(STATUS_HANDLE_NOT_CLOSABLE, syscalls::handle_NtClose(this->context(), *duplicate));
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(*duplicate, false, false));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), *duplicate));
    }

    TEST_F(HandleFlagsTest, DuplicateUsesTheRequestedAttributesUnlessSameAttributesIsAsked)
    {
        constexpr ULONG duplicate_same_attributes = 4;
        const auto source = this->emu->process.events.store(event{});
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(source, true, true));

        const auto plain = this->duplicate(source, 0, 0);
        this->expect_flags(plain, false, false);
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), plain));

        const auto requested = this->duplicate(source, OBJ_INHERIT | OBJ_PROTECT_CLOSE, 0);
        this->expect_flags(requested, true, true);
        EXPECT_EQ(STATUS_HANDLE_NOT_CLOSABLE, syscalls::handle_NtClose(this->context(), requested));
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(requested, false, false));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), requested));

        const auto same = this->duplicate(source, 0, duplicate_same_attributes);
        this->expect_flags(same, true, true);
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(same, false, false));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), same));

        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(source, false, false));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), source));
    }

    TEST_F(HandleFlagsTest, DuplicateCloseSourceClosesTheOriginal)
    {
        constexpr ULONG duplicate_close_source = 1;
        const auto source = this->emu->process.events.store(event{});
        const auto duplicate = this->duplicate(source, 0, duplicate_close_source);

        EXPECT_EQ(nullptr, this->emu->process.events.get(source));
        ASSERT_NE(nullptr, this->emu->process.events.get(duplicate));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), duplicate));
    }

    TEST_F(HandleFlagsTest, ForgedSystemBitDoesNotCloseAProtectedGuestHandle)
    {
        const auto handle = this->emu->process.events.store(event{});
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(handle, false, true));

        auto forged = handle;
        forged.value.is_system = true;
        EXPECT_EQ(STATUS_INVALID_HANDLE, syscalls::handle_NtClose(this->context(), forged));
        ASSERT_NE(nullptr, this->emu->process.events.get(handle));

        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(handle, false, false));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), handle));
    }

    TEST_F(HandleFlagsTest, DuplicateCloseSourceIgnoresTheProtectedSourceCloseStatus)
    {
        constexpr ULONG duplicate_close_source = 1;
        const auto source = this->emu->process.events.store(event{});
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(source, false, true));

        const auto duplicate = this->duplicate(source, 0, duplicate_close_source);
        ASSERT_NE(nullptr, this->emu->process.events.get(source));
        ASSERT_NE(nullptr, this->emu->process.events.get(duplicate));

        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(source, false, false));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), source));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), duplicate));
    }

    TEST_F(HandleFlagsTest, RejectsInvalidObjectHandleWhenSettingFlags)
    {
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->set_flags({}, true, true));
        const auto h = this->emu->process.files.make_handle(99);
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->set_flags(h, true, true));
    }

    TEST_F(HandleFlagsTest, SerializationPreservesHandleProtection)
    {
        const auto h = this->emu->process.events.store(event{});
        ASSERT_EQ(STATUS_SUCCESS, this->set_flags(h, true, true));
        utils::buffer_serializer buffer{};
        this->emu->process.events.serialize(buffer);
        utils::buffer_deserializer reader{buffer};
        this->emu->process.events.deserialize(reader);
        this->expect_flags(h, true, true);
        EXPECT_EQ(STATUS_HANDLE_NOT_CLOSABLE, syscalls::handle_NtClose(this->context(), h));
    }

    TEST_F(HandleFlagsTest, KernelObjectCreationHonorsInheritAttribute)
    {
        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> attributes{this->emu->memory, this->storage + 0x300};
        const emulator_object<handle> out{this->emu->memory, this->storage + 0x400};
        for (const auto flags : {0u, static_cast<uint32_t>(OBJ_INHERIT)})
        {
            OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> values{};
            values.Length = sizeof(values);
            values.Attributes = flags;
            attributes.write(values);
            ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateEvent(this->context(), out, 0, attributes, NotificationEvent, FALSE));
            this->expect_flags(out.read(), flags != 0, false);
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), out.read()));
            ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateMutant(this->context(), out, 0, attributes, FALSE));
            this->expect_flags(out.read(), flags != 0, false);
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), out.read()));
            ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateSemaphore(this->context(), out, 0, attributes, 0, 1));
            this->expect_flags(out.read(), flags != 0, false);
            EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), out.read()));
        }
    }
}

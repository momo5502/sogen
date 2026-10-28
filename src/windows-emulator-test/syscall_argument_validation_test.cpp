#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                  BOOLEAN initial_state);
    NTSTATUS handle_NtGetNextThread(const syscall_context& c, handle process_handle, handle thread_handle, ACCESS_MASK desired_access,
                                    ULONG handle_attributes, ULONG flags, emulator_object<handle> new_thread_handle);
    NTSTATUS handle_NtCreateThreadEx(const syscall_context& c, emulator_object<handle> thread_handle, ACCESS_MASK desired_access,
                                     emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, handle process_handle,
                                     uint64_t start_routine, uint64_t argument, ULONG create_flags, EmulatorTraits<Emu64>::SIZE_T zero_bits,
                                     EmulatorTraits<Emu64>::SIZE_T stack_size, EmulatorTraits<Emu64>::SIZE_T maximum_stack_size,
                                     emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attribute_list);
    NTSTATUS handle_NtQueueApcThreadEx2(const syscall_context& c, handle thread_handle, handle reserve_handle, uint32_t apc_flags,
                                        uint64_t apc_routine, uint64_t apc_argument1, uint64_t apc_argument2, uint64_t apc_argument3);
    NTSTATUS handle_NtUnmapViewOfSectionEx(const syscall_context& c, handle process_handle, uint64_t base_address, ULONG flags);
    NTSTATUS handle_NtSetSecurityObject(const syscall_context& c, handle h, SECURITY_INFORMATION security_information,
                                        emulator_pointer security_descriptor);
}

namespace sogen::test
{
    namespace
    {
        constexpr auto untouched = make_handle(0xAAAAAAAAULL);
        constexpr SECURITY_INFORMATION dacl_security_information = 0x4;

        class SyscallArgumentValidationTest : public testing::Test
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
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true}));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_object<handle> handle_slot()
            {
                return {this->emu->memory, this->storage};
            }

            handle create_event()
            {
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateEvent(this->context(), this->handle_slot(), 0, {this->emu->memory},
                                                                         NotificationEvent, FALSE));
                return this->handle_slot().read();
            }

            NTSTATUS next_thread(const handle process, const handle thread, const ULONG flags, handle& next)
            {
                const auto slot = this->handle_slot();
                slot.write(untouched);
                const auto status = syscalls::handle_NtGetNextThread(this->context(), process, thread, 0, 0, flags, slot);
                next = slot.read();
                return status;
            }

            NTSTATUS create_thread(const handle process, const ULONG flags, handle& thread)
            {
                const auto slot = this->handle_slot();
                slot.write(untouched);
                const auto status = syscalls::handle_NtCreateThreadEx(this->context(), slot, 0, {this->emu->memory}, process, 0, 0, flags,
                                                                      0, 0, 0, {this->emu->memory});
                thread = slot.read();
                return status;
            }

            NTSTATUS queue_apc(const handle thread, const uint32_t flags)
            {
                return syscalls::handle_NtQueueApcThreadEx2(this->context(), thread, NULL_HANDLE, flags, 0x1000, 0, 0, 0);
            }

            size_t pending_apc_count() const
            {
                return this->emu->vcpu(0).active_thread->pending_apcs.size();
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(SyscallArgumentValidationTest, GetNextThreadRejectsFlagsBeforeLookingAtHandles)
    {
        for (const ULONG flags : {0x1u, 0x2u, 0x80000000u})
        {
            handle next{};
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->next_thread(CURRENT_PROCESS, NULL_HANDLE, flags, next)) << flags;
            EXPECT_EQ(NULL_HANDLE, next) << flags;
        }

        handle next{};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->next_thread(NULL_HANDLE, NULL_HANDLE, 1, next));
        EXPECT_EQ(NULL_HANDLE, next);
    }

    TEST_F(SyscallArgumentValidationTest, GetNextThreadWithoutAnOutputPointerIsAnAccessViolation)
    {
        const emulator_object<handle> no_output{this->emu->memory};

        for (const ULONG flags : {0x0u, 0x1u})
        {
            EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                      syscalls::handle_NtGetNextThread(this->context(), CURRENT_PROCESS, NULL_HANDLE, 0, 0, flags, no_output))
                << flags;
        }

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtGetNextThread(this->context(), NULL_HANDLE, NULL_HANDLE, 0, 0, 0, no_output));
    }

    TEST_F(SyscallArgumentValidationTest, GetNextThreadTellsWrongTypesFromInvalidHandles)
    {
        const auto event = this->create_event();

        handle next{};
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->next_thread(event, NULL_HANDLE, 0, next));
        EXPECT_EQ(NULL_HANDLE, next);
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->next_thread(CURRENT_PROCESS, event, 0, next));
        EXPECT_EQ(NULL_HANDLE, next);

        for (const auto process : {NULL_HANDLE, make_handle(0x1234)})
        {
            EXPECT_EQ(STATUS_INVALID_HANDLE, this->next_thread(process, NULL_HANDLE, 0, next)) << process.bits;
            EXPECT_EQ(NULL_HANDLE, next) << process.bits;
        }

        EXPECT_EQ(STATUS_INVALID_HANDLE, this->next_thread(CURRENT_PROCESS, make_handle(0x1234), 0, next));
        EXPECT_EQ(NULL_HANDLE, next);
    }

    TEST_F(SyscallArgumentValidationTest, GetNextThreadStillEnumeratesTheCurrentThread)
    {
        handle next{};
        EXPECT_EQ(STATUS_SUCCESS, this->next_thread(CURRENT_PROCESS, NULL_HANDLE, 0, next));
        EXPECT_EQ(this->emu->vcpu(0).active_thread, this->emu->process.threads.get(next));
    }

    TEST_F(SyscallArgumentValidationTest, CreateThreadRejectsUnknownCreateFlagsAsParameterSeven)
    {
        for (const ULONG flags : {0x81u, 0x101u, 0x201u, 0x401u, 0x801u, 0x1001u, 0x10001u, 0x80000001u, 0xFFFFFF81u})
        {
            handle thread{};
            EXPECT_EQ(STATUS_INVALID_PARAMETER_7, this->create_thread(CURRENT_PROCESS, flags, thread)) << flags;
            EXPECT_EQ(untouched, thread) << flags;
        }

        handle thread{};
        EXPECT_EQ(STATUS_INVALID_PARAMETER_7, this->create_thread(NULL_HANDLE, 0x81, thread));
        EXPECT_EQ(untouched, thread);
    }

    TEST_F(SyscallArgumentValidationTest, QueueApcRejectsFlagsOutsideTheSpecialAndCallbackDataBits)
    {
        for (const uint32_t flags : {0x2u, 0x4u, 0x8u, 0x100u, 0x8000u, 0x20000u, 0x40000000u, 0x80000000u, 0xFFFEFFFEu, 0xFFFFFFFFu})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->queue_apc(CURRENT_THREAD, flags)) << flags;
        }
        EXPECT_EQ(0u, this->pending_apc_count());

        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->queue_apc(NULL_HANDLE, 0x2));

        for (const uint32_t flags : {0x0u, 0x1u, 0x10000u, 0x10001u})
        {
            EXPECT_EQ(STATUS_SUCCESS, this->queue_apc(CURRENT_THREAD, flags)) << flags;
        }
        EXPECT_EQ(4u, this->pending_apc_count());
    }

    TEST_F(SyscallArgumentValidationTest, QueueApcTellsWrongTypesFromInvalidHandles)
    {
        EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->queue_apc(this->create_event(), 0));
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->queue_apc(NULL_HANDLE, 0));
    }

    TEST_F(SyscallArgumentValidationTest, UnmapViewExRejectsUnknownFlagsBeforeLookingAtTheAddress)
    {
        const auto private_memory = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);

        for (const ULONG flags : {0x4u, 0x8u, 0x10u, 0x100u, 0x80000000u, 0xFFFFFFFCu, 0xFFFFFFFFu})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER_3,
                      syscalls::handle_NtUnmapViewOfSectionEx(this->context(), CURRENT_PROCESS, private_memory, flags))
                << flags;
        }

        EXPECT_EQ(STATUS_INVALID_PARAMETER_3, syscalls::handle_NtUnmapViewOfSectionEx(this->context(), NULL_HANDLE, 0, 0x4));
        EXPECT_EQ(STATUS_NOT_MAPPED_VIEW, syscalls::handle_NtUnmapViewOfSectionEx(this->context(), CURRENT_PROCESS, private_memory, 0));
        EXPECT_EQ(STATUS_NOT_MAPPED_VIEW, syscalls::handle_NtUnmapViewOfSectionEx(this->context(), CURRENT_PROCESS, private_memory, 1));
    }

    TEST_F(SyscallArgumentValidationTest, SetSecurityObjectChecksTheDescriptorPointerBeforeTheHandle)
    {
        const auto event = this->create_event();

        for (const auto h : {event, NULL_HANDLE})
        {
            for (const SECURITY_INFORMATION information : {dacl_security_information, SECURITY_INFORMATION{0}})
            {
                EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtSetSecurityObject(this->context(), h, information, 0))
                    << h.bits << " " << information;
            }
        }
    }

    TEST_F(SyscallArgumentValidationTest, SetSecurityObjectRejectsHandlesThatDoNotResolve)
    {
        for (const auto h : {NULL_HANDLE, make_handle(0x1234)})
        {
            EXPECT_EQ(STATUS_INVALID_HANDLE,
                      syscalls::handle_NtSetSecurityObject(this->context(), h, dacl_security_information, this->storage + 0x400))
                << h.bits;
        }
    }
}

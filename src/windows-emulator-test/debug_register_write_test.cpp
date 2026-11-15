#include "emulation_test_utils.hpp"
#include <cpu_context.hpp>
#include <syscall_utils.hpp>
#include <thread_context.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtGetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtContinueEx(const syscall_context& c, uint64_t thread_context, uint64_t continue_argument);
    NTSTATUS handle_NtSetInformationThread(const syscall_context& c, handle thread_handle, THREADINFOCLASS info_class,
                                           uint64_t thread_information, uint32_t thread_information_length);
    NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, handle thread_handle, uint32_t info_class,
                                             uint64_t thread_information, uint32_t thread_information_length,
                                             emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t user_breakpoint = 0x1000;
        constexpr uint64_t highest_user_address = 0x7FFFFFFEFFFF;
        constexpr uint64_t above_user_addresses = 0x7FFFFFFF0000;
        constexpr uint64_t kernel_address = 0xFFFF800000000000;
        constexpr uint64_t all_ones = ~uint64_t{0};
        constexpr uint64_t dr6_status_bits = 0xE00F; // B0-B3, BD, BS, BT
        constexpr uint64_t dr7_reads_as_one = 0x400;
        constexpr uint64_t dr7_local_exact = 0x100;
        constexpr uint64_t dr7_local_enable_0 = 0x1;
        constexpr uint64_t continue_argument_offset = 0x800;
        constexpr uint64_t wow64_context_offset = 0x800;

        class DebugRegisterWriteTest : public testing::Test
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
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
                // No GDT before process setup: a context access then reads no descriptor for CS and stays 64-bit.
                this->cpu().load_gdt(0, 0);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            x86_64_cpu& cpu()
            {
                return this->emu->vcpu(0).x64_cpu();
            }

            emulator_object<CONTEXT64> write_junk_debug_registers()
            {
                const emulator_object<CONTEXT64> buffer{this->emu->memory, this->storage};
                CONTEXT64 value{};
                value.ContextFlags = CONTEXT_DEBUG_REGISTERS_64;
                value.Dr0 = user_breakpoint;
                value.Dr1 = above_user_addresses;
                value.Dr2 = highest_user_address;
                value.Dr3 = kernel_address;
                value.Dr6 = all_ones;
                value.Dr7 = all_ones;
                buffer.write(value);
                return buffer;
            }

            // DR6 and DR7 as the processor reads them back: DR6 with its reserved bits, DR7 with bit 10, both as 1.
            void expect_what_windows_keeps()
            {
                EXPECT_EQ(user_breakpoint, this->cpu().reg(x86_register::dr0));
                EXPECT_EQ(0u, this->cpu().reg(x86_register::dr1));
                EXPECT_EQ(highest_user_address, this->cpu().reg(x86_register::dr2));
                EXPECT_EQ(0u, this->cpu().reg(x86_register::dr3));
                EXPECT_EQ(0u, this->cpu().reg(x86_register::dr6) & dr6_status_bits);
                EXPECT_EQ(cpu_context::dr7_legal, this->cpu().reg(x86_register::dr7) & ~dr7_reads_as_one);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(DebugRegisterWriteTest, SetContextKeepsUserBreakpointsAndTheLegalDr7Bits)
    {
        const auto buffer = this->write_junk_debug_registers();
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetContextThread(this->context(), CURRENT_THREAD, buffer));
        this->expect_what_windows_keeps();
    }

    TEST_F(DebugRegisterWriteTest, ContinueKeepsUserBreakpointsAndTheLegalDr7Bits)
    {
        const auto buffer = this->write_junk_debug_registers();
        const auto continue_argument = this->storage + continue_argument_offset;
        this->emu->memory.write_memory(continue_argument, KCONTINUE_ARGUMENT{});
        syscalls::handle_NtContinueEx(this->context(), buffer.value(), continue_argument);
        this->expect_what_windows_keeps();
    }

    // A debugger stages a 32-bit breakpoint address before it enables it. RtlpWow64CtxToAmd64 sets DR7.LE for it, so the
    // address reads back through ThreadWow64Context and through a native context query.
    TEST_F(DebugRegisterWriteTest, Wow64ContextKeepsAStagedBreakpointWithLocalExactSet)
    {
        this->emu->process.is_wow64_process = true;
        const auto cpu_area = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        this->emu->vcpu(0).active_thread->wow64_cpu_reserved = emulator_object<WOW64_CPURESERVED>{this->emu->memory, cpu_area};

        const emulator_object<WOW64_CONTEXT> wow64_context{this->emu->memory, this->storage + wow64_context_offset};
        WOW64_CONTEXT staged{};
        staged.ContextFlags = CONTEXT_DEBUG_REGISTERS_32;
        staged.Dr0 = static_cast<uint32_t>(user_breakpoint);
        wow64_context.write(staged);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetInformationThread(this->context(), CURRENT_THREAD, ThreadWow64Context,
                                                                          wow64_context.value(), sizeof(WOW64_CONTEXT)));

        WOW64_CONTEXT requested{};
        requested.ContextFlags = CONTEXT_DEBUG_REGISTERS_32;
        wow64_context.write(requested);
        ASSERT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtQueryInformationThread(this->context(), CURRENT_THREAD, ThreadWow64Context, wow64_context.value(),
                                                            sizeof(WOW64_CONTEXT), {this->emu->memory}));
        EXPECT_EQ(user_breakpoint, wow64_context.read().Dr0);
        EXPECT_EQ(dr7_local_exact, wow64_context.read().Dr7);

        const emulator_object<CONTEXT64> native{this->emu->memory, this->storage};
        CONTEXT64 query{};
        query.ContextFlags = CONTEXT_DEBUG_REGISTERS_64;
        native.write(query);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtGetContextThread(this->context(), CURRENT_THREAD, native));
        EXPECT_EQ(user_breakpoint, native.read().Dr0);
        EXPECT_EQ(dr7_local_exact, native.read().Dr7 & ~dr7_reads_as_one);
    }

    // A debugger arms a 32-bit breakpoint on a thread stopped outside 32-bit code, e.g. in a system call. wow64cpu reloads
    // the saved WoW64 context without the debug registers, so PspWow64SetContextThread sets them on the native context.
    TEST_F(DebugRegisterWriteTest, Wow64ContextOfAThreadOutside32BitCodeArmsItsDebugRegisters)
    {
        this->emu->process.is_wow64_process = true;
        const auto cpu_area = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        auto& thread = *this->emu->vcpu(0).active_thread;
        thread.wow64_cpu_reserved = emulator_object<WOW64_CPURESERVED>{this->emu->memory, cpu_area};

        WOW64_CONTEXT armed{};
        armed.ContextFlags = CONTEXT_DEBUG_REGISTERS_32;
        armed.Dr0 = static_cast<uint32_t>(user_breakpoint);
        armed.Dr7 = static_cast<uint32_t>(dr7_local_enable_0);
        std::vector<uint8_t> bytes(sizeof(armed));
        memcpy(bytes.data(), &armed, sizeof(armed));
        ASSERT_TRUE(apply_wow64_thread_context(*this->emu, this->emu->vcpu(0), thread, bytes));

        const auto reported = capture_wow64_thread_context(*this->emu, this->emu->vcpu(0), thread);
        ASSERT_EQ(sizeof(WOW64_CONTEXT), reported.size());
        WOW64_CONTEXT wow64_context{};
        memcpy(&wow64_context, reported.data(), sizeof(wow64_context));
        EXPECT_EQ(user_breakpoint, wow64_context.Dr0);
        EXPECT_EQ(dr7_local_enable_0, wow64_context.Dr7);

        const emulator_object<CONTEXT64> native{this->emu->memory, this->storage};
        CONTEXT64 query{};
        query.ContextFlags = CONTEXT_DEBUG_REGISTERS_64;
        native.write(query);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtGetContextThread(this->context(), CURRENT_THREAD, native));
        EXPECT_EQ(user_breakpoint, native.read().Dr0);
        EXPECT_EQ(dr7_local_enable_0, native.read().Dr7 & ~dr7_reads_as_one);
    }
}

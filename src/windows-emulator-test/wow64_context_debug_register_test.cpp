#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, handle thread_handle, uint32_t info_class,
                                             uint64_t thread_information, uint32_t thread_information_length,
                                             emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t breakpoint = 0x10000;
        constexpr DWORD stored_junk = 0xDEADBEEF;
        constexpr uint64_t dr7_l0 = 0x1;
        constexpr uint64_t dr7_ge = 0x200;
        constexpr uint64_t reserved_offset = 0x1000;

        class Wow64ContextDebugRegisterTest : public testing::Test
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
                this->storage = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                auto* active = this->emu->process.threads.get(thread);
                this->emu->vcpu(0).active_thread = active;

                // A WoW64 thread whose saved 32-bit context holds the debug registers a ThreadWow64Context write stored.
                this->emu->process.is_wow64_process = true;
                const emulator_object<WOW64_CPURESERVED> reserved{this->emu->memory, this->storage + reserved_offset};
                WOW64_CPURESERVED value{};
                value.Context.Dr0 = value.Context.Dr1 = value.Context.Dr2 = value.Context.Dr3 = stored_junk;
                value.Context.Dr6 = value.Context.Dr7 = stored_junk;
                reserved.write(value);
                active->wow64_cpu_reserved = reserved;
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            x86_64_cpu& cpu()
            {
                return this->emu->vcpu(0).x64_cpu();
            }

            WOW64_CONTEXT query_debug_registers()
            {
                const emulator_object<WOW64_CONTEXT> buffer{this->emu->memory, this->storage};
                WOW64_CONTEXT value{};
                value.ContextFlags = CONTEXT_DEBUG_REGISTERS_32;
                buffer.write(value);
                EXPECT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtQueryInformationThread(this->context(), CURRENT_THREAD, ThreadWow64Context, buffer.value(),
                                                                    sizeof(WOW64_CONTEXT), {this->emu->memory, 0}));
                return buffer.read();
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(Wow64ContextDebugRegisterTest, TheQueryShowsNoDebugRegistersWhileDr7EnablesNone)
    {
        this->cpu().reg(x86_register::dr0, breakpoint);
        this->cpu().reg(x86_register::dr7, uint64_t{0});

        const auto context = this->query_debug_registers();
        EXPECT_EQ(0u, context.Dr0);
        EXPECT_EQ(0u, context.Dr1);
        EXPECT_EQ(0u, context.Dr2);
        EXPECT_EQ(0u, context.Dr3);
        EXPECT_EQ(0u, context.Dr6);
        EXPECT_EQ(0u, context.Dr7);
    }

    TEST_F(Wow64ContextDebugRegisterTest, TheQueryShowsTheNativeDebugRegistersWhileDr7EnablesOne)
    {
        this->cpu().reg(x86_register::dr0, breakpoint);
        this->cpu().reg(x86_register::dr1, uint64_t{0});
        this->cpu().reg(x86_register::dr2, uint64_t{0});
        this->cpu().reg(x86_register::dr3, uint64_t{0});
        this->cpu().reg(x86_register::dr7, dr7_l0 | dr7_ge);

        // A backend without debug registers reads them as 0 and drops a write. DR7 is not compared whole: the processor reads
        // its bit 10 as 1.
        if (this->cpu().reg(x86_register::dr0) != breakpoint || (this->cpu().reg(x86_register::dr7) & dr7_l0) == 0)
        {
            GTEST_SKIP() << "The " << this->emu->emu().get_name() << " backend keeps no debug registers";
        }

        const auto held_dr6 = static_cast<DWORD>(this->cpu().reg(x86_register::dr6));

        // RtlpWow64CtxFromAmd64 drops GE and DR7's bit 10, which the processor reads as 1.
        const auto context = this->query_debug_registers();
        EXPECT_EQ(breakpoint, context.Dr0);
        EXPECT_EQ(0u, context.Dr1);
        EXPECT_EQ(0u, context.Dr2);
        EXPECT_EQ(0u, context.Dr3);
        EXPECT_EQ(held_dr6, context.Dr6);
        EXPECT_EQ(dr7_l0, context.Dr7);
    }
}

#include "emulation_test_utils.hpp"
#include <syscall_dispatcher.hpp>
#include <syscall_utils.hpp>
#include <user_callback_dispatch.hpp>

namespace sogen::syscalls
{
    uint64_t handle_NtUserGetControlColor(const syscall_context& c, hwnd parent, hwnd window, hdc dc, uint32_t message);
    uint64_t completion_NtUserGetControlColor(const syscall_context& c, hwnd parent, hwnd window, hdc dc, uint32_t message);
    uint64_t handle_NtUserGetControlBrush(const syscall_context& c, hwnd window, hdc dc, uint32_t message);
    uint64_t completion_NtUserGetControlBrush(const syscall_context& c, hwnd window, hdc dc, uint32_t message);
    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context& c, uint32_t color, uint64_t unused);
    BOOL handle_NtUserFillWindow(const syscall_context& c, hwnd parent_window, hwnd window, hdc dc, hbrush brush);
    BOOL completion_NtUserFillWindow(const syscall_context& c, hwnd parent_window, hwnd window, hdc dc, hbrush brush);
    hdc handle_NtUserGetDC(const syscall_context& c, hwnd window);
    COLORREF handle_NtGdiGetPixel(const syscall_context& c, hdc dc, int x, int y);
}

namespace sogen::test
{
    namespace
    {
        struct dword_callback_args
        {
            uint64_t pwnd;
            uint32_t msg;
            uint64_t w_param;
            uint64_t l_param;
            uint64_t x_param;
            uint64_t xpfn_proc;
        };

        constexpr uint32_t k_wm_ctlcolormsgbox = 0x132;
        constexpr uint32_t k_wm_ctlcolorstatic = 0x138;
        constexpr hbrush k_ctlcolor_static = 6;
        constexpr COLORREF k_system_color = 0x00C0C0C0;
        constexpr COLORREF k_parent_color = 0x00112233;
        constexpr hdc k_dc = 0x4321;

        class ControlColorTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->parent = this->create_window(0, WS_POPUP | WS_VISIBLE);
                this->control = this->create_window(this->parent, WS_CHILD | WS_VISIBLE);

                const auto system_brush = syscalls::handle_NtGdiCreateSolidBrush(this->context(), k_system_color, 0);
                this->emu.process.user_handles.get_server_info().access(
                    [&](USER_SERVERINFO& server_info) { std::ranges::fill(server_info.ahbrSystem, system_brush); });
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            hwnd create_window(const hwnd parent_window, const uint32_t style)
            {
                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.parent_handle = parent_window != 0 ? parent_window : this->emu.process.default_desktop_window_handle.bits;
                win.thread_id = this->emu.vcpu(0).active_thread->id;
                win.style = style;
                win.wnd_proc = 0x1000;
                win.width = 20;
                win.height = 20;
                win.host_surface_window = parent_window == 0;
                win.guest.access([&](USER_WINDOW& guest) { guest.hWnd = handle.bits; });
                return win.handle;
            }

            // Checks that the parent was asked with the WM_CTLCOLOR* message and lets its window procedure answer.
            syscall_context answer_parent(const uint64_t parent_answer, const uint32_t message = k_wm_ctlcolorstatic, const hdc dc = k_dc)
            {
                const auto rsp = this->context().x64_cpu().read_stack_pointer();
                const auto callout = this->emu.memory.read_memory<user_callout_frame>(rsp);
                const auto args = this->emu.memory.read_memory<dword_callback_args>(callout.buffer);
                EXPECT_EQ(this->emu.process.windows.get(this->parent)->guest.value(), args.pwnd);
                EXPECT_EQ(message, args.msg);
                EXPECT_EQ(dc, args.w_param);
                EXPECT_EQ(this->control, args.l_param);

                auto& callbacks = this->emu.vcpu(0).active_thread->callback_stack;
                auto frame = std::move(callbacks.back());
                callbacks.pop_back();
                frame.restore_registers(this->emu.vcpu(0).cpu);

                auto completion = this->context();
                completion.is_callback_completion = true;
                completion.previous_callback_result.value = parent_answer;
                return completion;
            }

            uint64_t control_color(const uint64_t parent_answer)
            {
                auto call = this->context();
                (void)syscalls::handle_NtUserGetControlColor(call, this->parent, this->control, k_dc, k_wm_ctlcolorstatic);
                EXPECT_TRUE(call.run_callback);
                return syscalls::completion_NtUserGetControlColor(this->answer_parent(parent_answer), this->parent, this->control, k_dc,
                                                                  k_wm_ctlcolorstatic);
            }

            uint64_t control_brush(const uint64_t parent_answer)
            {
                auto call = this->context();
                (void)syscalls::handle_NtUserGetControlBrush(call, this->control, k_dc, k_wm_ctlcolorstatic);
                EXPECT_TRUE(call.run_callback);
                return syscalls::completion_NtUserGetControlBrush(this->answer_parent(parent_answer), this->control, k_dc,
                                                                  k_wm_ctlcolorstatic);
            }

            COLORREF fill_window(const uint64_t parent_answer)
            {
                const auto dc = syscalls::handle_NtUserGetDC(this->context(), this->control);
                auto call = this->context();
                (void)syscalls::handle_NtUserFillWindow(call, this->parent, this->control, dc, k_ctlcolor_static);
                EXPECT_TRUE(call.run_callback);
                const auto completion =
                    this->answer_parent(parent_answer, k_wm_ctlcolormsgbox + static_cast<uint32_t>(k_ctlcolor_static), dc);
                EXPECT_TRUE(syscalls::completion_NtUserFillWindow(completion, this->parent, this->control, dc, k_ctlcolor_static));
                return syscalls::handle_NtGdiGetPixel(this->context(), dc, 5, 5);
            }

            uint64_t system_brush()
            {
                return syscalls::handle_NtUserGetControlColor(this->context(), 0, this->control, k_dc, k_wm_ctlcolorstatic);
            }

            windows_emulator emu = create_sample_emulator();
            hwnd parent{};
            hwnd control{};
        };
    }

    TEST_F(ControlColorTest, TheParentChoosesTheControlBrush)
    {
        const auto brush = syscalls::handle_NtGdiCreateSolidBrush(this->context(), 0x00112233, 0);
        ASSERT_NE(0u, this->system_brush());
        ASSERT_NE(this->system_brush(), brush);

        EXPECT_EQ(brush, this->control_color(brush));
        EXPECT_EQ(brush, this->control_brush(brush));
    }

    TEST_F(ControlColorTest, AParentWithoutABrushLeavesTheSystemBrush)
    {
        EXPECT_EQ(this->system_brush(), this->control_color(0));
        EXPECT_EQ(this->system_brush(), this->control_color(0x1234));
        EXPECT_EQ(this->system_brush(), this->control_brush(0));
    }

    TEST_F(ControlColorTest, FillWindowPaintsWithTheBrushTheParentChoosesForTheControlType)
    {
        const auto brush = syscalls::handle_NtGdiCreateSolidBrush(this->context(), k_parent_color, 0);

        EXPECT_EQ(k_parent_color, this->fill_window(brush));
        EXPECT_EQ(k_system_color, this->fill_window(0));
        EXPECT_EQ(k_system_color, this->fill_window(0x1234));
    }

    // KeUserModeCallback's callout frame ends in a MACHINE_FRAME with the syscall's return RIP and its trap RSP, which
    // KiUserCallbackDispatcher's unwind data reads (UWOP_ALLOC_SMALL 0x30, then UWOP_PUSH_MACHFRAME).
    TEST_F(ControlColorTest, TheCallbackFrameHoldsWhereTheSyscallReturnsTo)
    {
        auto& cpu = this->emu.vcpu(0).x64_cpu();
        const auto syscall_rip = cpu.read_instruction_pointer();
        const auto trap_rsp = cpu.read_stack_pointer();

        auto call = this->context();
        call.trap_stack_pointer = trap_rsp;
        (void)syscalls::handle_NtUserGetControlColor(call, this->parent, this->control, k_dc, k_wm_ctlcolorstatic);
        ASSERT_TRUE(call.run_callback);

        const auto rsp = cpu.read_stack_pointer();
        const auto callout = this->emu.memory.read_memory<user_callout_frame>(rsp);
        EXPECT_EQ(rsp + sizeof(user_callout_frame), callout.buffer);
        EXPECT_EQ(syscall_rip + this->emu.abi().syscall_abi().syscall_instruction_size(), callout.machine_frame.Rip);
        EXPECT_EQ(trap_rsp, callout.machine_frame.Rsp);
    }
}

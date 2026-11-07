#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <window_show_orchestrator.hpp>

#include <functional>

namespace sogen::test
{
    namespace
    {
        class WindowShowEraseTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->dialog = this->create_window(0, WS_POPUP, {.left = 0, .top = 0, .right = 100, .bottom = 80});
                this->control =
                    this->create_window(this->dialog, WS_CHILD | WS_VISIBLE, {.left = 10, .top = 10, .right = 50, .bottom = 30});
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            hwnd create_window(const hwnd parent, const uint32_t style, const RECT& rect)
            {
                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.parent_handle = parent;
                win.style = style;
                win.x = rect.left;
                win.y = rect.top;
                win.width = rect.right - rect.left;
                win.height = rect.bottom - rect.top;
                win.host_surface_window = true;
                win.update_pending = true;
                return win.handle;
            }

            window& get(const hwnd handle)
            {
                return *this->emu.process.windows.get(handle);
            }

            // Runs the show sequence of the dialog and returns the windows that received WM_ERASEBKGND. on_step sees every
            // message before it would be dispatched, as the guest's window procedure would.
            std::vector<hwnd> erased_windows_when_shown(const std::function<void(const window_show_step&)>& on_step)
            {
                const auto c = this->context();
                window_show_data data{};
                const window_show_orchestrator orchestrator{data, c};
                orchestrator.start_show(this->get(this->dialog), SWP_SHOWWINDOW, true, true, false);
                this->get(this->dialog).style |= WS_VISIBLE;

                std::vector<hwnd> erased{};
                while (const auto step = orchestrator.advance())
                {
                    on_step(*step);
                    if (step->message.message == WM_ERASEBKGND)
                    {
                        erased.push_back(step->handle);
                        orchestrator.erase_background_completed(1);
                    }
                }
                return erased;
            }

            windows_emulator emu = create_sample_emulator();
            hwnd dialog{};
            hwnd control{};
        };
    }

    TEST_F(WindowShowEraseTest, ShowingErasesTheWindowsThatStillNeedPainting)
    {
        const auto erased = this->erased_windows_when_shown([](const window_show_step&) {});

        EXPECT_EQ((std::vector<hwnd>{this->dialog, this->control}), erased);
    }

    TEST_F(WindowShowEraseTest, WindowsPaintedDuringActivationAreNotErasedAgain)
    {
        const auto erased = this->erased_windows_when_shown([this](const window_show_step& step) {
            if (step.message.message == WM_ACTIVATE)
            {
                this->get(this->dialog).update_pending = false;
                this->get(this->control).update_pending = false;
            }
        });

        EXPECT_TRUE(erased.empty());
    }
}

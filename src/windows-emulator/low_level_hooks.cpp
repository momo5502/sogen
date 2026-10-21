#include "std_include.hpp"
#include "low_level_hooks.hpp"
#include "user_callback_dispatch.hpp"
#include "win32k_userconnect.hpp"
#include "windows_emulator.hpp"

namespace sogen
{
    namespace
    {
        constexpr int k_wh_keyboard_ll = 13;
        constexpr int k_hc_action = 0;
        constexpr uint32_t k_fn_hk_in_lp_kbdll_hook_struct_callback_id = 0x2D;
        constexpr uint32_t k_fn_hk_in_lp_msll_hook_struct_callback_id = 0x2E;
        constexpr size_t k_client_pfn_dispatch_hook_index = 19;

        constexpr DWORD k_llkhf_extended = 0x01;
        constexpr DWORD k_llkhf_altdown = 0x20;
        constexpr DWORD k_llkhf_up = 0x80;

        struct kbdll_hook_struct
        {
            DWORD vkCode{};
            DWORD scanCode{};
            DWORD flags{};
            DWORD time{};
            uint64_t dwExtraInfo{};
        };

        struct msll_hook_struct
        {
            POINT pt{};
            DWORD mouseData{};
            DWORD flags{};
            DWORD time{};
            uint64_t dwExtraInfo{};
        };

        static_assert(sizeof(kbdll_hook_struct) == 0x18);
        static_assert(sizeof(msll_hook_struct) == 0x20);

        // The argument block of user32's __fnHkINLPKBDLLHOOKSTRUCT/__fnHkINLPMSLLHOOKSTRUCT: it calls
        // dispatch(code, wParam, &hook_data, proc) and returns the result through NtCallbackReturn.
        template <typename HookData>
        struct fn_hk_in_ll_hook_message
        {
            UINT code{};
            wparam wParam{};
            pointer proc{};
            pointer dispatch{};
            HookData hook_data{};
        };

        static_assert(offsetof(fn_hk_in_ll_hook_message<kbdll_hook_struct>, hook_data) == 0x20);
        static_assert(offsetof(fn_hk_in_ll_hook_message<msll_hook_struct>, hook_data) == 0x20);

        uint64_t client_dispatch_hook(const syscall_context& c)
        {
            (void)win32k_userconnect::try_bootstrap_client_pfn_arrays_from_ntdll(c.win_emu);
            return c.proc.user_handles.get_server_info().read().apfnClientW[k_client_pfn_dispatch_hook_index];
        }

        kbdll_hook_struct make_keyboard_hook_data(const process_context::low_level_input& input)
        {
            const auto l_param = input.l_param;
            DWORD flags = 0;
            if ((l_param & (1ull << 24)) != 0)
            {
                flags |= k_llkhf_extended;
            }
            if ((l_param & (1ull << 29)) != 0)
            {
                flags |= k_llkhf_altdown;
            }
            if (input.message == WM_KEYUP || input.message == WM_SYSKEYUP)
            {
                flags |= k_llkhf_up;
            }

            return kbdll_hook_struct{
                .vkCode = static_cast<DWORD>(input.w_param & 0xFF),
                .scanCode = static_cast<DWORD>((l_param >> 16) & 0xFF),
                .flags = flags,
                .time = input.time,
            };
        }

        msll_hook_struct make_mouse_hook_data(const process_context::low_level_input& input)
        {
            DWORD mouse_data = 0;
            if (input.message == WM_MOUSEWHEEL || input.message == WM_MOUSEHWHEEL || input.message == WM_XBUTTONDOWN ||
                input.message == WM_XBUTTONUP || input.message == WM_XBUTTONDBLCLK)
            {
                mouse_data = static_cast<DWORD>(input.w_param & 0xFFFF0000);
            }

            return msll_hook_struct{
                .pt = POINT{.x = input.screen_x, .y = input.screen_y},
                .mouseData = mouse_data,
                .time = input.time,
            };
        }
    }

    bool dispatch_low_level_hook(const syscall_context& c, const callback_id completion_id)
    {
        const auto hook = c.win_emu.begin_low_level_hook(c.thread());
        if (!hook)
        {
            return false;
        }

        const auto& [input, entry] = *hook;
        const auto code = static_cast<UINT>(k_hc_action | (entry.id << 16));
        const auto dispatch = client_dispatch_hook(c);

        if (entry.id == k_wh_keyboard_ll)
        {
            const fn_hk_in_ll_hook_message<kbdll_hook_struct> args{
                .code = code,
                .wParam = input.message,
                .proc = entry.proc,
                .dispatch = dispatch,
                .hook_data = make_keyboard_hook_data(input),
            };
            dispatch_user_callback(c, completion_id, k_fn_hk_in_lp_kbdll_hook_struct_callback_id, args);
        }
        else
        {
            const fn_hk_in_ll_hook_message<msll_hook_struct> args{
                .code = code,
                .wParam = input.message,
                .proc = entry.proc,
                .dispatch = dispatch,
                .hook_data = make_mouse_hook_data(input),
            };
            dispatch_user_callback(c, completion_id, k_fn_hk_in_lp_msll_hook_struct_callback_id, args);
        }

        return true;
    }

    void complete_low_level_hook(const syscall_context& c)
    {
        c.win_emu.finish_low_level_hook(c.thread(), c.previous_callback_result.value != 0);
    }
}

#include "std_include.hpp"
#include "low_level_hooks.hpp"
#include "keyboard_layout.hpp"
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
        constexpr uint32_t k_fn_hk_in_lp_msg_callback_id = 0x2F;
        constexpr int k_wh_getmessage = 3;
        constexpr size_t k_client_pfn_dispatch_hook_index = 19;

        constexpr DWORD k_llkhf_extended = 0x01;
        constexpr DWORD k_llkhf_injected = 0x10;
        constexpr DWORD k_llkhf_altdown = 0x20;
        constexpr DWORD k_llkhf_up = 0x80;
        constexpr DWORD k_llmhf_injected = 0x01;

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

        // user32's __fnHkINLPMSG returns {result, size, &hook_data} through NtCallbackReturn, so the hook's changes to
        // the message come back as the callback output.
        struct fn_hk_in_lp_msg_message
        {
            UINT code{};
            wparam wParam{};
            pointer proc{};
            pointer dispatch{};
            msg hook_data{};
            DWORD flags{};
        };

        static_assert(offsetof(fn_hk_in_lp_msg_message, hook_data) == 0x20);
        static_assert(offsetof(fn_hk_in_lp_msg_message, flags) == 0x50);

        uint64_t client_dispatch_hook(const syscall_context& c, const bool ansi = false)
        {
            (void)win32k_userconnect::try_bootstrap_client_pfn_arrays_from_ntdll(c.win_emu);
            const auto server_info = c.proc.user_handles.get_server_info().read();
            return ansi ? server_info.apfnClientA[k_client_pfn_dispatch_hook_index]
                        : server_info.apfnClientW[k_client_pfn_dispatch_hook_index];
        }

        bool runs_get_message_hook(const emulator_thread& thread)
        {
            return std::ranges::any_of(thread.callback_stack, [](const callback_frame& frame) {
                return dynamic_cast<const get_message_hook_state*>(frame.state.get()) != nullptr;
            });
        }

        kbdll_hook_struct make_keyboard_hook_data(const process_context::low_level_input& input, const ui_event& event)
        {
            const auto l_param = event.lParam;
            DWORD flags = event.injected ? k_llkhf_injected : 0;
            if ((l_param & (1ull << 24)) != 0)
            {
                flags |= k_llkhf_extended;
            }
            if ((l_param & (1ull << 29)) != 0)
            {
                flags |= k_llkhf_altdown;
            }
            if (event.message == WM_KEYUP || event.message == WM_SYSKEYUP)
            {
                flags |= k_llkhf_up;
            }

            const auto virtual_key = static_cast<DWORD>(event.wParam & 0xFF);
            return kbdll_hook_struct{
                .vkCode = side_virtual_key(virtual_key, l_param),
                .scanCode =
                    virtual_key == VK_PACKET ? static_cast<DWORD>(event.injected_character) : static_cast<DWORD>((l_param >> 16) & 0xFF),
                .flags = flags,
                .time = input.time,
                .dwExtraInfo = event.extra_info,
            };
        }

        msll_hook_struct make_mouse_hook_data(const process_context::low_level_input& input, const ui_event& event)
        {
            DWORD mouse_data = 0;
            const auto message = event.message;
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
                message == WM_XBUTTONDBLCLK)
            {
                mouse_data = static_cast<DWORD>(event.wParam & 0xFFFF0000);
            }

            return msll_hook_struct{
                .pt = POINT{.x = input.screen_x, .y = input.screen_y},
                .mouseData = mouse_data,
                .flags = event.injected ? k_llmhf_injected : 0,
                .time = input.time,
                .dwExtraInfo = event.extra_info,
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
        const auto event = c.win_emu.resolve_injected_event(input.event);
        const auto code = static_cast<UINT>(k_hc_action | (entry.id << 16));
        const auto dispatch = client_dispatch_hook(c);

        if (entry.id == k_wh_keyboard_ll)
        {
            const fn_hk_in_ll_hook_message<kbdll_hook_struct> args{
                .code = code,
                .wParam = event.message,
                .proc = entry.proc,
                .dispatch = dispatch,
                .hook_data = make_keyboard_hook_data(input, event),
            };
            dispatch_user_callback(c, completion_id, k_fn_hk_in_lp_kbdll_hook_struct_callback_id, args);
        }
        else
        {
            const fn_hk_in_ll_hook_message<msll_hook_struct> args{
                .code = code,
                .wParam = event.message,
                .proc = entry.proc,
                .dispatch = dispatch,
                .hook_data = make_mouse_hook_data(input, event),
            };
            dispatch_user_callback(c, completion_id, k_fn_hk_in_lp_msll_hook_struct_callback_id, args);
        }

        return true;
    }

    void complete_low_level_hook(const syscall_context& c)
    {
        c.win_emu.finish_low_level_hook(c.thread(), c.previous_callback_result.value != 0);
    }

    bool dispatch_get_message_hook(const syscall_context& c, const callback_id completion_id, const msg& message, const UINT remove_flags)
    {
        const auto& thread = c.thread();
        if (!thread.get_message_hook || runs_get_message_hook(thread))
        {
            return false;
        }

        const auto& hook = *thread.get_message_hook;
        const fn_hk_in_lp_msg_message args{
            .code = static_cast<UINT>(k_hc_action | (k_wh_getmessage << 16)),
            .wParam = remove_flags,
            .proc = hook.proc,
            .dispatch = client_dispatch_hook(c, hook.ansi),
            .hook_data = message,
        };

        get_message_hook_state state{};
        state.message = message;
        dispatch_user_callback(c, completion_id, k_fn_hk_in_lp_msg_callback_id, std::move(state), args);
        return true;
    }

    msg complete_get_message_hook(const syscall_context& c)
    {
        auto message = c.get_completion_state<get_message_hook_state>().message;
        const auto& result = c.get_callback_result_object();
        if (result.output != 0 && result.output_size >= sizeof(message))
        {
            c.win_emu.memory.read_memory(result.output, &message, sizeof(message));
        }
        return message;
    }
}

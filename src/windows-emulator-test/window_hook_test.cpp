#include "std_include.hpp"

#include <limits>
#include <map>

#include <low_level_hooks.hpp>
#include <user_callback_dispatch.hpp>

namespace sogen::syscalls
{
    uint64_t handle_NtUserSetWindowsHookEx(const syscall_context& c, hinstance instance,
                                           emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> module, DWORD thread_id, int hook_id,
                                           pointer proc, BOOL ansi);
    uint64_t handle_NtUserSetWindowsHookAW(const syscall_context& c, int hook_id, pointer proc, BOOL ansi);
    BOOL handle_NtUserUnhookWindowsHookEx(const syscall_context& c, uint64_t hook);
    BOOL handle_NtUserUnhookWindowsHook(const syscall_context& c, int hook_id, pointer proc);
    lresult handle_NtUserCallNextHookEx(const syscall_context& c, int code, wparam w_param, lparam l_param, BOOL ansi);
    lresult completion_NtUserCallNextHookEx(const syscall_context& c, int code, wparam w_param, lparam l_param, BOOL ansi);
    BOOL handle_NtUserCallMsgFilter(const syscall_context& c, emulator_object<msg> message, int code);
    BOOL completion_NtUserCallMsgFilter(const syscall_context& c, emulator_object<msg> message, int code);
    BOOL handle_NtUserGetMessage(const syscall_context& c, emulator_object<msg> message, hwnd window, UINT filter_min, UINT filter_max);
    BOOL handle_NtUserPeekMessage(const syscall_context& c, emulator_object<msg> message, hwnd window, UINT filter_min, UINT filter_max,
                                  UINT remove_message);
    hwnd completion_NtUserCreateWindowEx(const syscall_context& c, DWORD ex_style, emulator_object<LARGE_STRING> class_name,
                                         emulator_object<LARGE_STRING> cls_version, emulator_object<LARGE_STRING> window_name, DWORD style,
                                         int x, int y, int width, int height, hwnd parent, hmenu menu, hinstance instance, pointer l_param,
                                         DWORD flags, pointer acbi_buffer);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t k_no_error = 0xDEAD;
        constexpr uint32_t k_error_not_enough_memory = 8;
        constexpr uint32_t k_error_access_denied = 5;
        constexpr uint32_t k_error_invalid_parameter = 87;
        constexpr uint32_t k_error_invalid_hook_handle = 1404;
        constexpr uint32_t k_error_invalid_hook_filter = 1426;
        constexpr uint32_t k_error_invalid_filter_proc = 1427;
        constexpr uint32_t k_error_hook_needs_hmod = 1428;
        constexpr uint32_t k_error_global_only_hook = 1429;
        constexpr uint32_t k_error_hook_not_installed = 1431;
        constexpr int k_wh_msgfilter = -1;
        constexpr int k_wh_keyboard = 2;
        constexpr int k_wh_getmessage = 3;
        constexpr int k_wh_cbt = 5;
        constexpr uint64_t k_failed_install = std::numeric_limits<uint64_t>::max();
        constexpr uint64_t k_hook_proc = 0x140001000;
        constexpr size_t k_phk_current_index = 6;
        constexpr size_t k_hook_current_index = 11;

        struct hook_callback_args
        {
            uint32_t code;
            uint64_t w_param;
            uint64_t proc;
            uint64_t dispatch;
        };

        struct retrieval_case
        {
            bool get_message;
            UINT remove_message;
            uint64_t hook_w_param;
        };

        struct install_case
        {
            int type;
            uint64_t proc;
            uint64_t module;
            uint32_t thread;
            uint64_t result;
            uint32_t error;
        };

        constexpr uint32_t k_current_thread = 0xCAFE0000;
        constexpr uint32_t k_gui_helper_thread = 0xCAFE0001;
        constexpr uint32_t k_non_gui_helper_thread = 0xCAFE0002;
        constexpr uint32_t k_exited_helper_thread = 0xCAFE0003;
        constexpr uint32_t k_gui_child_thread = 0xCAFE0004;
        constexpr uint32_t k_non_gui_child_thread = 0xCAFE0005;
        constexpr uint32_t k_exited_child_thread = 0xCAFE0006;
        constexpr uint32_t k_terminated_child_thread = 0xCAFE0007;
        constexpr uint64_t k_child_teb = 0x7FF000;

        struct child_thread_state
        {
            uint32_t thread_id;
            uint64_t win32_thread_info;
            bool process_exited;
            bool thread_exited;
        };

        class child_process_manager final : public process_manager
        {
          public:
            std::map<uint64_t, child_thread_state> children{};

            process_create_result create_process(process_create_request) override
            {
                return {.error = process_error::unavailable};
            }

            process_error resume_process(managed_process) override
            {
                return process_error::unavailable;
            }

            process_error terminate_process(managed_process, uint64_t) override
            {
                return process_error::unavailable;
            }

            process_exit_status_result exit_status(const managed_process process) const override
            {
                const auto child = this->children.find(process.value);
                if (child == this->children.end())
                {
                    return {.error = process_error::invalid_process};
                }
                return {.status = child->second.process_exited ? std::optional{process_exit{.kind = process_exit_kind::exited, .code = 0}}
                                                               : std::nullopt};
            }

            process_thread_information thread_information(const managed_process process, const uint32_t thread_id) override
            {
                const auto child = this->children.find(process.value);
                if (child == this->children.end() || child->second.thread_id != thread_id)
                {
                    return {.error = process_error::invalid_process};
                }
                return {.teb = k_child_teb, .exited = child->second.thread_exited};
            }

            process_memory_read_result read_memory(const managed_process process, const uint64_t address,
                                                   const uint64_t size) const override
            {
                const auto child = this->children.find(process.value);
                if (child == this->children.end() || address != k_child_teb + offsetof(TEB64, Win32ThreadInfo) || size != sizeof(uint64_t))
                {
                    return {.error = process_error::unavailable};
                }

                std::vector<uint8_t> data(sizeof(uint64_t));
                std::memcpy(data.data(), &child->second.win32_thread_info, data.size());
                return {.data = std::move(data)};
            }

            process_memory_result allocate_memory(managed_process, uint64_t, uint64_t, memory_permission, bool, bool) override
            {
                return {.error = process_error::unavailable};
            }

            process_error free_memory(managed_process, uint64_t, uint64_t, bool) override
            {
                return process_error::unavailable;
            }

            process_memory_result protect_memory(managed_process, uint64_t, uint64_t, memory_permission) override
            {
                return {.error = process_error::unavailable};
            }

            process_memory_result write_memory(managed_process, uint64_t, std::span<const uint8_t>) override
            {
                return {.error = process_error::unavailable};
            }
        };

        class WindowHookTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.processes = &this->children;
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));

                const auto base = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->emu->process.base_allocator = emulator_allocator{this->emu->memory, base, 0x10000};
                this->emu->process.user_handles.setup(false);
                this->main_thread = this->add_thread();
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(this->main_thread);
                const auto stack = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->context().x64_cpu().reg(x86_register::rsp, stack + 0x10000);
                const auto code = this->emu->memory.allocate_memory(0x1000, memory_permission::read_exec);
                this->context().x64_cpu().reg(x86_register::rip, code);
                this->hook_code = code;
                this->message_address = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);

                this->gui_helper = this->add_thread();
                this->emu->process.threads.get(this->gui_helper)->win32k_thread_info = 0x1000;
                this->non_gui_helper = this->add_thread();
                this->exited_helper = this->add_thread();
                auto& exited = *this->emu->process.threads.get(this->exited_helper);
                exited.win32k_thread_info = 0x1000;
                exited.exit_status = STATUS_SUCCESS;

                this->add_child_thread(k_gui_child_thread, 0x1000, false, false);
                this->add_child_thread(k_non_gui_child_thread, 0, false, false);
                this->add_child_thread(k_exited_child_thread, 0x1000, true, true);
                this->add_child_thread(k_terminated_child_thread, 0x1000, false, true);
            }

            void add_child_thread(const uint32_t thread_id, const uint64_t win32_thread_info, const bool process_exited,
                                  const bool thread_exited)
            {
                const auto process = static_cast<uint64_t>(this->children.children.size()) + 1;
                this->children.children[process] = {.thread_id = thread_id,
                                                    .win32_thread_info = win32_thread_info,
                                                    .process_exited = process_exited,
                                                    .thread_exited = thread_exited};

                managed_process_thread thread{};
                thread.process.value = process;
                thread.process_id = 0x4000 + static_cast<uint32_t>(process);
                thread.thread_id = thread_id;
                this->emu->process.managed_threads.store(std::move(thread));
            }

            handle add_thread()
            {
                const auto id = 8 + 4 * static_cast<uint32_t>(this->emu->process.threads.size());
                return this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, true});
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_thread& thread()
            {
                return *this->emu->vcpu(0).active_thread;
            }

            uint32_t thread_id(const uint32_t which)
            {
                switch (which)
                {
                case k_gui_helper_thread:
                    return this->emu->process.threads.get(this->gui_helper)->id;
                case k_non_gui_helper_thread:
                    return this->emu->process.threads.get(this->non_gui_helper)->id;
                case k_exited_helper_thread:
                    return this->emu->process.threads.get(this->exited_helper)->id;
                case k_current_thread:
                    return this->thread().id;
                default:
                    return which;
                }
            }

            uint32_t last_error()
            {
                return this->thread().teb64->read().LastErrorValue;
            }

            void clear_last_error()
            {
                this->thread().teb64->field<ULONG>(offsetof(TEB64, LastErrorValue)).write(k_no_error);
            }

            uint64_t install(const int type, const uint64_t proc, const uint64_t module, const uint32_t thread, const BOOL ansi = FALSE)
            {
                this->clear_last_error();
                const auto call = this->context();
                return syscalls::handle_NtUserSetWindowsHookEx(call, module, {this->emu->memory, 0}, thread, type, proc, ansi);
            }

            BOOL unhook_legacy(const int type, const uint64_t proc)
            {
                this->clear_last_error();
                const auto call = this->context();
                return syscalls::handle_NtUserUnhookWindowsHook(call, type, proc);
            }

            uint32_t fs_hooks()
            {
                return this->thread().teb64->field<uint32_t>(offsetof(TEB64, Win32ClientInfo) + 7 * sizeof(uint64_t)).read();
            }

            uint64_t install_legacy(const int type, const uint64_t proc, const BOOL ansi)
            {
                const auto call = this->context();
                return syscalls::handle_NtUserSetWindowsHookAW(call, type, proc, ansi);
            }

            BOOL unhook(const uint64_t hook)
            {
                this->clear_last_error();
                const auto call = this->context();
                return syscalls::handle_NtUserUnhookWindowsHookEx(call, hook);
            }

            bool runs_callback(const std::function<void(const syscall_context&)>& handler)
            {
                const auto call = this->context();
                handler(call);
                this->context().x64_cpu().reg(x86_register::rip, this->hook_code);
                return call.run_callback;
            }

            bool calls_next_hook(const uint64_t l_param)
            {
                return this->runs_callback(
                    [&](const syscall_context& call) { syscalls::handle_NtUserCallNextHookEx(call, 0, 0, l_param, TRUE); });
            }

            bool calls_next_hook()
            {
                return this->calls_next_hook(this->message_address);
            }

            bool filters_message()
            {
                return this->runs_callback([&](const syscall_context& call) {
                    syscalls::handle_NtUserCallMsgFilter(call, {this->emu->memory, this->message_address}, 0);
                });
            }

            bool retrieves_message(const retrieval_case& row)
            {
                const emulator_object<msg> message{this->emu->memory, this->message_address};
                constexpr UINT k_wm_user_1 = 0x401;
                this->thread().post_message(*this->emu, {.window = 0, .message = k_wm_user_1, .wParam = 0x100, .lParam = 0x22});
                return this->runs_callback([&](const syscall_context& call) {
                    if (row.get_message)
                    {
                        syscalls::handle_NtUserGetMessage(call, message, 0, 0, 0);
                    }
                    else
                    {
                        syscalls::handle_NtUserPeekMessage(call, message, 0, 0, 0, row.remove_message);
                    }
                });
            }

            syscall_context complete_hook()
            {
                auto& callbacks = this->thread().callback_stack;
                auto frame = std::move(callbacks.back());
                callbacks.pop_back();
                frame.restore_registers(this->emu->vcpu(0).cpu);
                this->completed_state = std::move(frame.state);
                auto completion = this->context();
                completion.is_callback_completion = true;
                completion.current_completion_state = this->completed_state.get();
                return completion;
            }

            uint64_t next_hook_handle()
            {
                const auto client_info = offsetof(TEB64, Win32ClientInfo);
                return this->thread().teb64->field<uint64_t>(client_info + k_phk_current_index * sizeof(uint64_t)).read();
            }

            uint32_t running_hook_word()
            {
                const auto client_info = offsetof(TEB64, Win32ClientInfo);
                return this->thread().teb64->field<uint32_t>(client_info + k_hook_current_index * sizeof(uint64_t)).read();
            }

            hook_callback_args dispatched_hook()
            {
                const auto rsp = this->context().x64_cpu().read_stack_pointer();
                const auto callout = this->emu->memory.read_memory<user_callout_frame>(rsp);
                return this->emu->memory.read_memory<hook_callback_args>(callout.buffer);
            }

            child_process_manager children{};
            std::unique_ptr<windows_emulator> emu{};
            handle main_thread{};
            handle gui_helper{};
            handle non_gui_helper{};
            handle exited_helper{};
            uint64_t hook_code{};
            uint64_t message_address{};
            std::unique_ptr<completion_state> completed_state{};
        };
    }

    TEST_F(WindowHookTest, InstallAndUnhookFollowTheWindowsErrorMatrix)
    {
        const std::vector<install_case> rejected{
            {-2, k_hook_proc, 0, k_current_thread, 0, k_error_invalid_hook_filter},
            {15, k_hook_proc, 0, k_current_thread, 0, k_error_invalid_hook_filter},
            {16, k_hook_proc, 0, k_current_thread, 0, k_error_invalid_hook_filter},
            {0x7FFF, k_hook_proc, 0, k_current_thread, 0, k_error_invalid_hook_filter},
            {0x10000, k_hook_proc, 0, k_current_thread, 0, k_error_invalid_hook_filter},
            {std::numeric_limits<int>::min(), k_hook_proc, 0, k_current_thread, 0, k_error_invalid_hook_filter},
            {std::numeric_limits<int>::max(), k_hook_proc, 0, k_current_thread, 0, k_error_invalid_hook_filter},
            {6, k_hook_proc, 0, k_current_thread, 0, k_error_global_only_hook},
            {13, k_hook_proc, 0, k_current_thread, 0, k_error_global_only_hook},
            {14, k_hook_proc, 0, k_current_thread, 0, k_error_global_only_hook},
            {6, k_hook_proc, 0, k_gui_helper_thread, 0, k_error_global_only_hook},
            {13, k_hook_proc, 0, k_gui_helper_thread, 0, k_error_global_only_hook},
            {14, k_hook_proc, 0, k_gui_helper_thread, 0, k_error_global_only_hook},
            {6, k_hook_proc, 0, 0x7FFFFFFC, 0, k_error_invalid_parameter},
            {13, k_hook_proc, 0, 0x7FFFFFFC, 0, k_error_invalid_parameter},
            {14, k_hook_proc, 0, 0x7FFFFFFC, 0, k_error_invalid_parameter},
            {6, k_hook_proc, 0, k_non_gui_helper_thread, 0, k_error_invalid_parameter},
            {13, k_hook_proc, 0, k_non_gui_helper_thread, 0, k_error_invalid_parameter},
            {14, k_hook_proc, 0, k_non_gui_helper_thread, 0, k_error_invalid_parameter},
            {6, k_hook_proc, 0, k_exited_helper_thread, 0, k_error_invalid_parameter},
            {13, k_hook_proc, 0, k_exited_helper_thread, 0, k_error_invalid_parameter},
            {14, k_hook_proc, 0, k_exited_helper_thread, 0, k_error_invalid_parameter},
            {6, k_hook_proc, 0, k_gui_child_thread, 0, k_error_global_only_hook},
            {13, k_hook_proc, 0, k_gui_child_thread, 0, k_error_global_only_hook},
            {14, k_hook_proc, 0, k_gui_child_thread, 0, k_error_global_only_hook},
            {6, k_hook_proc, 0, k_non_gui_child_thread, 0, k_error_invalid_parameter},
            {13, k_hook_proc, 0, k_non_gui_child_thread, 0, k_error_invalid_parameter},
            {14, k_hook_proc, 0, k_non_gui_child_thread, 0, k_error_invalid_parameter},
            {6, k_hook_proc, 0, k_exited_child_thread, 0, k_error_invalid_parameter},
            {13, k_hook_proc, 0, k_exited_child_thread, 0, k_error_invalid_parameter},
            {14, k_hook_proc, 0, k_exited_child_thread, 0, k_error_invalid_parameter},
            {6, k_hook_proc, 0, k_terminated_child_thread, 0, k_error_invalid_parameter},
            {13, k_hook_proc, 0, k_terminated_child_thread, 0, k_error_invalid_parameter},
            {14, k_hook_proc, 0, k_terminated_child_thread, 0, k_error_invalid_parameter},
            {6, k_hook_proc, 0, 0, 0, k_error_hook_needs_hmod},
            {7, 0, 0, k_current_thread, 0, k_error_invalid_filter_proc},
            {3, 0, 0, k_current_thread, 0, k_error_invalid_filter_proc},
            {5, 0, 0, k_current_thread, 0, k_error_invalid_filter_proc},
            {3, k_hook_proc, 0, 0, 0, k_error_hook_needs_hmod},
            {7, k_hook_proc, 0, 0, 0, k_error_hook_needs_hmod},
            {7, k_hook_proc, 0, 0xFFFFFFFF, 0, k_error_invalid_parameter},
            {7, k_hook_proc, 0, 0x7FFFFFFC, 0, k_error_invalid_parameter},
            {7, k_hook_proc, 0, 0x80000000, 0, k_error_invalid_parameter},
            {7, k_hook_proc, 0, k_non_gui_helper_thread, 0, k_error_invalid_parameter},
            {3, k_hook_proc, 0, k_non_gui_helper_thread, 0, k_error_invalid_parameter},
            {7, k_hook_proc, 0, k_exited_helper_thread, 0, k_error_invalid_parameter},
        };

        for (const auto& row : rejected)
        {
            EXPECT_EQ(row.result, this->install(row.type, row.proc, row.module, this->thread_id(row.thread)))
                << "type " << row.type << " thread " << row.thread;
            EXPECT_EQ(row.error, this->last_error()) << "type " << row.type << " thread " << row.thread;
        }

        constexpr std::array accepted{-1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12};
        uint64_t previous_uniqueness = 0;
        for (const auto type : accepted)
        {
            const auto hook = this->install(type, k_hook_proc, 0, this->thread_id(k_current_thread));
            EXPECT_NE(0u, hook) << "type " << type;
            EXPECT_EQ(k_no_error, this->last_error()) << "type " << type;
            EXPECT_EQ(0u, hook >> 32) << "type " << type;
            EXPECT_NE(0u, hook & 0xFFFF) << "type " << type;
            EXPECT_NE(previous_uniqueness, hook >> 16) << "type " << type;
            previous_uniqueness = hook >> 16;

            EXPECT_EQ(TRUE, this->unhook(hook)) << "type " << type;
            EXPECT_EQ(k_no_error, this->last_error()) << "type " << type;
            EXPECT_EQ(FALSE, this->unhook(hook)) << "type " << type;
            EXPECT_EQ(k_error_invalid_hook_handle, this->last_error()) << "type " << type;
        }

        for (const auto type : {13, 14})
        {
            const auto hook = this->install(type, k_hook_proc, 0, 0);
            EXPECT_NE(0u, hook) << "type " << type;
            EXPECT_EQ(k_no_error, this->last_error()) << "type " << type;
            EXPECT_EQ(TRUE, this->unhook(hook)) << "type " << type;
        }

        EXPECT_NE(0u, this->install(7, k_hook_proc, 0x7ff751520000, this->thread_id(k_current_thread)));
        EXPECT_NE(0u, this->install(7, k_hook_proc, 0, this->thread_id(k_gui_helper_thread)));

        const auto hook = this->install(7, k_hook_proc, 0, this->thread_id(k_current_thread));
        const std::vector<uint64_t> stale{
            0, 1, 0xDEAD, 0x10001, ~0ull, hook + 0x10000, hook & 0xFFFF, hook - 1, hook + 1,
        };
        for (const auto handle : stale)
        {
            EXPECT_EQ(FALSE, this->unhook(handle)) << handle;
            EXPECT_EQ(k_error_invalid_hook_handle, this->last_error()) << handle;
        }

        EXPECT_EQ(TRUE, this->unhook(hook | (1ull << 56)));

        for (const auto matching : {0x80000000ull, 0x7FFF0000ull})
        {
            const auto masked = this->install(7, k_hook_proc, 0, this->thread_id(k_current_thread));
            EXPECT_EQ(TRUE, this->unhook(matching == 0x80000000ull ? masked ^ matching : (masked & 0xFFFF) | matching));
        }

        const auto wow64_hook = this->install(7, k_hook_proc, 0, this->thread_id(k_current_thread));
        EXPECT_EQ(FALSE, this->unhook(wow64_hook & 0xFFFF));
        this->emu->process.is_wow64_process = true;
        EXPECT_EQ(TRUE, this->unhook(wow64_hook & 0xFFFF));
        this->emu->process.is_wow64_process = false;

        const auto exiting = this->install(3, k_hook_proc, 0, this->thread_id(k_gui_helper_thread));
        this->emu->process.threads.get(this->gui_helper)->exit_status = STATUS_SUCCESS;
        EXPECT_EQ(FALSE, this->unhook(exiting));
        EXPECT_EQ(k_no_error, this->last_error());

        for (const auto ansi : {FALSE, TRUE})
        {
            this->thread().thread_hooks.clear();
            EXPECT_NE(0u, this->install_legacy(k_wh_msgfilter, k_hook_proc, ansi));
            EXPECT_NE(0u, this->install_legacy(k_wh_msgfilter, k_hook_proc, ansi));

            this->thread().thread_hooks.clear();
            EXPECT_EQ(0u, this->install_legacy(k_wh_getmessage, k_hook_proc, ansi));
            const auto second = this->install_legacy(k_wh_getmessage, k_hook_proc, ansi);
            EXPECT_NE(0u, second);
            EXPECT_NE(k_failed_install, second);
            EXPECT_EQ(2u, this->thread().thread_hooks.size());

            this->thread().thread_hooks.clear();
            EXPECT_EQ(0u, this->install_legacy(k_wh_cbt, k_hook_proc, ansi));
            EXPECT_NE(0u, this->install_legacy(k_wh_cbt, k_hook_proc, ansi));
        }
        EXPECT_EQ(k_failed_install, this->install_legacy(16, k_hook_proc, TRUE));

        this->thread().thread_hooks.clear();
        EXPECT_NE(0u, this->install_legacy(k_wh_keyboard, k_hook_proc, TRUE));

        this->thread().thread_hooks.clear();
        const auto running_hook = this->install(k_wh_getmessage, k_hook_proc, 0, this->thread_id(k_current_thread));
        enter_hook(this->context(), *this->thread().find_hook(running_hook));
        EXPECT_EQ(TRUE, this->unhook(running_hook));
        EXPECT_NE(0u, this->install_legacy(k_wh_getmessage, k_hook_proc, TRUE));
        leave_hook(this->context());

        this->thread().thread_hooks.clear();
        const auto older = this->install(k_wh_getmessage, k_hook_proc, 0, this->thread_id(k_current_thread));
        const auto newer = this->install(k_wh_getmessage, k_hook_proc, 0, this->thread_id(k_current_thread));
        EXPECT_EQ(FALSE, this->unhook_legacy(99, k_hook_proc));
        EXPECT_EQ(k_error_invalid_hook_filter, this->last_error());
        EXPECT_EQ(FALSE, this->unhook_legacy(k_wh_getmessage, k_hook_proc + 8));
        EXPECT_EQ(k_error_hook_not_installed, this->last_error());
        EXPECT_EQ(TRUE, this->unhook_legacy(k_wh_getmessage, k_hook_proc));
        EXPECT_EQ(FALSE, this->unhook(newer));
        EXPECT_EQ(TRUE, this->unhook(older));

        this->thread().thread_hooks.clear();
        for (uint64_t index = 1; index <= 0xFFFF; ++index)
        {
            this->thread().thread_hooks.push_back(user_thread_hook{
                .handle = (1ull << 16) | index,
                .id = 7,
                .proc = k_hook_proc,
                .ansi = false,
                .installer = 0,
                .destroyed = false,
            });
        }
        EXPECT_EQ(0u, this->install(7, k_hook_proc, 0, this->thread_id(k_current_thread)));
        EXPECT_EQ(k_error_not_enough_memory, this->last_error());
    }

    TEST_F(WindowHookTest, GetMessageHooksSeeTheRawRetrievalFlags)
    {
        constexpr UINT k_pm_remove = 0x1;
        constexpr UINT k_pm_noyield = 0x2;
        constexpr UINT k_pm_qs_postmessage = 0x980000;

        const std::vector<retrieval_case> retrievals{
            {false, 0, 0x0},
            {false, k_pm_remove, 0x1},
            {false, k_pm_remove | k_pm_noyield, 0x3},
            {false, k_pm_remove | k_pm_qs_postmessage, 0x980001},
            {true, 0, 0x1},
        };

        this->install(k_wh_getmessage, k_hook_proc, 0, this->thread_id(k_current_thread));
        for (size_t i = 0; i < retrievals.size(); ++i)
        {
            this->thread().message_queue.clear();
            ASSERT_TRUE(this->retrieves_message(retrievals[i])) << i;
            EXPECT_EQ(retrievals[i].hook_w_param, this->dispatched_hook().w_param) << i;
            this->complete_hook();
            leave_hook(this->context());
        }
    }

    TEST_F(WindowHookTest, HookChainsRunNewestFirstAndCallNextHookExContinuesThemUntilTheirInstallerEnds)
    {
        constexpr uint64_t k_oldest = k_hook_proc;
        constexpr uint64_t k_middle = k_hook_proc + 0x100;
        constexpr uint64_t k_newest = k_hook_proc + 0x200;
        constexpr uint32_t k_msgfilter_word = 0xFFFF0000;
        const auto thread = this->thread_id(k_current_thread);
        const auto oldest_hook = this->install(k_wh_msgfilter, k_oldest, 0, thread);
        const auto middle_hook = this->install(k_wh_msgfilter, k_middle, 0, thread);
        const auto newest_hook = this->install(k_wh_msgfilter, k_newest, 0, thread);

        EXPECT_FALSE(this->calls_next_hook());
        EXPECT_EQ(0u, this->next_hook_handle());

        ASSERT_TRUE(this->filters_message());
        EXPECT_EQ(k_newest, this->dispatched_hook().proc);
        EXPECT_EQ(middle_hook, this->next_hook_handle());
        EXPECT_EQ(k_msgfilter_word, this->running_hook_word());
        EXPECT_FALSE(this->calls_next_hook(0));
        EXPECT_FALSE(this->calls_next_hook(0x10));

        ASSERT_TRUE(this->calls_next_hook());
        EXPECT_EQ(k_middle, this->dispatched_hook().proc);
        EXPECT_EQ(oldest_hook, this->next_hook_handle());

        ASSERT_TRUE(this->calls_next_hook());
        EXPECT_EQ(k_oldest, this->dispatched_hook().proc);
        EXPECT_EQ(0u, this->next_hook_handle());

        EXPECT_FALSE(this->calls_next_hook());

        constexpr uint64_t k_chain_result = 0x77;
        const std::array enclosing{oldest_hook, middle_hook};
        for (const auto expected : enclosing)
        {
            auto completion = this->complete_hook();
            completion.previous_callback_result.value = k_chain_result;
            EXPECT_EQ(k_chain_result, syscalls::completion_NtUserCallNextHookEx(completion, 0, 0, this->message_address, TRUE));
            EXPECT_EQ(expected, this->next_hook_handle());
        }

        auto completion = this->complete_hook();
        completion.previous_callback_result.value = 1;
        EXPECT_EQ(TRUE, syscalls::completion_NtUserCallMsgFilter(completion, {this->emu->memory, this->message_address}, 0));
        EXPECT_TRUE(this->thread().running_hooks.empty());
        EXPECT_EQ(0u, this->next_hook_handle());
        EXPECT_EQ(0u, this->running_hook_word());

        EXPECT_EQ(TRUE, this->unhook(middle_hook));
        ASSERT_TRUE(this->filters_message());
        EXPECT_EQ(k_newest, this->dispatched_hook().proc);
        EXPECT_EQ(oldest_hook, this->next_hook_handle());

        EXPECT_EQ(TRUE, this->unhook(newest_hook));
        ASSERT_TRUE(this->calls_next_hook());
        EXPECT_EQ(k_oldest, this->dispatched_hook().proc);

        auto nested = this->complete_hook();
        syscalls::completion_NtUserCallNextHookEx(nested, 0, 0, this->message_address, TRUE);
        auto finished = this->complete_hook();
        EXPECT_EQ(FALSE, syscalls::completion_NtUserCallMsgFilter(finished, {this->emu->memory, this->message_address}, 0));
        EXPECT_EQ(nullptr, this->thread().find_hook(newest_hook));
        EXPECT_EQ(0u, this->next_hook_handle());
        ASSERT_TRUE(this->filters_message());
        EXPECT_EQ(k_oldest, this->dispatched_hook().proc);

        constexpr uint64_t k_message_size = 0x30;
        const auto modified_address = this->message_address + 0x200;
        msg modified{};
        modified.message = 0x44d;
        this->emu->memory.write_memory(modified_address, &modified, sizeof(modified));

        for (const auto type : {k_wh_getmessage, k_wh_msgfilter})
        {
            const auto current_thread_id = this->thread_id(k_current_thread);
            this->thread().thread_hooks.clear();
            this->install(type, k_hook_proc, 0, current_thread_id);
            this->install(type, k_hook_proc + 0x100, 0, current_thread_id);
            this->emu->memory.write_memory(this->message_address, std::array<std::byte, k_message_size>{}.data(), k_message_size);

            const auto call = this->context();
            enter_hook(call, *this->thread().newest_hook(type));
            ASSERT_TRUE(this->calls_next_hook());
            auto copied = this->complete_hook();
            copied.previous_callback_result.output = modified_address;
            copied.previous_callback_result.output_size = k_message_size;
            syscalls::completion_NtUserCallNextHookEx(copied, 0, 0, this->message_address, TRUE);
            leave_hook(this->context());

            EXPECT_EQ(type == k_wh_getmessage ? 0x44du : 0u, this->emu->memory.read_memory<msg>(this->message_address).message);
        }

        const auto guarded = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        this->thread().thread_hooks.clear();
        this->install(k_wh_getmessage, k_hook_proc, 0, this->thread_id(k_current_thread));
        this->install(k_wh_getmessage, k_hook_proc + 0x100, 0, this->thread_id(k_current_thread));
        enter_hook(this->context(), *this->thread().newest_hook(k_wh_getmessage));

        this->emu->memory.protect_memory(guarded, 0x1000, memory_permission::read);
        EXPECT_FALSE(this->calls_next_hook(guarded));
        this->emu->memory.protect_memory(guarded, 0x1000, memory_permission::none);
        EXPECT_FALSE(this->calls_next_hook(guarded));
        this->emu->memory.protect_memory(guarded, 0x1000, memory_permission::read_write);

        ASSERT_TRUE(this->calls_next_hook(guarded));
        this->emu->memory.protect_memory(guarded, 0x1000, memory_permission::read);
        auto freed = this->complete_hook();
        freed.previous_callback_result.value = 0x55;
        freed.previous_callback_result.output = modified_address;
        freed.previous_callback_result.output_size = k_message_size;
        EXPECT_EQ(0u, syscalls::completion_NtUserCallNextHookEx(freed, 0, 0, guarded, TRUE));
        EXPECT_EQ(0u, this->emu->memory.read_memory<msg>(guarded).message);
        this->emu->memory.protect_memory(guarded, 0x1000, memory_permission::read_write);

        const user_low_level_hook low_level{.handle = 0x7FFE0001, .id = 13, .proc = k_hook_proc};
        enter_low_level_hook(this->context(), low_level);
        EXPECT_FALSE(this->calls_next_hook());
        leave_hook(this->context());
        leave_hook(this->context());

        this->thread().thread_hooks.clear();
        this->emu->process.threads.get(this->main_thread)->win32k_thread_info = 0x1000;
        this->emu->vcpu(0).active_thread = this->emu->process.threads.get(this->gui_helper);
        const auto main_id = this->emu->process.threads.get(this->main_thread)->id;
        const auto helper_hook = this->install(k_wh_getmessage, k_hook_proc, 0, main_id);
        ASSERT_NE(0u, helper_hook);
        ASSERT_NE(0u, this->install(k_wh_msgfilter, k_hook_proc, 0, main_id));
        this->emu->vcpu(0).active_thread = this->emu->process.threads.get(this->main_thread);
        ASSERT_NE(nullptr, this->thread().find_hook(helper_hook));
        EXPECT_EQ(1u, this->fs_hooks());

        EXPECT_EQ(FALSE, this->unhook_legacy(k_wh_getmessage, k_hook_proc));
        EXPECT_EQ(k_error_access_denied, this->last_error());
        ASSERT_NE(nullptr, this->thread().find_hook(helper_hook));

        this->emu->process.terminate_thread(*this->emu->process.threads.get(this->gui_helper), STATUS_SUCCESS);
        EXPECT_EQ(nullptr, this->thread().find_hook(helper_hook));
        EXPECT_EQ(0u, this->fs_hooks());
        EXPECT_FALSE(this->filters_message());
        EXPECT_EQ(FALSE, this->unhook(helper_hook));
        EXPECT_EQ(k_error_invalid_hook_handle, this->last_error());
    }

    TEST_F(WindowHookTest, WindowCreateHooksSeeTheirOwnEncodingAndLeaveNoRunningHookBehind)
    {
        constexpr uint32_t k_vanished_window = 0x1234;
        const auto hook_handle = this->install(k_wh_cbt, k_hook_proc, 0, this->thread_id(k_current_thread));
        const auto* hook = this->thread().find_hook(hook_handle);
        ASSERT_NE(nullptr, hook);

        const auto call = this->context();
        auto& cpu = call.x64_cpu();
        window_create_state state{};
        state.handle = k_vanished_window;
        state.phase = window_create_phase::cbt_create;
        state.create_struct_alloc = cpu.push_stack(EMU_CREATESTRUCT{});
        state.window_rect_alloc = cpu.push_stack(RECT{});
        state.min_max_info_alloc = cpu.push_stack(EMU_MINMAXINFO{});
        enter_hook(call, *hook);

        auto completion = this->context();
        completion.is_callback_completion = true;
        completion.current_completion_state = &state;
        const emulator_object<LARGE_STRING> no_string{this->emu->memory, 0};
        EXPECT_EQ(
            0u, syscalls::completion_NtUserCreateWindowEx(completion, 0, no_string, no_string, no_string, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));

        EXPECT_TRUE(this->thread().running_hooks.empty());
        EXPECT_EQ(0u, this->next_hook_handle());
        EXPECT_FALSE(this->calls_next_hook());

        this->thread().thread_hooks.clear();
        constexpr uint64_t k_atom = 0xC001;
        const auto window = this->emu->process.windows.create(this->emu->memory);
        const auto text_address = this->message_address + 0x400;
        const auto create_struct_address = this->message_address + 0x500;
        const auto cbt_address = this->message_address + 0x600;
        const std::array<char, 3> ansi_title{'A', 'b', '\0'};
        this->emu->memory.write_memory(text_address, ansi_title.data(), ansi_title.size());
        EMU_CREATESTRUCT create_struct{};
        create_struct.lpszName = text_address;
        create_struct.lpszClass = k_atom;
        this->emu->memory.write_memory(create_struct_address, &create_struct, sizeof(create_struct));
        const std::array<uint64_t, 2> cbt_create{create_struct_address, 0};
        this->emu->memory.write_memory(cbt_address, cbt_create.data(), sizeof(cbt_create));

        const auto denied = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        this->emu->memory.protect_memory(denied, 0x1000, memory_permission::none);
        const auto read_only = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto unreadable_name_struct = this->message_address + 0x700;
        const auto unreadable_name_cbt = this->message_address + 0x780;
        const auto read_only_cbt = this->message_address + 0x7C0;
        const auto low_word_name_struct = this->message_address + 0x800;
        const auto low_word_name_cbt = low_word_name_struct + sizeof(EMU_CREATESTRUCT);
        EMU_CREATESTRUCT unreadable_name{};
        unreadable_name.lpszName = denied;
        this->emu->memory.write_memory(unreadable_name_struct, &unreadable_name, sizeof(unreadable_name));
        this->emu->memory.write_memory(read_only, &create_struct, sizeof(create_struct));
        this->emu->memory.protect_memory(read_only, 0x1000, memory_permission::read);
        EMU_CREATESTRUCT low_word_name{};
        low_word_name.lpszName = 0x1234;
        low_word_name.lpszClass = k_atom;
        this->emu->memory.write_memory(low_word_name_struct, &low_word_name, sizeof(low_word_name));
        const std::array<uint64_t, 2> low_word_name_create{low_word_name_struct, 0};
        this->emu->memory.write_memory(low_word_name_cbt, low_word_name_create.data(), sizeof(low_word_name_create));
        const std::array<uint64_t, 2> unreadable_name_create{unreadable_name_struct, 0};
        const std::array<uint64_t, 2> read_only_create{read_only, 0};
        this->emu->memory.write_memory(unreadable_name_cbt, unreadable_name_create.data(), sizeof(unreadable_name_create));
        this->emu->memory.write_memory(read_only_cbt, read_only_create.data(), sizeof(read_only_create));

        const auto thread = this->thread_id(k_current_thread);
        this->install(k_wh_cbt, k_hook_proc, 0, thread, FALSE);
        this->install(k_wh_cbt, k_hook_proc + 0x100, 0, thread, TRUE);
        const auto newest = this->thread().newest_hook(k_wh_cbt);
        enter_hook(this->context(), *newest);
        constexpr int k_hcbt_createwnd = 3;
        for (const auto rejected_cbt : {unreadable_name_cbt, low_word_name_cbt, read_only_cbt, uint64_t{0}})
        {
            EXPECT_FALSE(this->runs_callback([&](const syscall_context& next) {
                syscalls::handle_NtUserCallNextHookEx(next, k_hcbt_createwnd, window.first.bits, rejected_cbt, FALSE);
            }));
        }
        ASSERT_TRUE(this->runs_callback([&](const syscall_context& next) {
            syscalls::handle_NtUserCallNextHookEx(next, k_hcbt_createwnd, window.first.bits, cbt_address, TRUE);
        }));

        const auto rsp = this->context().x64_cpu().read_stack_pointer();
        const auto callout = this->emu->memory.read_memory<user_callout_frame>(rsp);
        const auto dispatched = this->emu->memory.read_memory<EMU_CREATESTRUCT>(callout.buffer + 0x40);
        const auto title = read_string<char16_t>(this->emu->memory, dispatched.lpszName);
        EXPECT_EQ(u"Ab", title);
        EXPECT_EQ(k_atom, dispatched.lpszClass);

        auto finished = this->complete_hook();
        syscalls::completion_NtUserCallNextHookEx(finished, k_hcbt_createwnd, window.first.bits, cbt_address, TRUE);
        char converted_byte{};
        EXPECT_FALSE(this->emu->memory.try_read_memory(dispatched.lpszName, &converted_byte, 1));
    }
}

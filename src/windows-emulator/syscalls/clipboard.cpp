#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../clipboard_messages.hpp"

#include <utility>

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            constexpr uint32_t error_success = 0;
            constexpr uint32_t error_access_denied = 5;
            constexpr uint32_t error_invalid_handle = 6;
            constexpr uint32_t error_invalid_parameter = 87;
            constexpr uint32_t error_noaccess = 998;
            constexpr uint32_t error_insufficient_buffer = 122;
            constexpr uint32_t error_not_found = 1168;
            constexpr uint32_t error_invalid_window_handle = 1400;
            constexpr uint32_t error_clipboard_not_open = 1418;

            clipboard_state& live_clipboard(const syscall_context& c)
            {
                auto& clipboard = c.proc.clipboard;
                if (clipboard.opening)
                {
                    const auto* opener = c.proc.find_thread_by_id(clipboard.opening->thread_id);
                    if (!opener || opener->is_terminated())
                    {
                        clipboard.opening.reset();
                    }
                }

                return clipboard;
            }

            clipboard_state* clipboard_opened_by_caller(const syscall_context& c)
            {
                auto& clipboard = live_clipboard(c);
                if (clipboard.is_open_by(c.thread().id))
                {
                    return &clipboard;
                }

                set_guest_last_error(c, error_clipboard_not_open);
                return nullptr;
            }

            hwnd existing_window(const syscall_context& c, const hwnd window)
            {
                return c.proc.windows.get(window) ? window : 0;
            }

            const window* messageable_window(const syscall_context& c, const hwnd handle)
            {
                const auto* win = c.proc.windows.get(handle);
                return win && win->thread_id == c.thread().id ? win : nullptr;
            }

            void post_clipboard_update(const syscall_context& c, const uint64_t format_count)
            {
                for (const auto listener : c.proc.clipboard.listeners)
                {
                    const auto* win = c.proc.windows.get(listener);
                    auto* thread = win ? c.proc.find_thread_by_id(win->thread_id) : nullptr;
                    if (thread && !thread->is_terminated())
                    {
                        msg update{};
                        update.window = listener;
                        update.message = WM_CLIPBOARDUPDATE;
                        update.wParam = format_count;
                        thread->post_message(c.win_emu, update);
                    }
                }
            }

            bool viewer_notification_in_progress(const syscall_context& c)
            {
                const auto* thread = c.proc.find_thread_by_id(c.proc.clipboard.notifying_thread);
                return thread && !thread->is_terminated();
            }

            bool notify_viewer(const syscall_context& c, const callback_id id, clipboard_message_state state)
            {
                auto& clipboard = c.proc.clipboard;
                const auto* viewer = messageable_window(c, clipboard.viewer);
                if (!viewer || viewer_notification_in_progress(c))
                {
                    return false;
                }

                clipboard.notifying_thread = c.thread().id;
                dispatch_clipboard_message(c, id, std::move(state), *viewer, WM_DRAWCLIPBOARD, existing_window(c, clipboard.owner));
                return true;
            }

            uint64_t finish_get_clipboard_data(const syscall_context& c, clipboard_state& clipboard, const uint32_t format,
                                               const emulator_object<GETCLIPBDATA> request)
            {
                const auto data = clipboard.data(format);
                if (!data)
                {
                    set_guest_last_error(c, error_not_found);
                    return 0;
                }

                request.try_write({.format = data->format, .global_handle = data->memory_handle, .locale = 0});
                return data->handle;
            }

            bool is_registered_format(const syscall_context& c, const uint32_t format)
            {
                return format <= std::numeric_limits<uint16_t>::max() && c.proc.atoms.contains(static_cast<uint16_t>(format));
            }
        }

        uint32_t handle_NtUserGetClipboardSequenceNumber(const syscall_context& c)
        {
            return c.proc.clipboard.sequence_number;
        }

        BOOL handle_NtUserOpenClipboard(const syscall_context& c, const hwnd window, const emulator_object<BOOL> emptied)
        {
            if (window != 0)
            {
                const auto* target = c.proc.windows.get(window);
                if (!target)
                {
                    set_guest_last_error(c, error_invalid_window_handle);
                    return FALSE;
                }

                if (!c.proc.find_thread_by_id(target->thread_id))
                {
                    set_guest_last_error(c, error_success);
                    return FALSE;
                }
            }

            if (!live_clipboard(c).open(window, c.thread().id))
            {
                set_guest_last_error(c, error_access_denied);
                return FALSE;
            }

            if (emptied)
            {
                emptied.try_write(FALSE);
            }

            return TRUE;
        }

        BOOL handle_NtUserCloseClipboard(const syscall_context& c)
        {
            auto* clipboard = clipboard_opened_by_caller(c);
            if (!clipboard)
            {
                return FALSE;
            }

            if (clipboard->close() && !notify_viewer(c, callback_id::NtUserCloseClipboard, {}))
            {
                post_clipboard_update(c, clipboard->entries.size());
            }

            return TRUE;
        }

        BOOL completion_NtUserCloseClipboard(const syscall_context& c)
        {
            c.proc.clipboard.notifying_thread = 0;
            post_clipboard_update(c, c.proc.clipboard.entries.size());
            return TRUE;
        }

        BOOL handle_NtUserEmptyClipboard(const syscall_context& c)
        {
            auto* clipboard = clipboard_opened_by_caller(c);
            if (!clipboard)
            {
                return FALSE;
            }

            if (const auto* owner = messageable_window(c, clipboard->owner))
            {
                dispatch_clipboard_message(c, callback_id::NtUserEmptyClipboard, {}, *owner, WM_DESTROYCLIPBOARD, 0);
                return FALSE;
            }

            clipboard->empty();
            return TRUE;
        }

        BOOL completion_NtUserEmptyClipboard(const syscall_context& c)
        {
            auto* clipboard = clipboard_opened_by_caller(c);
            if (!clipboard)
            {
                return FALSE;
            }

            clipboard->empty();
            return TRUE;
        }

        BOOL handle_NtUserAddClipboardFormatListener(const syscall_context& c, const hwnd window)
        {
            if (!c.proc.windows.get(window))
            {
                set_guest_last_error(c, error_invalid_window_handle);
                return FALSE;
            }

            if (!c.proc.clipboard.add_listener(window))
            {
                set_guest_last_error(c, error_invalid_parameter);
                return FALSE;
            }

            return TRUE;
        }

        BOOL handle_NtUserRemoveClipboardFormatListener(const syscall_context& c, const hwnd window)
        {
            if (!c.proc.clipboard.remove_listener(window))
            {
                set_guest_last_error(c, error_invalid_parameter);
                return FALSE;
            }

            return TRUE;
        }

        uint64_t handle_NtUserGetClipboardData(const syscall_context& c, const uint32_t format, const emulator_object<GETCLIPBDATA> request)
        {
            auto* clipboard = clipboard_opened_by_caller(c);
            if (!clipboard)
            {
                return 0;
            }

            const auto* entry = clipboard->find(format);
            if (entry && !entry->synthesized && entry->handle == 0)
            {
                if (const auto* owner = messageable_window(c, clipboard->owner))
                {
                    clipboard_message_state state{};
                    state.result = clipboard->modified;
                    clipboard->rendering = true;
                    dispatch_clipboard_message(c, callback_id::NtUserGetClipboardData, std::move(state), *owner, WM_RENDERFORMAT, format);
                    return 0;
                }
            }

            return finish_get_clipboard_data(c, *clipboard, format, request);
        }

        uint64_t completion_NtUserGetClipboardData(const syscall_context& c, const uint32_t format,
                                                   const emulator_object<GETCLIPBDATA> request)
        {
            c.proc.clipboard.modified = c.get_completion_state<clipboard_message_state>().result != 0;
            c.proc.clipboard.rendering = false;

            return finish_get_clipboard_data(c, c.proc.clipboard, format, request);
        }

        uint64_t handle_NtUserConvertMemHandle(const syscall_context& c, const emulator_pointer data, const uint32_t size)
        {
            if (!is_readable_range(c.win_emu, data, size))
            {
                set_guest_last_error(c, error_noaccess);
                return 0;
            }

            std::vector<std::byte> contents(size);
            c.win_emu.memory.read_memory(data, contents.data(), contents.size());
            return c.proc.clipboard.store_memory(std::move(contents));
        }

        NTSTATUS handle_NtUserCreateLocalMemHandle(const syscall_context& c, const uint64_t handle, const emulator_pointer buffer,
                                                   const uint32_t size, const emulator_object<uint32_t> needed)
        {
            const auto* contents = c.proc.clipboard.memory(handle);
            if (!contents)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (buffer != 0 && !is_writable_range(c.win_emu, buffer, size))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (needed && (!is_writable_range(c.win_emu, needed.value(), needed.size()) ||
                           !needed.try_write(static_cast<uint32_t>(contents->size()))))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (buffer == 0 || size < contents->size())
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            c.win_emu.memory.write_memory(buffer, contents->data(), contents->size());
            return STATUS_SUCCESS;
        }

        BOOL handle_NtUserSetClipboardData(const syscall_context& c, const uint32_t format, const uint64_t data,
                                           const emulator_object<SETCLIPBDATA> request)
        {
            auto& clipboard = live_clipboard(c);
            if (!clipboard.opening || format == 0)
            {
                set_guest_last_error(c, error_clipboard_not_open);
                return FALSE;
            }

            if (format >= clipboard_format::first_registered && !is_registered_format(c, format))
            {
                set_guest_last_error(c, error_invalid_handle);
                return FALSE;
            }

            const auto flags = request.try_read().value_or(SETCLIPBDATA{.global_handle = FALSE, .increment_sequence_number = TRUE});
            clipboard.set(format, data, flags.global_handle != FALSE, flags.increment_sequence_number != FALSE);
            return TRUE;
        }

        BOOL handle_NtUserIsClipboardFormatAvailable(const syscall_context& c, const uint32_t format)
        {
            return c.proc.clipboard.find(format) != nullptr;
        }

        int32_t handle_NtUserCountClipboardFormats(const syscall_context& c)
        {
            return static_cast<int32_t>(c.proc.clipboard.entries.size());
        }

        uint32_t handle_NtUserEnumClipboardFormats(const syscall_context& c, const uint32_t format)
        {
            const auto* clipboard = clipboard_opened_by_caller(c);
            return clipboard ? clipboard->next_format(format) : 0;
        }

        int32_t handle_NtUserGetPriorityClipboardFormat(const syscall_context& c, const emulator_pointer formats, const int32_t count)
        {
            if (count < 0)
            {
                set_guest_last_error(c, error_noaccess);
                return 0;
            }

            const auto& clipboard = c.proc.clipboard;
            if (clipboard.entries.empty())
            {
                return 0;
            }

            for (int32_t i = 0; i < count; ++i)
            {
                const auto format = emulator_object<uint32_t>{c.emu, formats}.try_read(static_cast<size_t>(i));
                if (!format)
                {
                    set_guest_last_error(c, error_noaccess);
                    return 0;
                }

                if (clipboard.find(*format))
                {
                    return static_cast<int32_t>(*format);
                }
            }

            return -1;
        }

        int32_t handle_NtUserGetClipboardFormatName(const syscall_context& c, const uint32_t format, const emulator_pointer buffer,
                                                    const int32_t max_count)
        {
            if (max_count < 0 || (buffer == 0 && max_count != 0))
            {
                set_guest_last_error(c, error_noaccess);
                return 0;
            }

            const auto atom = static_cast<uint16_t>(format);
            if (atom < clipboard_format::first_registered)
            {
                set_guest_last_error(c, error_invalid_parameter);
                return 0;
            }

            const auto name = c.proc.get_atom_name(atom);
            if (!name)
            {
                set_guest_last_error(c, error_invalid_handle);
                return 0;
            }

            if (max_count < 2)
            {
                if (buffer != 0)
                {
                    set_guest_last_error(c, error_insufficient_buffer);
                }

                return 0;
            }

            const auto copied = std::min<size_t>(name->size(), static_cast<size_t>(max_count) - 1);
            std::u16string text = name->substr(0, copied);
            if (!c.emu.try_write_memory(buffer, text.c_str(), (copied + 1) * sizeof(char16_t)))
            {
                set_guest_last_error(c, error_noaccess);
                return 0;
            }

            return static_cast<int32_t>(copied);
        }

        hwnd handle_NtUserGetClipboardOwner(const syscall_context& c)
        {
            return existing_window(c, c.proc.clipboard.owner);
        }

        hwnd handle_NtUserGetClipboardViewer(const syscall_context& c)
        {
            return existing_window(c, c.proc.clipboard.viewer);
        }

        hwnd handle_NtUserSetClipboardViewer(const syscall_context& c, const hwnd window)
        {
            if (!c.proc.windows.get(window))
            {
                return 0;
            }

            auto& clipboard = c.proc.clipboard;
            clipboard_message_state state{};
            state.result = existing_window(c, clipboard.viewer);
            clipboard.viewer = window;

            const auto previous = state.result;
            return notify_viewer(c, callback_id::NtUserSetClipboardViewer, std::move(state)) ? 0 : previous;
        }

        hwnd completion_NtUserSetClipboardViewer(const syscall_context& c, const hwnd /*window*/)
        {
            c.proc.clipboard.notifying_thread = 0;
            return c.get_completion_state<clipboard_message_state>().result;
        }

        BOOL handle_NtUserChangeClipboardChain(const syscall_context& c, const hwnd window, const hwnd next)
        {
            auto& clipboard = c.proc.clipboard;
            if (window == 0 || clipboard.viewer != window)
            {
                return FALSE;
            }

            clipboard.viewer = next;
            return TRUE;
        }

        hwnd handle_NtUserGetOpenClipboardWindow(const syscall_context& c)
        {
            const auto& clipboard = live_clipboard(c);
            return clipboard.opening ? existing_window(c, clipboard.opening->window) : 0;
        }

        BOOL handle_NtUserGetUpdatedClipboardFormats(const syscall_context& c, const emulator_pointer formats, const uint32_t count,
                                                     const emulator_object<uint32_t> returned)
        {
            if (!returned)
            {
                set_guest_last_error(c, error_noaccess);
                return FALSE;
            }

            const auto& entries = c.proc.clipboard.entries;
            if (!returned.try_write(static_cast<uint32_t>(entries.size())))
            {
                set_guest_last_error(c, error_noaccess);
                return FALSE;
            }

            if (entries.size() > count)
            {
                set_guest_last_error(c, error_insufficient_buffer);
                return FALSE;
            }

            const emulator_object<uint32_t> output{c.emu, formats};
            for (size_t i = 0; i < entries.size(); ++i)
            {
                if (!output.try_write(entries[i].format, i))
                {
                    set_guest_last_error(c, error_noaccess);
                    return FALSE;
                }
            }

            return TRUE;
        }
    }
}

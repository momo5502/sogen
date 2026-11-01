#pragma once

#include "handles.hpp"
#include "memory_manager.hpp"
#include "shared_sync_state.hpp"
#include "filesystem/guest_file.hpp"

#include <algorithm>
#include <string_view>
#include <serialization_helper.hpp>
#include <utils/time.hpp>
#include <platform/synchronisation.hpp>
#include <platform/win_pefile.hpp>
#include <platform/window.hpp>

namespace sogen
{

    struct job_object : ref_counted_object
    {
        // The x64 JOBOBJECT_EXTENDED_LIMIT_INFORMATION, whose first part is the JOBOBJECT_BASIC_LIMIT_INFORMATION.
        static constexpr size_t extended_limit_information_size = 144;
        static constexpr size_t basic_limit_information_size = 64;
        static constexpr size_t limit_flags_offset = 16;

        struct member
        {
            uint64_t process{};
            uint32_t process_id{};
            bool exit_reported{};
        };

        std::u16string name{};
        std::array<uint8_t, extended_limit_information_size> extended_limits{};
        uint32_t ui_restrictions{};
        // Managed child processes assigned to the job.
        std::vector<member> processes{};
        // The completion port job notifications are posted to, once one is associated.
        handle completion_port{};
        uint64_t completion_key{};
        bool active_process_zero_reported{};

        uint32_t limit_flags() const
        {
            uint32_t flags{};
            memcpy(&flags, this->extended_limits.data() + limit_flags_offset, sizeof(flags));
            return flags;
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->extended_limits);
            buffer.write(this->ui_restrictions);
            buffer.write_vector(this->processes);
            buffer.write(this->completion_port);
            buffer.write(this->completion_key);
            buffer.write(this->active_process_zero_reported);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->extended_limits);
            buffer.read(this->ui_restrictions);
            buffer.read_vector(this->processes);
            buffer.read(this->completion_port);
            buffer.read(this->completion_key);
            buffer.read(this->active_process_zero_reported);
        }
    };

    // Suspensions made through a thread state-change object are undone when the object is closed.
    struct thread_state_change : ref_counted_object
    {
        uint32_t thread_id{};
        uint32_t suspend_count{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->thread_id);
            buffer.write(this->suspend_count);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->thread_id);
            buffer.read(this->suspend_count);
        }
    };

    struct timer : ref_counted_object
    {
        std::u16string name{};
        bool extended{};
        bool synchronization{};
        bool signaled{};
        std::optional<std::chrono::steady_clock::time_point> due_time{};
        int64_t period_nanoseconds{};

        bool is_signaled(utils::clock& clock)
        {
            return this->due_time ? this->is_signaled(clock.steady_now()) : this->signaled;
        }

        bool is_signaled(const std::chrono::steady_clock::time_point now)
        {
            if (this->due_time && *this->due_time <= now)
            {
                this->signaled = true;
                if (this->period_nanoseconds > 0)
                {
                    const auto period = std::chrono::nanoseconds(this->period_nanoseconds);
                    const auto elapsed = now - *this->due_time;
                    const auto remaining = period - elapsed % period;
                    *this->due_time = remaining > std::chrono::steady_clock::time_point::max() - now
                                          ? std::chrono::steady_clock::time_point::max()
                                          : now + remaining;
                }
                else
                {
                    this->due_time.reset();
                }
            }
            return this->signaled;
        }

        void consume_signal()
        {
            if (this->synchronization)
            {
                this->signaled = false;
            }
        }

        // Puts back what consume_signal took.
        void restore_signal()
        {
            if (this->synchronization)
            {
                this->signaled = true;
            }
        }

        void cancel()
        {
            this->due_time.reset();
            this->period_nanoseconds = 0;
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->extended);
            buffer.write(this->synchronization);
            buffer.write(this->signaled);
            buffer.write_optional(this->due_time);
            buffer.write(this->period_nanoseconds);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->extended);
            buffer.read(this->synchronization);
            buffer.read(this->signaled);
            buffer.read_optional(this->due_time);
            buffer.read(this->period_nanoseconds);
        }
    };

    struct event : ref_counted_object
    {
        EVENT_TYPE type{};
        std::u16string name{};
        // Set once a handle to the event is passed to another process.
        std::optional<shared_sync_state> shared{};

        bool is_signaled() const
        {
            if (this->shared)
            {
                return this->shared->access([](const shared_sync_state::fields& state) { return state.signaled != 0; });
            }
            return this->signaled_;
        }

        bool set_signaled(const bool signaled)
        {
            if (this->shared)
            {
                return this->shared->access(
                    [&](shared_sync_state::fields& state) { return std::exchange(state.signaled, signaled ? 1u : 0u) != 0; });
            }
            return std::exchange(this->signaled_, signaled);
        }

        // Satisfies a wait: reports whether the event is signaled and resets it if it is a synchronization event.
        bool try_acquire()
        {
            const auto auto_reset = this->type == SynchronizationEvent;
            if (this->shared)
            {
                return this->shared->access([&](shared_sync_state::fields& state) {
                    const auto signaled = state.signaled != 0;
                    if (signaled && auto_reset)
                    {
                        state.signaled = 0;
                    }
                    return signaled;
                });
            }

            const auto signaled = this->signaled_;
            if (signaled && auto_reset)
            {
                this->signaled_ = false;
            }
            return signaled;
        }

        const std::string& share()
        {
            if (!this->shared)
            {
                this->shared = shared_sync_state::create();
                this->shared->access([&](shared_sync_state::fields& state) { state.signaled = this->signaled_ ? 1 : 0; });
            }
            return this->shared->name();
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->is_signaled());
            buffer.write(this->type);
            buffer.write(this->name);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->signaled_);
            buffer.read(this->type);
            buffer.read(this->name);
            this->shared.reset();
        }

      private:
        bool signaled_{};
    };

    // A debugger's view of the child processes it debugs; their events reach it through the process manager.
    struct debug_object : ref_counted_object
    {
        struct debuggee
        {
            uint64_t process{};
            uint32_t process_id{};
            bool exit_reported{};
            std::vector<uint32_t> thread_ids{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->process);
                buffer.write(this->process_id);
                buffer.write(this->exit_reported);
                buffer.write_vector(this->thread_ids);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->process);
                buffer.read(this->process_id);
                buffer.read(this->exit_reported);
                buffer.read_vector(this->thread_ids);
            }
        };

        std::vector<debuggee> debuggees{};
        bool kill_on_close{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write_vector(this->debuggees);
            buffer.write(this->kill_on_close);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read_vector(this->debuggees);
            buffer.read(this->kill_on_close);
        }
    };

    struct deferred_window_positions : ref_counted_object
    {
        std::vector<EMU_WINDOWPOS> positions{};
        hwnd parent_handle{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write_vector(this->positions);
            buffer.write(this->parent_handle);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read_vector(this->positions);
            buffer.read(this->parent_handle);
        }
    };

    struct accelerator_table_entry
    {
        uint8_t flags{};
        uint16_t key{};
        uint16_t command{};
    };

    static_assert(sizeof(accelerator_table_entry) == 6);

    template <typename GuestType>
    struct user_object : ref_counted_object
    {
        using guest_type = GuestType;
        emulator_object<GuestType> guest;

        user_object(memory_interface& memory)
            : guest(memory)
        {
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->guest);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->guest);
        }
    };

    struct accelerator_table : user_object<USER_ACCELERATOR_TABLE>
    {
        std::vector<accelerator_table_entry> entries{};

        accelerator_table(memory_interface& memory)
            : user_object(memory)
        {
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            user_object::serialize_object(buffer);
            buffer.write_vector(this->entries);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            user_object::deserialize_object(buffer);
            buffer.read_vector(this->entries);
        }
    };

    // WC_DIALOG = MAKEINTATOM(0x8002); user32 creates dialogs and message boxes with this fixed
    // system atom, which the kernel reports as the class name "#32770". The atom value is constant
    // across Windows builds (unlike the builtin control-class atoms, which vary per build and are
    // resolved through SERVERINFO.atomSysClass), so matching this canonical name is portable.
    inline constexpr std::u16string_view builtin_dialog_class_name = u"#32770";

    inline std::u16string_view normalize_builtin_window_class_name(const std::u16string_view class_name)
    {
        if (class_name == u"#1" || class_name == u"BUTTON" || class_name == u"Button")
        {
            return u"Button";
        }
        if (class_name == u"#2" || class_name == u"EDIT" || class_name == u"Edit")
        {
            return u"Edit";
        }
        if (class_name == u"#3" || class_name == u"STATIC" || class_name == u"Static")
        {
            return u"Static";
        }
        if (class_name == u"#4" || class_name == u"LISTBOX" || class_name == u"ListBox")
        {
            return u"ListBox";
        }
        if (class_name == u"#5" || class_name == u"SCROLLBAR" || class_name == u"ScrollBar")
        {
            return u"ScrollBar";
        }
        if (class_name == u"#6" || class_name == u"COMBOBOX" || class_name == u"ComboBox")
        {
            return u"ComboBox";
        }
        return class_name;
    }

    struct window : user_object<USER_WINDOW>
    {
        uint32_t thread_id{};
        hwnd handle{};
        hwnd parent_handle{};
        hwnd owner_handle{};
        std::u16string name{};
        std::u16string class_name{};
        int32_t width{};
        int32_t height{};
        int32_t x{};
        int32_t y{};
        uint32_t ex_style{};
        uint32_t style{};
        RECT update_rect{};
        bool update_pending{};
        bool internal_paint_pending{};
        bool erase_pending{};
        std::map<std::u16string, uint64_t> props{};
        emulator_pointer wnd_proc{};
        hmenu system_menu_handle{};
        bool host_surface_window{};
        bool message_only{};
        bool unicode_proc{};

        window(memory_interface& memory)
            : user_object(memory)
        {
        }

        bool is_dialog() const
        {
            return this->class_name == builtin_dialog_class_name;
        }

        RECT nonclient_insets() const
        {
            int32_t border = 0;

            const bool caption = (this->style & WS_CAPTION) == WS_CAPTION;
            if ((this->style & WS_THICKFRAME) != 0)
            {
                border = caption ? 3 : 2;
            }
            else if (caption)
            {
                border = 1;
            }
            else if ((this->style & WS_DLGFRAME) != 0)
            {
                border = 3;
            }
            else if ((this->style & WS_BORDER) != 0)
            {
                border = 1;
            }

            if ((this->ex_style & WS_EX_CLIENTEDGE) != 0)
            {
                border += 2;
            }

            return {.left = border, .top = border, .right = border, .bottom = border};
        }

        int32_t client_x_offset() const
        {
            const auto outer_width = std::max(0, this->width);
            return std::min<int>(outer_width, this->nonclient_insets().left);
        }

        int32_t client_y_offset() const
        {
            const auto outer_height = std::max(0, this->height);
            return std::min<int>(outer_height, this->nonclient_insets().top);
        }

        int32_t client_x() const
        {
            return this->x + this->client_x_offset();
        }

        int32_t client_y() const
        {
            return this->y + this->client_y_offset();
        }

        int32_t client_width() const
        {
            const auto outer_width = std::max(0, this->width);
            const auto insets = this->nonclient_insets();
            const auto client_left = std::min<int>(outer_width, insets.left);
            const auto client_right = std::max<int>(client_left, outer_width - insets.right);
            return client_right - client_left;
        }

        int32_t client_height() const
        {
            const auto outer_height = std::max(0, this->height);
            const auto insets = this->nonclient_insets();
            const auto client_top = std::min<int>(outer_height, insets.top);
            const auto client_bottom = std::max<int>(client_top, outer_height - insets.bottom);
            return client_bottom - client_top;
        }

        void sync_guest_rects()
        {
            const RECT window_rect{.left = this->x, .top = this->y, .right = this->x + this->width, .bottom = this->y + this->height};
            const RECT client_rect{
                .left = this->client_x(),
                .top = this->client_y(),
                .right = this->client_x() + this->client_width(),
                .bottom = this->client_y() + this->client_height(),
            };

            this->guest.access([&](USER_WINDOW& guest_win) {
                guest_win.rcWindow = window_rect;
                guest_win.rcClient = client_rect;
            });
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            user_object::serialize_object(buffer);
            buffer.write(this->thread_id);
            buffer.write(this->handle);
            buffer.write(this->parent_handle);
            buffer.write(this->owner_handle);
            buffer.write(this->name);
            buffer.write(this->class_name);
            buffer.write(this->width);
            buffer.write(this->height);
            buffer.write(this->x);
            buffer.write(this->y);
            buffer.write(this->ex_style);
            buffer.write(this->style);
            buffer.write(this->update_rect);
            buffer.write(this->update_pending);
            buffer.write(this->internal_paint_pending);
            buffer.write(this->erase_pending);
            buffer.write_map(this->props);
            buffer.write(this->wnd_proc);
            buffer.write(this->system_menu_handle);
            buffer.write(this->host_surface_window);
            buffer.write(this->message_only);
            buffer.write(this->unicode_proc);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            user_object::deserialize_object(buffer);
            buffer.read(this->thread_id);
            buffer.read(this->handle);
            buffer.read(this->parent_handle);
            buffer.read(this->owner_handle);
            buffer.read(this->name);
            buffer.read(this->class_name);
            buffer.read(this->width);
            buffer.read(this->height);
            buffer.read(this->x);
            buffer.read(this->y);
            buffer.read(this->ex_style);
            buffer.read(this->style);
            buffer.read(this->update_rect);
            buffer.read(this->update_pending);
            buffer.read(this->internal_paint_pending);
            buffer.read(this->erase_pending);
            buffer.read_map(this->props);
            buffer.read(this->wnd_proc);
            buffer.read(this->system_menu_handle);
            buffer.read(this->host_surface_window);
            buffer.read(this->message_only);
            buffer.read(this->unicode_proc);
        }
    };

    struct menu_item
    {
        uint32_t id{};
        hmenu submenu{};
        emulator_pointer submenu_ptr{};
        uint32_t type{};
        uint32_t state{};
        uint64_t data{};
        hbitmap hbmp_checked{};
        hbitmap hbmp_unchecked{};
        hbitmap hbmp_item{};
        std::u16string text{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->id);
            buffer.write(this->submenu);
            buffer.write(this->submenu_ptr);
            buffer.write(this->type);
            buffer.write(this->state);
            buffer.write(this->data);
            buffer.write(this->hbmp_checked);
            buffer.write(this->hbmp_unchecked);
            buffer.write(this->hbmp_item);
            buffer.write(this->text);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->id);
            buffer.read(this->submenu);
            buffer.read(this->submenu_ptr);
            buffer.read(this->type);
            buffer.read(this->state);
            buffer.read(this->data);
            buffer.read(this->hbmp_checked);
            buffer.read(this->hbmp_unchecked);
            buffer.read(this->hbmp_item);
            buffer.read(this->text);
        }
    };

    struct menu : user_object<USER_MENU>
    {
        hmenu handle{};
        uint32_t item_count{};
        bool popup{};
        std::vector<menu_item> items{};
        emulator_pointer item_storage{};
        emulator_pointer text_storage{};

        menu(memory_interface& memory)
            : user_object(memory)
        {
        }

        void init_guest() const
        {
            this->guest.access([&](USER_MENU& m) {
                m.hMenu = this->handle;
                m.ptrBase = this->guest.value();
                m.rgItems = this->item_storage;
                m.flags = 0;
                m.cItems = this->item_count;
            });
        }

        void sync_guest_items(memory_manager& memory)
        {
            this->release_backing(memory);
            this->item_count = static_cast<uint32_t>(this->items.size());

            const auto text_bytes = this->text_storage_size();
            if (text_bytes != 0)
            {
                this->text_storage = memory.allocate_memory(static_cast<size_t>(page_align_up(text_bytes)), memory_permission::read);
            }

            if (!this->items.empty())
            {
                const auto item_bytes = sizeof(USER_MENU_ITEM) * this->items.size();
                this->item_storage = memory.allocate_memory(static_cast<size_t>(page_align_up(item_bytes)), memory_permission::read);
            }

            auto next_text = this->text_storage;
            for (size_t i = 0; i < this->items.size(); ++i)
            {
                const auto& item = this->items.at(i);
                const auto text_ptr = !item.text.empty() ? next_text : 0;
                const auto guest_item = make_guest_item(item, text_ptr);
                write_guest_item_text(memory, item, text_ptr);

                if (text_ptr != 0)
                {
                    next_text += (item.text.size() + 1) * sizeof(char16_t);
                }

                memory.write_memory(this->item_storage + i * sizeof(USER_MENU_ITEM), &guest_item, sizeof(guest_item));
            }

            this->guest.access([&](USER_MENU& m) {
                m.rgItems = this->item_storage;
                m.cItems = this->item_count;
            });
        }

        void sync_guest_item(memory_manager& memory, const size_t index)
        {
            // TODO: This method is very naive and smartly reserving memory could avoid lots of reallocations.
            if (this->item_storage == 0 || static_cast<size_t>(this->item_count) != this->items.size() || index >= this->items.size())
            {
                this->sync_guest_items(memory);
                return;
            }

            const auto& item = this->items.at(index);
            const auto text_ptr = this->get_guest_text_ptr(index);
            const auto guest_item = make_guest_item(item, text_ptr);
            write_guest_item_text(memory, item, text_ptr);

            memory.write_memory(this->item_storage + index * sizeof(USER_MENU_ITEM), &guest_item, sizeof(guest_item));
        }

        void release_guest_backing(memory_manager& memory)
        {
            this->release_backing(memory);
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            user_object::serialize_object(buffer);
            buffer.write(this->handle);
            buffer.write(this->item_count);
            buffer.write(this->popup);
            buffer.write_vector(this->items);
            buffer.write(this->item_storage);
            buffer.write(this->text_storage);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            user_object::deserialize_object(buffer);
            buffer.read(this->handle);
            buffer.read(this->item_count);
            buffer.read(this->popup);
            buffer.read_vector(this->items);
            buffer.read(this->item_storage);
            buffer.read(this->text_storage);
        }

      private:
        static USER_MENU_ITEM make_guest_item(const menu_item& item, const emulator_pointer text_ptr)
        {
            return USER_MENU_ITEM{
                .type = item.type,
                .state = item.state,
                .id = item.id,
                .submenu = item.submenu_ptr,
                .hbmpChecked = item.hbmp_checked,
                .hbmpUnchecked = item.hbmp_unchecked,
                .text = text_ptr,
                .cch = static_cast<uint32_t>(item.text.size()),
                .data = item.data,
                .hbmpItem = item.hbmp_item,
            };
        }

        static void write_guest_item_text(memory_manager& memory, const menu_item& item, const emulator_pointer text_ptr)
        {
            if (!item.text.empty() && text_ptr != 0)
            {
                const auto bytes = item.text.size() * sizeof(char16_t);
                memory.write_memory(text_ptr, item.text.data(), bytes);

                constexpr char16_t terminator = 0;
                memory.write_memory(text_ptr + bytes, &terminator, sizeof(terminator));
            }
        }

        void release_backing(memory_manager& memory)
        {
            if (this->item_storage != 0)
            {
                memory.release_memory(this->item_storage, 0);
                this->item_storage = 0;
            }

            if (this->text_storage != 0)
            {
                memory.release_memory(this->text_storage, 0);
                this->text_storage = 0;
            }
        }

        emulator_pointer get_guest_text_ptr(const size_t index) const
        {
            if (this->text_storage == 0 || index >= this->items.size())
            {
                return 0;
            }

            auto text_ptr = this->text_storage;
            for (size_t i = 0; i < index; ++i)
            {
                const auto& item = this->items.at(i);
                if (!item.text.empty())
                {
                    text_ptr += (item.text.size() + 1) * sizeof(char16_t);
                }
            }

            return this->items.at(index).text.empty() ? 0 : text_ptr;
        }

        size_t text_storage_size() const
        {
            size_t bytes = 0;
            for (const auto& item : this->items)
            {
                if (!item.text.empty())
                {
                    bytes += (item.text.size() + 1) * sizeof(char16_t);
                }
            }

            return bytes;
        }
    };

    struct desktop : ref_counted_object
    {
        std::u16string name{};
        uint64_t mapped_object{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->mapped_object);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->mapped_object);
        }
    };

    struct token_privilege
    {
        uint32_t luid{};
        uint32_t attributes{};
    };

    // The privileges of an interactive user's token on Windows 10 and 11, as `whoami /priv` lists them, or those of the
    // built-in Administrator's elevated token.
    inline std::vector<token_privilege> default_token_privileges(const bool elevated)
    {
        constexpr std::array<uint32_t, 5> standard_privileges{19, 23, 25, 33, 34};
        constexpr std::array<uint32_t, 24> elevated_privileges{5,  8,  9,  10, 11, 12, 13, 14, 15, 17, 18, 19,
                                                               20, 22, 23, 24, 25, 28, 29, 30, 33, 34, 35, 36};
        constexpr std::array<uint32_t, 3> enabled_by_default{23, 29, 30};
        constexpr uint32_t enabled = 0x3;

        const auto luids = elevated ? std::span<const uint32_t>{elevated_privileges} : std::span<const uint32_t>{standard_privileges};
        std::vector<token_privilege> privileges{};
        privileges.reserve(luids.size());
        for (const auto luid : luids)
        {
            privileges.push_back(
                {.luid = luid, .attributes = std::ranges::find(enabled_by_default, luid) != enabled_by_default.end() ? enabled : 0});
        }
        return privileges;
    }

    // A handle to one of the tokens the emulator models, which are identified by their pseudo-handle.
    struct token_object : ref_counted_object
    {
        handle identity{};
        // Unset for handles to the process token, whose privileges belong to the process. A token created from another
        // one has its own.
        std::optional<std::vector<token_privilege>> privileges{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->identity);
            buffer.write(this->privileges.has_value());
            if (this->privileges)
            {
                buffer.write_vector(*this->privileges);
            }
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->identity);
            this->privileges.reset();
            if (buffer.read<bool>())
            {
                buffer.read_vector(this->privileges.emplace());
            }
        }
    };

    struct mutant : ref_counted_object
    {
      private:
        using state = shared_sync_state::fields;
        state local_{};

        static void abandon(state& current)
        {
            if (current.locked_count == 0)
            {
                return;
            }

            // Any of these stores can be the last one a dying process makes: abandoned first, so a free mutant is
            // never left unreported, then the count, so a held count always still has a host owner recovery can
            // check and an owner a thread can match.
            current.abandoned = 1;
            current.locked_count = 0;
            current.owner = 0;
            current.owner_host_process = 0;
        }

        template <typename Function>
        auto access(Function&& function) const
        {
            if (this->shared)
            {
                return this->shared->access(std::forward<Function>(function));
            }
            return function(this->local_);
        }

        template <typename Function>
        auto access(Function&& function)
        {
            if (this->shared)
            {
                return this->shared->access(std::forward<Function>(function));
            }
            return function(this->local_);
        }

      public:
        std::u16string name{};
        // Set once a handle to the mutant is passed to another process.
        std::optional<shared_sync_state> shared{};

        // Thread IDs are only unique within a process, so ownership names both.
        static uint64_t make_owner(const uint32_t process_id, const uint32_t thread_id)
        {
            return (static_cast<uint64_t>(process_id) << 32) | thread_id;
        }

        bool is_locked() const
        {
            return this->access([](const state& current) { return current.locked_count != 0; });
        }

        bool is_abandoned() const
        {
            return this->access([](const state& current) { return current.abandoned != 0; });
        }

        uint64_t owner() const
        {
            return this->access([](const state& current) { return current.owner; });
        }

        struct lock_info
        {
            uint32_t locked_count{};
            uint64_t owner{};
            bool abandoned{};
        };

        // What NtQueryMutant reports, in one look, so a shared mutant whose owner dies cannot be described by a mix
        // of two states.
        lock_info lock_state() const
        {
            return this->access([](const state& current) {
                return lock_info{.locked_count = current.locked_count, .owner = current.owner, .abandoned = current.abandoned != 0};
            });
        }

        bool is_signaled(const uint64_t owner) const
        {
            return this->access(
                [&](const state& current) { return current.abandoned != 0 || current.locked_count == 0 || current.owner == owner; });
        }

        // Returns whether the mutant was abandoned, or nothing if another thread owns it.
        std::optional<bool> try_lock(const uint64_t owner)
        {
            return this->access([&](state& current) -> std::optional<bool> {
                if (current.locked_count == 0)
                {
                    // The owner and its host process are in place before the count says the mutant is held, so a
                    // death in between leaves it free rather than held by nobody recovery can identify.
                    current.owner = owner;
                    current.owner_host_process = utils::current_host_process_id();
                    current.locked_count = 1;
                    return std::exchange(current.abandoned, 0) != 0;
                }

                if (current.owner != owner)
                {
                    return std::nullopt;
                }

                ++current.locked_count;
                return false;
            });
        }

        std::pair<uint32_t, bool> release(const uint64_t owner)
        {
            return this->access([&](state& current) -> std::pair<uint32_t, bool> {
                const auto old_count = current.locked_count;
                if (current.locked_count == 0 || current.owner != owner)
                {
                    return {old_count, false};
                }

                --current.locked_count;
                if (current.locked_count == 0)
                {
                    current.owner = 0;
                    current.owner_host_process = 0;
                }

                return {old_count, true};
            });
        }

        void abandon()
        {
            this->access([](state& current) { abandon(current); });
        }

        // Undoes a try_lock, including the abandoned flag it cleared, for a wait-all that has to put back what it
        // took after a later object turned out to be gone.
        void unlock_and_restore(const uint64_t owner, const bool was_abandoned)
        {
            this->access([&](state& current) {
                if (current.locked_count == 0 || current.owner != owner)
                {
                    return;
                }

                if (was_abandoned)
                {
                    current.abandoned = 1;
                }

                --current.locked_count;
                if (current.locked_count == 0)
                {
                    current.owner = 0;
                    current.owner_host_process = 0;
                }
            });
        }

        void abandon_if_owned_by(const uint64_t owner)
        {
            this->access([&](state& current) {
                if (current.owner == owner)
                {
                    abandon(current);
                }
            });
        }

        // A restored lock belongs to this emulator again only when its owner is one of this process's threads.
        // Anything else, such as a mutant a child held when the parent saved its state, has no thread left that could
        // release it, so it comes back abandoned, as Windows reports an owner that is gone. Called once the process id
        // is known, which is read after the mutants.
        void adopt_restored_lock(const uint32_t process_id)
        {
            this->access([&](state& current) {
                if (current.locked_count == 0)
                {
                    current.owner_host_process = 0;
                    return;
                }

                if ((current.owner >> 32) == process_id)
                {
                    current.owner_host_process = utils::current_host_process_id();
                    return;
                }

                abandon(current);
            });
        }

        void abandon_if_owned_by_process(const uint32_t process_id)
        {
            this->access([&](state& current) {
                if ((current.owner >> 32) == process_id)
                {
                    abandon(current);
                }
            });
        }

        const std::string& share()
        {
            if (!this->shared)
            {
                const auto local = this->local_;
                this->shared = shared_sync_state::create();
                this->shared->access([&](state& current) { current = local; });
            }
            return this->shared->name();
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            const auto current = this->access([](const state& values) { return values; });
            buffer.write(current.locked_count);
            buffer.write(current.owner);
            buffer.write(current.abandoned);
            buffer.write(this->name);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->local_.locked_count);
            buffer.read(this->local_.owner);
            buffer.read(this->local_.abandoned);
            buffer.read(this->name);
            this->shared.reset();
            this->local_.owner_host_process = 0;
        }
    };

    struct file_entry
    {
        std::filesystem::path file_path{};
        std::u16string short_name{};
        uint64_t file_size{};
        bool is_directory{};
        LARGE_INTEGER creation_time{};
        LARGE_INTEGER last_access_time{};
        LARGE_INTEGER last_write_time{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->file_path);
            buffer.write(this->short_name);
            buffer.write(this->file_size);
            buffer.write(this->is_directory);
            buffer.write(this->creation_time);
            buffer.write(this->last_access_time);
            buffer.write(this->last_write_time);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->file_path);
            buffer.read(this->short_name);
            buffer.read(this->file_size);
            buffer.read(this->is_directory);
            buffer.read(this->creation_time);
            buffer.read(this->last_access_time);
            buffer.read(this->last_write_time);
        }
    };

    struct file_enumeration_state
    {
        size_t current_index{0};
        std::vector<file_entry> files{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->current_index);
            buffer.write_vector(this->files);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->current_index);
            buffer.read_vector(this->files);
        }
    };

    struct directory_change_watch
    {
        uint32_t thread_id{};
        uint64_t event{};
        uint64_t io_status_block{};
    };

    struct file : ref_counted_object
    {
        guest_file handle{};
        std::u16string name{};
        std::optional<file_enumeration_state> enumeration_state{};
        ACCESS_MASK access_mask{};
        uint32_t file_mode{}; // FileModeInformation
        uint8_t drive_number{'c' - 'a' + 1};
        std::vector<directory_change_watch> change_watches{};

        bool is_file() const
        {
            return static_cast<bool>(this->handle);
        }

        bool is_directory() const
        {
            return !this->is_file();
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write_optional(this->enumeration_state);
            buffer.write(this->access_mask);
            buffer.write(this->file_mode);
            buffer.write(this->drive_number);
            buffer.write(this->handle);
            buffer.write_vector(this->change_watches);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read_optional(this->enumeration_state);
            buffer.read(this->access_mask);
            buffer.read(this->file_mode);
            buffer.read(this->drive_number);
            buffer.read(this->handle);
            buffer.read_vector(this->change_watches);
        }
    };

    struct section : ref_counted_object
    {
        static constexpr ACCESS_MASK all_access = 0xF001F;

        static ACCESS_MASK map_generic_access(ACCESS_MASK access)
        {
            constexpr ACCESS_MASK generic_read = 0x80000000;
            constexpr ACCESS_MASK generic_write = 0x40000000;
            constexpr ACCESS_MASK generic_execute = 0x20000000;
            constexpr ACCESS_MASK generic_all = 0x10000000;
            constexpr ACCESS_MASK maximum_allowed = 0x02000000;
            constexpr ACCESS_MASK section_query = 0x1;
            constexpr ACCESS_MASK section_map_write = 0x2;
            constexpr ACCESS_MASK section_map_read = 0x4;
            constexpr ACCESS_MASK section_map_execute = 0x8;
            constexpr ACCESS_MASK read_control = 0x20000;
            constexpr ACCESS_MASK synchronize_and_standard = 0x000F0000;

            if (access & (generic_all | maximum_allowed))
            {
                access |= all_access;
            }
            if (access & generic_read)
            {
                access |= section_query | section_map_read | read_control;
            }
            if (access & generic_write)
            {
                access |= section_map_write | read_control;
            }
            if (access & generic_execute)
            {
                access |= section_map_execute | read_control;
            }

            return access & (all_access | synchronize_and_standard);
        }

        std::u16string name{};
        // Section handles that differ in access are separate objects sharing one backing, as a handle's access is fixed.
        ACCESS_MASK granted_access{all_access};
        std::u16string file_name{};
        uint64_t maximum_size{};
        uint32_t section_page_protection{};
        uint32_t allocation_attributes{};
        // Shared backing for a pagefile-backed section: allocated once (lazily, on first map) and reused by
        // every view, so all views of the section see the same memory and section offsets resolve correctly.
        // 0 until allocated. Freed once the last section handle is closed and no mapped view remains.
        uint64_t backing_address{};
        uint32_t mapped_view_count{};
        // Set instead of backing_address for a section whose views each get their own guest range (see section_views).
        uint64_t view_storage_id{};
        std::optional<winpe::pe_image_basic_info> cached_image_info{};

        bool is_image() const
        {
            return this->allocation_attributes & SEC_IMAGE;
        }

        void cache_image_info_from_filedata(const std::vector<std::byte>& file_data)
        {
            winpe::pe_image_basic_info info{};

            // Read the PE magic to determine if it's 32-bit or 64-bit
            bool parsed = false;
            if (file_data.size() >= sizeof(PEDosHeader_t))
            {
                const auto* dos_header = reinterpret_cast<const PEDosHeader_t*>(file_data.data());
                if (dos_header->e_magic == PEDosHeader_t::k_Magic &&
                    file_data.size() >= dos_header->e_lfanew + sizeof(uint32_t) + sizeof(PEFileHeader_t) + sizeof(uint16_t))
                {
                    const auto* magic_ptr = reinterpret_cast<const uint16_t*>(file_data.data() + dos_header->e_lfanew + sizeof(uint32_t) +
                                                                              sizeof(PEFileHeader_t));
                    const uint16_t magic = *magic_ptr;

                    // Parse based on the actual PE type
                    if (magic == PEOptionalHeader_t<std::uint32_t>::k_Magic)
                    {
                        parsed = winpe::parse_pe_headers<uint32_t>(file_data, info);
                    }
                    else if (magic == PEOptionalHeader_t<std::uint64_t>::k_Magic)
                    {
                        parsed = winpe::parse_pe_headers<uint64_t>(file_data, info);
                    }
                }
            }

            if (parsed)
            {
                this->cached_image_info = info;
            }
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->file_name);
            buffer.write(this->maximum_size);
            buffer.write(this->section_page_protection);
            buffer.write(this->allocation_attributes);
            buffer.write(this->granted_access);
            buffer.write(this->backing_address);
            buffer.write(this->mapped_view_count);
            buffer.write(this->view_storage_id);
            buffer.write_optional<winpe::pe_image_basic_info>(this->cached_image_info);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->file_name);
            buffer.read(this->maximum_size);
            buffer.read(this->section_page_protection);
            buffer.read(this->allocation_attributes);
            buffer.read(this->granted_access);
            buffer.read(this->backing_address);
            buffer.read(this->mapped_view_count);
            buffer.read(this->view_storage_id);
            buffer.read_optional(this->cached_image_info);
        }
    };

    struct semaphore : ref_counted_object
    {
        std::u16string name{};
        uint32_t current_count{};
        uint32_t max_count{};

        bool try_lock()
        {
            if (this->current_count > 0)
            {
                --this->current_count;
                return true;
            }

            return false;
        }

        std::pair<uint32_t, bool> release(const uint32_t release_count)
        {
            const auto old_count = this->current_count;

            if (this->current_count + release_count > this->max_count)
            {
                return {old_count, false};
            }

            this->current_count += release_count;

            return {old_count, true};
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->current_count);
            buffer.write(this->max_count);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->current_count);
            buffer.read(this->max_count);
        }
    };

    struct io_completion_message
    {
        uint64_t key_context{};
        uint64_t apc_context{};
        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> io_status_block{};
        handle wait_packet_handle{};

        handle worker_factory_handle{};
        bool worker_factory_release{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->key_context);
            buffer.write(this->apc_context);
            buffer.write(this->io_status_block);
            buffer.write(this->wait_packet_handle);
            buffer.write(this->worker_factory_handle);
            buffer.write(this->worker_factory_release);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->key_context);
            buffer.read(this->apc_context);
            buffer.read(this->io_status_block);
            buffer.read(this->wait_packet_handle);
            buffer.read(this->worker_factory_handle);
            buffer.read(this->worker_factory_release);
        }
    };

    struct io_completion : ref_counted_object
    {
        std::u16string name{};
        uint32_t number_of_concurrent_threads{};
        std::vector<io_completion_message> queue{};

        void enqueue(const io_completion_message& message)
        {
            this->queue.push_back(message);
        }

        bool dequeue(io_completion_message& out_message)
        {
            if (this->queue.empty())
            {
                return false;
            }

            out_message = this->queue.front();
            this->queue.erase(this->queue.begin());
            return true;
        }

        bool remove_by_wait_packet(const handle wait_packet_handle)
        {
            const auto entry = std::ranges::find_if(this->queue, [&](const io_completion_message& message) {
                return message.wait_packet_handle == wait_packet_handle; //
            });

            if (entry == this->queue.end())
            {
                return false;
            }

            this->queue.erase(entry);
            return true;
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->number_of_concurrent_threads);
            buffer.write_vector(this->queue);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->number_of_concurrent_threads);
            buffer.read_vector(this->queue);
        }
    };

    struct wait_completion_packet : ref_counted_object
    {
        std::u16string name{};
        handle io_completion_handle{};
        handle target_object_handle{};
        uint64_t key_context{};
        uint64_t apc_context{};
        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> io_status_block{};
        uint64_t io_status_information{};
        bool associated{};
        bool queued_completion{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->io_completion_handle);
            buffer.write(this->target_object_handle);
            buffer.write(this->key_context);
            buffer.write(this->apc_context);
            buffer.write(this->io_status_block);
            buffer.write(this->io_status_information);
            buffer.write(this->associated);
            buffer.write(this->queued_completion);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->io_completion_handle);
            buffer.read(this->target_object_handle);
            buffer.read(this->key_context);
            buffer.read(this->apc_context);
            buffer.read(this->io_status_block);
            buffer.read(this->io_status_information);
            buffer.read(this->associated);
            buffer.read(this->queued_completion);
        }
    };

    struct worker_factory : ref_counted_object
    {
        std::u16string name{};
        handle io_completion_handle{};
        handle worker_process_handle{};
        uint64_t start_routine{};
        uint64_t start_parameter{};
        uint32_t max_thread_count{};
        uint64_t stack_reserve{};
        uint64_t stack_commit{};
        bool shutdown{};

        int64_t timeout{};
        int64_t retry_timeout{};
        int64_t idle_timeout{};
        uint32_t binding_count{};
        uint32_t thread_minimum{};
        uint32_t thread_maximum{};
        uint32_t paused{};
        uint32_t thread_base_priority{};
        uint32_t timeout_waiters{};
        uint32_t flags{};
        uint32_t thread_soft_maximum{};

        uint32_t last_info_class{};
        uint32_t last_info_length{};
        uint64_t last_info_value{};
        uint32_t pending_release_count{};
        bool release_pending{};
        std::vector<handle> worker_threads{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->io_completion_handle);
            buffer.write(this->worker_process_handle);
            buffer.write(this->start_routine);
            buffer.write(this->start_parameter);
            buffer.write(this->max_thread_count);
            buffer.write(this->stack_reserve);
            buffer.write(this->stack_commit);
            buffer.write(this->shutdown);
            buffer.write(this->timeout);
            buffer.write(this->retry_timeout);
            buffer.write(this->idle_timeout);
            buffer.write(this->binding_count);
            buffer.write(this->thread_minimum);
            buffer.write(this->thread_maximum);
            buffer.write(this->paused);
            buffer.write(this->thread_base_priority);
            buffer.write(this->timeout_waiters);
            buffer.write(this->flags);
            buffer.write(this->thread_soft_maximum);
            buffer.write(this->last_info_class);
            buffer.write(this->last_info_length);
            buffer.write(this->last_info_value);
            buffer.write(this->pending_release_count);
            buffer.write(this->release_pending);
            buffer.write_vector(this->worker_threads);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->io_completion_handle);
            buffer.read(this->worker_process_handle);
            buffer.read(this->start_routine);
            buffer.read(this->start_parameter);
            buffer.read(this->max_thread_count);
            buffer.read(this->stack_reserve);
            buffer.read(this->stack_commit);
            buffer.read(this->shutdown);
            buffer.read(this->timeout);
            buffer.read(this->retry_timeout);
            buffer.read(this->idle_timeout);
            buffer.read(this->binding_count);
            buffer.read(this->thread_minimum);
            buffer.read(this->thread_maximum);
            buffer.read(this->paused);
            buffer.read(this->thread_base_priority);
            buffer.read(this->timeout_waiters);
            buffer.read(this->flags);
            buffer.read(this->thread_soft_maximum);
            buffer.read(this->last_info_class);
            buffer.read(this->last_info_length);
            buffer.read(this->last_info_value);
            buffer.read(this->pending_release_count);
            buffer.read(this->release_pending);
            buffer.read_vector(this->worker_threads);
        }
    };

    struct private_namespace : ref_counted_object
    {
        std::u16string boundary_name{};
        ACCESS_MASK access_mask{};
        bool deleted{false};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->boundary_name);
            buffer.write(this->access_mask);
            buffer.write(this->deleted);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->boundary_name);
            buffer.read(this->access_mask);
            buffer.read(this->deleted);
        }
    };

} // namespace sogen

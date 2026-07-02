#pragma once

#include "handles.hpp"
#include "memory_manager.hpp"

#include <algorithm>
#include <string_view>
#include <serialization_helper.hpp>
#include <utils/file_handle.hpp>
#include <platform/synchronisation.hpp>
#include <platform/win_pefile.hpp>
#include <platform/window.hpp>

namespace sogen
{

    struct timer : ref_counted_object
    {
        std::u16string name{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
        }
    };

    struct event : ref_counted_object
    {
        bool signaled{};
        EVENT_TYPE type{};
        std::u16string name{};

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->signaled);
            buffer.write(this->type);
            buffer.write(this->name);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->signaled);
            buffer.read(this->type);
            buffer.read(this->name);
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
        bool paint_message_posted{};
        bool erase_pending{};
        std::map<std::u16string, uint64_t> props{};
        emulator_pointer wnd_proc{};
        hmenu system_menu_handle{};
        bool host_surface_window{};
        bool unicode_proc{};

        window(memory_interface& memory)
            : user_object(memory)
        {
        }

        bool is_dialog() const
        {
            return this->class_name == builtin_dialog_class_name;
        }

        // The guest sizes its window via AdjustWindowRect, which inflates the client rect it wants to render
        // into by the non-client frame. user32 always adds a 1px border for framed windows (SM_CXBORDER/
        // SM_CYBORDER are hardcoded to 1) and our system metrics for the sizing frame and caption are zero, so
        // the only inset is that 1px border. Mirror it so the client size we report (GetClientRect, the Vulkan
        // surface extent, the presented surface) matches what the guest actually renders -- otherwise the
        // layer (DXVK) renders its whole frame onto a 1px-larger surface and the upscaled image softens.
        int32_t nonclient_border() const
        {
            return (this->style & (WS_BORDER | WS_DLGFRAME | WS_THICKFRAME)) != 0 ? 1 : 0;
        }

        int32_t client_width() const
        {
            return std::max(0, this->width - 2 * this->nonclient_border());
        }

        int32_t client_height() const
        {
            return std::max(0, this->height - 2 * this->nonclient_border());
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
            buffer.write(this->paint_message_posted);
            buffer.write(this->erase_pending);
            buffer.write_map(this->props);
            buffer.write(this->wnd_proc);
            buffer.write(this->system_menu_handle);
            buffer.write(this->host_surface_window);
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
            buffer.read(this->paint_message_posted);
            buffer.read(this->erase_pending);
            buffer.read_map(this->props);
            buffer.read(this->wnd_proc);
            buffer.read(this->system_menu_handle);
            buffer.read(this->host_surface_window);
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

    struct mutant : ref_counted_object
    {
        uint32_t locked_count{0};
        uint32_t owning_thread_id{};
        bool abandoned{false};
        std::u16string name{};

        bool is_signaled(const uint32_t thread_id) const
        {
            return this->abandoned || this->locked_count == 0 || this->owning_thread_id == thread_id;
        }

        std::optional<bool> try_lock(const uint32_t thread_id)
        {
            if (this->locked_count == 0)
            {
                ++this->locked_count;
                this->owning_thread_id = thread_id;
                const auto was_abandoned = this->abandoned;
                this->abandoned = false;
                return was_abandoned;
            }

            if (this->owning_thread_id != thread_id)
            {
                return std::nullopt;
            }

            ++this->locked_count;
            return false;
        }

        std::pair<uint32_t, bool> release(const uint32_t thread_id)
        {
            const auto old_count = this->locked_count;

            if (this->locked_count <= 0 || this->owning_thread_id != thread_id)
            {
                return {old_count, false};
            }

            --this->locked_count;
            if (this->locked_count == 0)
            {
                this->owning_thread_id = 0;
            }
            return {old_count, true};
        }

        void abandon()
        {
            if (this->locked_count == 0)
            {
                return;
            }

            this->locked_count = 0;
            this->owning_thread_id = 0;
            this->abandoned = true;
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->locked_count);
            buffer.write(this->owning_thread_id);
            buffer.write(this->abandoned);
            buffer.write(this->name);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->locked_count);
            buffer.read(this->owning_thread_id);
            buffer.read(this->abandoned);
            buffer.read(this->name);
        }
    };

    struct file_entry
    {
        std::filesystem::path file_path{};
        uint64_t file_size{};
        bool is_directory{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->file_path);
            buffer.write(this->file_size);
            buffer.write(this->is_directory);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->file_path);
            buffer.read(this->file_size);
            buffer.read(this->is_directory);
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

    struct file : ref_counted_object
    {
        utils::file_handle handle{};
        std::u16string name{};
        std::u16string open_mode{};
        std::filesystem::path host_path{};
        std::optional<file_enumeration_state> enumeration_state{};
        ACCESS_MASK access_mask{};
        uint32_t file_mode{}; // FileModeInformation
        uint8_t drive_number{'c' - 'a' + 1};

        bool is_file() const
        {
            return this->handle;
        }

        bool is_directory() const
        {
            return !this->is_file();
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->name);
            buffer.write(this->open_mode);
            buffer.write(this->host_path.u16string());
            buffer.write_optional(this->enumeration_state);
            buffer.write(this->access_mask);
            buffer.write(this->file_mode);
            buffer.write(this->drive_number);

            const auto has_handle = static_cast<bool>(this->handle);
            buffer.write(has_handle);

            if (has_handle)
            {
                buffer.write(this->handle);
            }
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->open_mode);
            this->host_path = buffer.read<std::u16string>();
            buffer.read_optional(this->enumeration_state);
            buffer.read(this->access_mask);
            buffer.read(this->file_mode);
            buffer.read(this->drive_number);

            const auto has_handle = buffer.read<bool>();

            this->handle = {};

            if (has_handle)
            {
#if defined(OS_WINDOWS)
                FILE* native_file = _wfopen(this->host_path.c_str(), reinterpret_cast<const wchar_t*>(this->open_mode.c_str()));
#else
                FILE* native_file = fopen(u16_to_u8(this->host_path.u16string()).c_str(), u16_to_u8(this->open_mode).c_str());
#endif

                if (native_file)
                {
                    this->handle = native_file;
                    buffer.read(this->handle);
                }
                else
                {
                    throw std::runtime_error("Failed to reobtain file handle");
                }
            }
        }
    };

    struct section : ref_counted_object
    {
        std::u16string name{};
        std::u16string file_name{};
        uint64_t maximum_size{};
        uint32_t section_page_protection{};
        uint32_t allocation_attributes{};
        // Shared backing for a pagefile-backed section: allocated once (lazily, on first map) and reused by
        // every view, so all views of the section see the same memory and section offsets resolve correctly.
        // 0 until allocated. Freed when last section handle is closed.
        uint64_t backing_address{};
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
            buffer.write(this->backing_address);
            buffer.write_optional<winpe::pe_image_basic_info>(this->cached_image_info);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->name);
            buffer.read(this->file_name);
            buffer.read(this->maximum_size);
            buffer.read(this->section_page_protection);
            buffer.read(this->allocation_attributes);
            buffer.read(this->backing_address);
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

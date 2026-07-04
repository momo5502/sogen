#pragma once
#include "emulator_utils.hpp"
#include "handles.hpp"

#include <array>

namespace sogen
{

    class user_handle_table
    {
      public:
        static constexpr uint32_t MAX_HANDLES = 0xFFFF;
        static constexpr size_t CLIENT_MESSAGE_BITS_SIZE = 0xC8;
        static constexpr size_t WND_MESSAGE_BITS_COUNT = FNID_ARRAY_SIZE + 2;
        static constexpr size_t DEF_WINDOW_MSGS_INDEX = FNID_ARRAY_SIZE;
        static constexpr size_t DEF_WINDOW_SPEC_MSGS_INDEX = FNID_ARRAY_SIZE + 1;

        user_handle_table(memory_manager& memory)
            : memory_(&memory)
        {
        }

        void setup(const bool is_wow64_process)
        {
            this->is_wow64_process_ = is_wow64_process;

            used_indices_.resize(MAX_HANDLES, false);

            const auto server_info_size = static_cast<size_t>(page_align_up(sizeof(USER_SERVERINFO)));
            server_info_addr_ = this->allocate_memory(server_info_size, memory_permission::read);

            const auto display_info_size = static_cast<size_t>(page_align_up(sizeof(USER_DISPINFO)));
            display_info_addr_ = this->allocate_memory(display_info_size, memory_permission::read);

            const emulator_object<USER_SERVERINFO> srv_obj(*memory_, server_info_addr_);
            srv_obj.access([&](USER_SERVERINFO& srv) {
                srv.cHandleEntries = MAX_HANDLES - 1; //
                srv.defaultFontHeightScale = -11;
                srv.defaultFontWidthScale = 0;
                srv.systemDpi = 96;
                srv.systemMetrics[0] = 1920; // SM_CXSCREEN
                srv.systemMetrics[1] = 1080; // SM_CYSCREEN
                srv.systemMetrics[2] = 17;   // SM_CXVSCROLL
                srv.systemMetrics[3] = 17;   // SM_CYHSCROLL
                srv.systemMetrics[10] = 17;  // SM_CXHTHUMB
                srv.systemMetrics[11] = 32;  // SM_CXICON
                srv.systemMetrics[12] = 32;  // SM_CYICON
                srv.systemMetrics[19] = 1;   // SM_MOUSEPRESENT
                srv.systemMetrics[20] = 17;  // SM_CYVSCROLL
                srv.systemMetrics[21] = 17;  // SM_CXHSCROLL
                srv.systemMetrics[43] = 3;   // SM_CMOUSEBUTTONS
                srv.systemMetrics[75] = 1;   // SM_MOUSEWHEELPRESENT
                srv.systemMetrics[91] = 1;   // SM_MOUSEHORIZONTALWHEELPRESENT
            });

            const auto handle_table_size = static_cast<size_t>(page_align_up(sizeof(USER_HANDLEENTRY) * MAX_HANDLES));
            handle_table_addr_ = this->allocate_memory(handle_table_size, memory_permission::read);

            const auto wnd_message_bits_size = static_cast<size_t>(page_align_up(get_wnd_message_bits_allocation_size()));
            wnd_message_bits_addr_ = this->allocate_memory(wnd_message_bits_size, memory_permission::read);
            wnd_message_bits_addrs_.fill(0);

            uint64_t wnd_message_bits_cursor = wnd_message_bits_addr_;
            for (size_t i = 0; i < WND_MESSAGE_BITS.size(); ++i)
            {
                const auto byte_size = get_wnd_message_bits_byte_size(WND_MESSAGE_BITS.at(i).max_msgs);
                if (byte_size == 0)
                {
                    continue;
                }

                wnd_message_bits_addrs_.at(i) = wnd_message_bits_cursor;
                memory_->write_memory(wnd_message_bits_cursor, WND_MESSAGE_BITS.at(i).bits.data(), byte_size);
                wnd_message_bits_cursor += align_up(byte_size, alignof(uint32_t));
            }
        }

        emulator_object<USER_SERVERINFO> get_server_info() const
        {
            return {*memory_, server_info_addr_};
        }

        emulator_object<USER_HANDLEENTRY> get_handle_table() const
        {
            return {*memory_, handle_table_addr_};
        }

        emulator_object<USER_DISPINFO> get_display_info() const
        {
            return {*memory_, display_info_addr_};
        }

        USER_WNDMSG get_awm_control_message(const size_t index) const
        {
            return get_wnd_message(index);
        }

        USER_WNDMSG get_def_window_messages() const
        {
            return get_wnd_message(DEF_WINDOW_MSGS_INDEX);
        }

        USER_WNDMSG get_def_window_spec_messages() const
        {
            return get_wnd_message(DEF_WINDOW_SPEC_MSGS_INDEX);
        }

        template <typename T>
        std::pair<handle, emulator_object<T>> allocate_object(handle_types::type type)
        {
            const auto index = find_free_index();

            const auto alloc_size = static_cast<size_t>(page_align_up(sizeof(T)));
            const auto alloc_ptr = this->allocate_memory(alloc_size, memory_permission::read);
            const emulator_object<T> alloc_obj(*memory_, alloc_ptr);

            const emulator_object<USER_HANDLEENTRY> handle_table_obj(*memory_, handle_table_addr_);
            handle_table_obj.access(
                [&](USER_HANDLEENTRY& entry) {
                    entry.pHead = alloc_ptr;
                    entry.bType = get_native_type(type);
                    entry.wUniq = static_cast<uint16_t>(type << 7);
                },
                index);

            used_indices_.at(index) = true;

            return {make_handle(index, type, false), alloc_obj};
        }

        void free_index(uint32_t index)
        {
            if (index >= used_indices_.size() || !used_indices_.at(index))
            {
                return;
            }

            used_indices_.at(index) = false;

            const emulator_object<USER_HANDLEENTRY> handle_table_obj(*memory_, handle_table_addr_);
            handle_table_obj.access(
                [&](USER_HANDLEENTRY& entry) {
                    memory_->release_memory(entry.pHead, 0);
                    entry = {};
                },
                index);
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(server_info_addr_);
            buffer.write(handle_table_addr_);
            buffer.write(display_info_addr_);
            buffer.write(wnd_message_bits_addr_);
            buffer.write(wnd_message_bits_addrs_);
            buffer.write_vector(used_indices_);
            buffer.write(is_wow64_process_);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(server_info_addr_);
            buffer.read(handle_table_addr_);
            buffer.read(display_info_addr_);
            buffer.read(wnd_message_bits_addr_);
            buffer.read(wnd_message_bits_addrs_);
            buffer.read_vector(used_indices_);
            buffer.read(is_wow64_process_);
        }

      private:
        static constexpr size_t WND_MESSAGE_BITS_MAX_INTS = 33;

        struct wnd_message_bits_definition
        {
            uint32_t max_msgs;
            std::array<uint32_t, WND_MESSAGE_BITS_MAX_INTS> bits;
        };

        static constexpr std::array<wnd_message_bits_definition, WND_MESSAGE_BITS_COUNT> WND_MESSAGE_BITS = {{
            {.max_msgs = 0x0318u,
             .bits = {0x00109580u, 0x00030000u, 0x00010000u, 0x00010000u, 0x00000096u, 0x00000000u, 0x00000000u, 0x00FF0000u, 0x00000027u,
                      0x00000100u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0020000Fu, 0x00000000u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u}},
            {.max_msgs = 0x0318u,
             .bits = {0x0210FDA2u, 0x02033800u, 0x00080000u, 0x30010000u, 0x00000086u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00020015u,
                      0x01FC0100u, 0xFFFFFFFFu, 0x00000093u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0020440Fu, 0x00000000u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000008u, 0x00000000u, 0x00000000u, 0x0180000Fu}},
            {.max_msgs = 0x0318u,
             .bits = {0x00108DA2u, 0x00030000u, 0x00000080u, 0x30010000u, 0x00000086u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00380005u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x0004FFFFu, 0x00000000u, 0x00000000u, 0x0020448Fu, 0x0008EC00u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u}},
            {.max_msgs = 0x0402u,
             .bits = {0x031100C0u, 0x0282C100u, 0x00210000u, 0x00010000u, 0x00000004u, 0x00000002u, 0x00000000u, 0x00000000u, 0x00050010u,
                      0x01FC0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00020002u, 0x00000000u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000007u}},
            {.max_msgs = 0x0318u,
             .bits = {0x0010FDA2u, 0x00030000u, 0x00020000u, 0x38010000u, 0x00000086u, 0x3FFF0000u, 0x03FFFBF6u, 0x00000000u, 0x0130E057u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x002044BFu, 0x00000000u,
                      0x00000000u, 0x00000000u, 0x00000176u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0100001Fu}},
            {.max_msgs = 0x0318u,
             .bits = {0x00108DA2u, 0x00030000u, 0x00000080u, 0x30010000u, 0x00000086u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00380005u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x0004FFFFu, 0x00000000u, 0x00000000u, 0x0020448Fu, 0x0008EC00u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0318u,
             .bits = {0x0010B406u, 0x00030000u, 0x00010000u, 0x00010000u, 0x00000096u, 0x0000000Au, 0x00000000u, 0x00000000u, 0x00080000u,
                      0x00000100u, 0x00000000u, 0x000F0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x0000000Au, 0x00000000u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u}},
            {.max_msgs = 0x0288u, .bits = {0x00108006u, 0x00000000u, 0x00000400u, 0x00010000u, 0x00000004u, 0x00000000u, 0x00000000u,
                                           0x00000000u, 0x0000E000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
                                           0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x000001AEu}},
            {.max_msgs = 0x0082u, .bits = {0x00000006u, 0x00000000u, 0x00000000u, 0x00010000u, 0x00000004u}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x0000u, .bits = {}},
            {.max_msgs = 0x033Fu,
             .bits = {0x80119800u, 0x082C08C1u, 0x00032000u, 0x88000000u, 0x0000016Bu, 0x0000C03Fu, 0x00000000u, 0x00000000u, 0x00040062u,
                      0x00000100u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000800u,
                      0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x08880000u, 0x80000000u}},
            {.max_msgs = 0x0349u,
             .bits = {0x030A6040u, 0x0080C002u, 0x042800C0u, 0x00000000u, 0x00001810u, 0x00001000u, 0x00000000u, 0x00000000u, 0x0600E211u,
                      0x01FC0280u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x03005420u, 0x18000400u,
                      0x000EFEEFu, 0x00040000u, 0x000300FEu, 0x00000000u, 0x00002000u, 0x00000040u, 0x02000000u, 0x00000000u, 0x00000200u}},
        }};

        static size_t get_wnd_message_bits_byte_size(const uint32_t max_msgs)
        {
            return max_msgs == 0 ? 0 : ((max_msgs / 32u) + 1u) * sizeof(uint32_t);
        }

        static size_t get_wnd_message_bits_allocation_size()
        {
            size_t size = 0;
            for (const auto& entry : WND_MESSAGE_BITS)
            {
                size += static_cast<size_t>(align_up(get_wnd_message_bits_byte_size(entry.max_msgs), alignof(uint32_t)));
            }
            return size;
        }

        USER_WNDMSG get_wnd_message(const size_t index) const
        {
            if (index >= WND_MESSAGE_BITS.size())
            {
                throw std::out_of_range("Invalid shared wnd message table index");
            }

            USER_WNDMSG message{};
            message.maxMsgs = WND_MESSAGE_BITS.at(index).max_msgs;
            message.abMsgs = wnd_message_bits_addrs_.at(index);
            return message;
        }

        uint32_t find_free_index() const
        {
            for (uint32_t i = 1; i < used_indices_.size(); ++i)
            {
                if (!used_indices_.at(i))
                {
                    return i;
                }
            }
            throw std::runtime_error("No more user handles available");
        }

        static uint8_t get_native_type(handle_types::type type)
        {
            switch (type)
            {
            case handle_types::type::window:
                return TYPE_WINDOW;
            case handle_types::type::menu:
                return TYPE_MENU;
            case handle_types::type::monitor:
                return TYPE_MONITOR;
            case handle_types::type::accelerator_table:
                return TYPE_ACCELTABLE;
            default:
                throw std::runtime_error("Unhandled handle type!");
            }
        }

        uint64_t allocate_memory(const size_t size, const nt_memory_permission permissions)
        {
            const auto allocation_base = this->is_wow64_process_ ? DEFAULT_ALLOCATION_ADDRESS_32BIT : DEFAULT_ALLOCATION_ADDRESS_64BIT;
            const auto base = memory_->find_free_allocation_base(size, allocation_base);
            return memory_->allocate_memory(size, permissions, false, base);
        }

        uint64_t server_info_addr_{};
        uint64_t handle_table_addr_{};
        uint64_t display_info_addr_{};
        uint64_t wnd_message_bits_addr_{};
        std::array<uint64_t, WND_MESSAGE_BITS_COUNT> wnd_message_bits_addrs_{};
        std::vector<bool> used_indices_{};
        memory_manager* memory_{};
        bool is_wow64_process_{};
    };

    template <handle_types::type Type, typename T>
        requires(utils::Serializable<T> && std::is_base_of_v<ref_counted_object, T>)
    class user_handle_store : public generic_handle_store
    {
      public:
        using index_type = uint32_t;
        using value_map = std::map<index_type, T>;

        explicit user_handle_store(user_handle_table& table)
            : table_(&table)
        {
        }

        std::pair<handle, T&> create(memory_interface& memory)
        {
            if (this->block_mutation_)
            {
                throw std::runtime_error("Mutation of user object store is blocked!");
            }

            auto [h, guest_obj] = table_->allocate_object<typename T::guest_type>(Type);

            T new_obj(memory);
            new_obj.guest = std::move(guest_obj);

            const auto index = static_cast<uint32_t>(h.value.id);
            const auto it = this->store_.emplace(index, std::move(new_obj)).first;
            return {h, it->second};
        }

        bool block_mutation(bool blocked)
        {
            std::swap(this->block_mutation_, blocked);
            return blocked;
        }

        handle make_handle(const index_type index) const
        {
            handle h{};
            h.bits = 0;
            h.value.is_pseudo = false;
            h.value.type = Type;
            h.value.id = index;

            return h;
        }

        T* get_by_index(const uint32_t index)
        {
            const auto it = this->store_.find(index);
            if (it == this->store_.end())
            {
                return nullptr;
            }
            return &it->second;
        }

        const T* get_by_index(const uint32_t index) const
        {
            const auto it = this->store_.find(index);
            if (it == this->store_.end())
            {
                return nullptr;
            }
            return &it->second;
        }

        T* get(const handle_value h)
        {
            if (h.type != Type || h.is_pseudo)
            {
                return nullptr;
            }

            return this->get_by_index(static_cast<uint32_t>(h.id));
        }

        const T* get(const handle_value h) const
        {
            if (h.type != Type || h.is_pseudo)
            {
                return nullptr;
            }

            return this->get_by_index(static_cast<uint32_t>(h.id));
        }

        T* get(const handle h)
        {
            return this->get(h.value);
        }

        const T* get(const handle h) const
        {
            return this->get(h.value);
        }

        T* get(const uint64_t h)
        {
            handle hh{};
            hh.bits = h;
            return this->get(hh);
        }

        const T* get(const uint64_t h) const
        {
            handle hh{};
            hh.bits = h;
            return this->get(hh);
        }

        size_t size() const
        {
            return this->store_.size();
        }

        std::optional<handle> duplicate(const handle h) override
        {
            auto* entry = this->get(h);
            if (!entry)
            {
                return std::nullopt;
            }

            ++entry->ref_count;
            return h;
        }

        std::pair<typename value_map::iterator, bool> erase(const value_map::iterator& entry)
        {
            if (this->block_mutation_)
            {
                throw std::runtime_error("Mutation of handle store is blocked!");
            }

            if (entry == this->store_.end())
            {
                return {entry, false};
            }

            if constexpr (handle_detail::has_deleter_function<T>())
            {
                if (!T::deleter(entry->second))
                {
                    return {entry, true};
                }
            }

            auto new_iter = this->store_.erase(entry);
            return {new_iter, true};
        }

        bool erase(const handle_value h)
        {
            if (this->block_mutation_)
            {
                throw std::runtime_error("Mutation of user object store is blocked!");
            }

            if (h.type != Type || h.is_pseudo)
            {
                return false;
            }

            const auto index = static_cast<uint32_t>(h.id);
            const auto entry = this->store_.find(index);

            if (entry == this->store_.end())
            {
                return false;
            }

            if constexpr (handle_detail::has_deleter_function<T>())
            {
                if (!T::deleter(entry->second))
                {
                    return false;
                }
            }

            table_->free_index(index);
            this->store_.erase(entry);

            return true;
        }

        bool erase(const handle h) override
        {
            return this->erase(h.value);
        }

        bool erase(const uint64_t h)
        {
            handle hh{};
            hh.bits = h;
            return this->erase(hh);
        }

        bool erase(const T& value)
        {
            const auto entry = this->find(value);
            if (entry == this->store_.end())
            {
                return false;
            }

            return this->erase(make_handle(entry->first));
        }

        value_map::iterator find(const T& value)
        {
            auto i = this->store_.begin();
            for (; i != this->store_.end(); ++i)
            {
                if (&i->second == &value)
                {
                    break;
                }
            }
            return i;
        }

        value_map::const_iterator find(const T& value) const
        {
            auto i = this->store_.begin();
            for (; i != this->store_.end(); ++i)
            {
                if (&i->second == &value)
                {
                    break;
                }
            }
            return i;
        }

        handle find_handle(const T& value) const
        {
            const auto entry = this->find(value);
            if (entry == this->end())
            {
                return {};
            }
            return this->make_handle(entry->first);
        }

        handle find_handle(const T* value) const
        {
            if (!value)
            {
                return {};
            }
            return this->find_handle(*value);
        }

        value_map::iterator begin()
        {
            return this->store_.begin();
        }
        value_map::const_iterator begin() const
        {
            return this->store_.begin();
        }
        value_map::iterator end()
        {
            return this->store_.end();
        }
        value_map::const_iterator end() const
        {
            return this->store_.end();
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->block_mutation_);
            buffer.write_map(this->store_);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->block_mutation_);
            buffer.read_map(this->store_);
        }

      private:
        user_handle_table* table_;
        bool block_mutation_{false};
        value_map store_{};
    };

} // namespace sogen

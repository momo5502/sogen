#pragma once
#include "emulator_utils.hpp"
#include "handles.hpp"

class user_handle_table
{
  public:
    static constexpr uint32_t MAX_HANDLES = 0xFFFF;

    user_handle_table(memory_manager& memory)
        : memory_(&memory)
    {
    }

    void setup()
    {
        used_indices_.resize(MAX_HANDLES, false);

        const auto server_info_size = static_cast<size_t>(page_align_up(sizeof(USER_SERVERINFO)));
        server_info_addr_ = memory_->allocate_memory(server_info_size, memory_permission::read);

        const auto display_info_size = static_cast<size_t>(page_align_up(sizeof(USER_DISPINFO)));
        display_info_addr_ = memory_->allocate_memory(display_info_size, memory_permission::read);

        const emulator_object<USER_SERVERINFO> srv_obj(*memory_, server_info_addr_);
        srv_obj.access([&](USER_SERVERINFO& srv) {
            srv.cHandleEntries = MAX_HANDLES - 1; //
        });

        const auto handle_table_size = static_cast<size_t>(page_align_up(sizeof(USER_HANDLEENTRY) * MAX_HANDLES));
        handle_table_addr_ = memory_->allocate_memory(handle_table_size, memory_permission::read);
    }

    emulator_object<USER_SHAREDINFO> get_server_info() const
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

    template <typename T>
    std::pair<handle, emulator_object<T>> allocate_object(handle_types::type type)
    {
        const auto index = find_free_index();

        const auto alloc_size = static_cast<size_t>(page_align_up(sizeof(T)));
        const auto alloc_ptr = memory_->allocate_memory(alloc_size, memory_permission::read);
        const emulator_object<T> alloc_obj(*memory_, alloc_ptr);

        const emulator_object<USER_HANDLEENTRY> handle_table_obj(*memory_, handle_table_addr_);
        handle_table_obj.access(
            [&](USER_HANDLEENTRY& entry) {
                entry.pHead = alloc_ptr;
                entry.bType = get_native_type(type);
                entry.wUniq = static_cast<uint16_t>(type << 7);
            },
            index);

        used_indices_[index] = true;

        return {make_handle(index, type, false), alloc_obj};
    }

    void free_index(uint32_t index)
    {
        if (index >= used_indices_.size() || !used_indices_[index])
        {
            return;
        }

        used_indices_[index] = false;

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
        buffer.write_vector(used_indices_);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(server_info_addr_);
        buffer.read(handle_table_addr_);
        buffer.read(display_info_addr_);
        buffer.read_vector(used_indices_);
    }

  private:
    uint32_t find_free_index() const
    {
        for (uint32_t i = 1; i < used_indices_.size(); ++i)
        {
            if (!used_indices_[i])
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
        case handle_types::type::monitor:
            return TYPE_MONITOR;
        default:
            throw std::runtime_error("Unhandled handle type!");
        }
    }

    uint64_t server_info_addr_{};
    uint64_t handle_table_addr_{};
    uint64_t display_info_addr_{};
    std::vector<bool> used_indices_{};
    memory_manager* memory_{};
};

template <handle_types::type Type, typename T>
    requires(utils::Serializable<T> && std::is_base_of_v<ref_counted_object, T>)
class user_handle_store : public generic_handle_store
{
  public:
    using index_type = uint32_t;
    using value_map = serialized_map<index_type, T>;

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

    T* get(const handle_value h)
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

    T* get(const uint64_t h)
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

    std::pair<typename value_map::iterator, bool> erase(const typename value_map::iterator& entry)
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

    typename value_map::iterator find(const T& value)
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

    typename value_map::const_iterator find(const T& value) const
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

    typename value_map::iterator begin()
    {
        return this->store_.begin();
    }
    typename value_map::const_iterator begin() const
    {
        return this->store_.begin();
    }
    typename value_map::iterator end()
    {
        return this->store_.end();
    }
    typename value_map::const_iterator end() const
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

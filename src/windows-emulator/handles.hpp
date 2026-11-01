#pragma once

#include <serialization.hpp>

// NOLINTBEGIN(cppcoreguidelines-use-enum-class)

namespace sogen
{

    struct handle_types
    {
        enum type : uint16_t
        {
            reserved = 0,
            file,
            device,
            event,
            section,
            symlink,
            directory,
            semaphore,
            port,
            thread,
            registry,
            mutant,
            token,
            window,
            menu,
            timer,
            user_timer,
            monitor,
            desktop,
            io_completion,
            wait_completion_packet,
            worker_factory,
            private_namespace,
            process,
            accelerator_table,
            managed_thread,
            job,
            deferred_window_positions,
            thread_state_change,
            debug_object,
        };
    };

    // NOLINTEND(cppcoreguidelines-use-enum-class)

#pragma pack(push)
#pragma pack(1)

    struct handle_value
    {
        // NT handles are multiples of 4: the kernel ignores the low 2 bits and user mode tags them
        // (rpcrt4 keeps private flags there), so an id starting at bit 0 would alias a neighbouring handle.
        uint64_t reserved : 2;
        uint64_t id : 21;
        uint64_t type : 7;
        uint64_t is_system : 1;
        uint64_t is_pseudo : 1;
        uint64_t high_bits : 32;
    };

#pragma pack(pop)

    static_assert(sizeof(handle_value) == 8);

    // TODO: this is a concrete 64bit handle
    struct handle
    {
        union
        {
            handle_value value;
            uint64_t bits;
            std::uint64_t h;
        };
    };

    namespace utils
    {
        inline void serialize(buffer_serializer& buffer, const handle& h)
        {
            buffer.write(h.bits);
        }

        inline void deserialize(buffer_deserializer& buffer, handle& h)
        {
            buffer.read(h.bits);
        }
    }

    inline bool operator==(const handle& h1, const handle& h2)
    {
        uint64_t h1_bits = (h1.bits & 0x00000000FFFFFFFF);
        uint64_t h2_bits = (h2.bits & 0x00000000FFFFFFFF);
        return h1_bits == h2_bits;
    }

    inline bool operator==(const handle& h1, const uint64_t& h2)
    {
        uint64_t h1_bits = (h1.bits & 0x00000000FFFFFFFF);
        uint64_t h2_bits = (h2 & 0x00000000FFFFFFFF);
        return h1_bits == h2_bits;
    }

    inline handle_value get_handle_value(const uint64_t h)
    {
        handle hh{};
        hh.bits = h;
        return hh.value;
    }

    constexpr handle make_handle(const uint32_t id, const handle_types::type type, const bool is_pseudo)
    {
        handle_value value{};

        // value.padding = 0;
        value.id = id;
        value.type = type;
        value.is_system = false;
        value.is_pseudo = is_pseudo;

        return {value};
    }

    constexpr handle make_handle(const uint64_t value)
    {
        handle h{};
        h.bits = value;
        return h;
    }

    constexpr handle make_pseudo_handle(const uint32_t id, const handle_types::type type)
    {
        return make_handle(id, type, true);
    }

    namespace handle_detail
    {
        template <typename, typename = void>
        struct has_deleter_function : std::false_type
        {
        };

        template <typename T>
        struct has_deleter_function<T, std::void_t<decltype(T::deleter(std::declval<T&>()))>>
            : std::is_same<decltype(T::deleter(std::declval<T&>())), bool>
        {
        };
    }

    class ref_counted_object
    {
      public:
        virtual ~ref_counted_object() = default;

        uint32_t ref_count{1};
        bool inherit{};
        bool protect_from_close{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->ref_count);
            buffer.write(this->inherit);
            buffer.write(this->protect_from_close);
            this->serialize_object(buffer);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->ref_count);
            buffer.read(this->inherit);
            buffer.read(this->protect_from_close);
            this->deserialize_object(buffer);
        }

        static bool deleter(ref_counted_object& e)
        {
            if (e.ref_count == 0)
            {
                return true;
            }

            return --e.ref_count == 0;
        }

        virtual void serialize_object(utils::buffer_serializer& buffer) const = 0;
        virtual void deserialize_object(utils::buffer_deserializer& buffer) = 0;
    };

    // On Windows these belong to the handle, not to the object, so two values for one object can differ.
    struct handle_attributes
    {
        bool inherit{};
        bool protect_from_close{};
    };

    struct generic_handle_store
    {
        virtual ~generic_handle_store() = default;
        virtual bool erase(handle h) = 0;
        virtual std::optional<handle> duplicate(handle h) = 0;

        virtual handle_attributes attributes_of(handle)
        {
            return {};
        }

        virtual bool set_attributes(handle, handle_attributes)
        {
            return false;
        }

        virtual ref_counted_object* get_object(handle)
        {
            return nullptr;
        }
    };

    template <handle_types::type Type, typename T>
        requires(utils::Serializable<T> && std::is_base_of_v<ref_counted_object, T>)
    class handle_store : public generic_handle_store
    {
      public:
        using index_type = uint32_t;
        using value_map = std::map<index_type, T>;
        using iterator = value_map::iterator;

        bool block_mutation(bool blocked)
        {
            std::swap(this->block_mutation_, blocked);
            return blocked;
        }

        std::pair<handle, T*> store_and_get(T value)
        {
            if (this->block_mutation_)
            {
                throw std::runtime_error("Mutation of handle store is blocked!");
            }

            auto index = this->find_free_index();
            const auto it = this->store_.emplace(index, std::move(value)).first;

            return {make_handle(index), &it->second};
        }

        handle store(T value)
        {
            return this->store_and_get(std::move(value)).first;
        }

        bool store_at(const handle h, T&& value)
        {
            if (this->block_mutation_)
            {
                throw std::runtime_error("Mutation of handle store is blocked!");
            }
            if (h.value.type != Type || h.value.is_pseudo || h.value.id == 0 ||
                this->aliases_.contains(static_cast<index_type>(h.value.id)))
            {
                return false;
            }
            return this->store_.try_emplace(static_cast<index_type>(h.value.id), std::move(value)).second;
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
            return this->get(this->make_handle(index));
        }

        ref_counted_object* get_object(const handle h) override
        {
            return this->get(h);
        }

        T* get(const handle_value h)
        {
            const auto entry = this->get_iterator(h);
            if (entry == this->store_.end())
            {
                return nullptr;
            }

            return &entry->second;
        }

        T* get(const handle h)
        {
            return this->get(h.value);
        }

        const T* get(const handle_value h) const
        {
            const auto index = this->resolve_index(h);
            if (!index)
            {
                return nullptr;
            }

            const auto entry = this->store_.find(*index);
            return entry == this->store_.end() ? nullptr : &entry->second;
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

        size_t size() const
        {
            return this->store_.size();
        }

        // Every duplicate is a handle value of its own, as on Windows, referring to the same object.
        std::optional<handle> duplicate(const handle h) override
        {
            if (this->block_mutation_)
            {
                throw std::runtime_error("Mutation of handle store is blocked!");
            }

            const auto entry = this->get_iterator(h.value);
            if (entry == this->store_.end())
            {
                return std::nullopt;
            }

            ++static_cast<ref_counted_object&>(entry->second).ref_count;
            const auto index = this->find_free_index();
            this->aliases_.emplace(index, entry->first);
            this->alias_attributes_.emplace(index, handle_attributes{});
            return this->make_handle(index);
        }

        // The value the object was stored under is its first handle, whose attributes live on the object and are
        // what the serialized object carries. Every duplicate keeps its own.
        handle_attributes attributes_of(const handle h) override
        {
            const auto index = static_cast<index_type>(h.value.id);
            if (this->aliases_.contains(index))
            {
                const auto entry = this->alias_attributes_.find(index);
                return entry == this->alias_attributes_.end() ? handle_attributes{} : entry->second;
            }

            const auto* object = this->get(h);
            if (!object)
            {
                return {};
            }

            return {.inherit = object->inherit, .protect_from_close = object->protect_from_close};
        }

        bool set_attributes(const handle h, const handle_attributes attributes) override
        {
            auto* object = this->get(h);
            if (!object)
            {
                return false;
            }

            const auto index = static_cast<index_type>(h.value.id);
            if (this->aliases_.contains(index))
            {
                this->alias_attributes_[index] = attributes;
                return true;
            }

            object->inherit = attributes.inherit;
            object->protect_from_close = attributes.protect_from_close;
            return true;
        }

        // Every value that refers to an object: the one it was stored under and each duplicate.
        std::vector<handle> handle_values() const
        {
            std::vector<handle> values{};
            values.reserve(this->store_.size() + this->aliases_.size());

            for (const auto& entry : this->store_)
            {
                values.push_back(this->make_handle(entry.first));
            }
            for (const auto& alias : this->aliases_)
            {
                values.push_back(this->make_handle(alias.first));
            }

            return values;
        }

        std::pair<iterator, bool> erase(const iterator& entry)
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

            const auto index = entry->first;
            for (const auto& [alias_index, target] : this->aliases_)
            {
                if (target == index)
                {
                    this->alias_attributes_.erase(alias_index);
                }
            }
            std::erase_if(this->aliases_, [&](const auto& alias) { return alias.second == index; });
            auto new_iter = this->store_.erase(entry);
            return {new_iter, true};
        }

        bool erase(const handle_value h)
        {
            if (h.type != Type || h.is_pseudo)
            {
                return false;
            }

            const auto index = static_cast<index_type>(h.id);
            if (const auto alias = this->aliases_.find(index); alias != this->aliases_.end())
            {
                const auto target = alias->second;
                this->aliases_.erase(alias);
                this->alias_attributes_.erase(index);
                return this->erase(this->store_.find(target)).second;
            }

            // The object stays at the index it was stored under for its whole life, even once that first handle is
            // closed and only duplicates remain. Everything that kept the value, such as a file's completion port
            // association or a pending request's event, keeps resolving to it, and the index is not handed out
            // again while the object is there. The object goes when its last reference does.
            return this->erase(this->store_.find(index)).second;
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
            return this->erase(entry);
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->block_mutation_);
            buffer.write_map(this->store_);
            buffer.write_map(this->aliases_);
            buffer.write_map(this->alias_attributes_);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->block_mutation_);
            buffer.read_map(this->store_);
            buffer.read_map(this->aliases_);
            buffer.read_map(this->alias_attributes_);
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

        iterator end()
        {
            return this->store_.end();
        }

        value_map::const_iterator end() const
        {
            return this->store_.end();
        }

      private:
        std::optional<index_type> resolve_index(const handle_value h) const
        {
            if (h.type != Type || h.is_pseudo)
            {
                return std::nullopt;
            }

            const auto index = static_cast<index_type>(h.id);
            const auto alias = this->aliases_.find(index);
            return alias == this->aliases_.end() ? index : alias->second;
        }

        value_map::iterator get_iterator(const handle_value h)
        {
            const auto index = this->resolve_index(h);
            return index ? this->store_.find(*index) : this->store_.end();
        }

        uint32_t find_free_index()
        {
            uint32_t index = 1;
            for (; index > 0; ++index)
            {
                if (!this->store_.contains(index) && !this->aliases_.contains(index))
                {
                    break;
                }
            }

            return index;
        }

        bool block_mutation_{false};
        value_map store_{};
        // Duplicated handle values, mapped to the index their object is stored under.
        std::map<index_type, index_type> aliases_{};
        std::map<index_type, handle_attributes> alias_attributes_{};
    };

    template <handle_types::type Type, typename T>
        requires(utils::Serializable<T> && std::is_base_of_v<ref_counted_object, T>)
    class dummy_handle_store : public generic_handle_store
    {
      public:
        using key_type = uint32_t;
        using value_map = std::map<key_type, T>;

        explicit dummy_handle_store(std::initializer_list<handle> handles)
        {
            for (const auto h : handles)
            {
                if (h.value.type != Type)
                {
                    throw std::invalid_argument("Handle has an unexpected type!");
                }

                const auto key = this->make_key(h);
                const auto [_, inserted] = this->store_.try_emplace(key);

                if (!inserted)
                {
                    throw std::invalid_argument("Duplicate handle!");
                }

                this->handles_.emplace(key, h);
            }
        }

        ref_counted_object* get_object(const handle h) override
        {
            return this->get(h);
        }

        T* get(const handle_value h)
        {
            handle hh{};
            hh.value = h;
            return this->get(hh);
        }

        const T* get(const handle_value h) const
        {
            handle hh{};
            hh.value = h;
            return this->get(hh);
        }

        T* get(const handle h)
        {
            const auto entry = this->store_.find(this->make_key(h));
            return entry == this->store_.end() ? nullptr : &entry->second;
        }

        const T* get(const handle h) const
        {
            const auto entry = this->store_.find(this->make_key(h));
            return entry == this->store_.end() ? nullptr : &entry->second;
        }

        T* get(const uint64_t h)
        {
            return this->get(sogen::make_handle(h));
        }

        const T* get(const uint64_t h) const
        {
            return this->get(sogen::make_handle(h));
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

            ++static_cast<ref_counted_object*>(entry)->ref_count;
            return h;
        }

        bool erase(const handle h) override
        {
            auto* entry = this->get(h);
            if (!entry)
            {
                return false;
            }

            auto& ref_count = static_cast<ref_counted_object*>(entry)->ref_count;
            if (ref_count > 1)
            {
                --ref_count;
            }

            return true;
        }

        bool erase(const handle_value h)
        {
            handle hh{};
            hh.value = h;
            return this->erase(hh);
        }

        bool erase(const uint64_t h)
        {
            return this->erase(sogen::make_handle(h));
        }

        handle find_handle(const T& value) const
        {
            for (const auto& [key, entry] : this->store_)
            {
                if (&entry == &value)
                {
                    return this->handles_.at(key);
                }
            }

            return {};
        }

        handle find_handle(const T* value) const
        {
            return value ? this->find_handle(*value) : handle{};
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

      private:
        key_type make_key(const handle h) const
        {
            return static_cast<key_type>(h.bits & 0x00000000FFFFFFFFULL);
        }

        value_map store_{};
        std::map<key_type, handle> handles_{};
    };

    constexpr auto NULL_HANDLE = make_handle(0ULL);

    constexpr auto KNOWN_DLLS_DIRECTORY = make_pseudo_handle(0x1, handle_types::directory);
    constexpr auto BASE_NAMED_OBJECTS_DIRECTORY = make_pseudo_handle(0x2, handle_types::directory);
    constexpr auto RPC_CONTROL_DIRECTORY = make_pseudo_handle(0x3, handle_types::directory);
    constexpr auto KNOWN_DLLS32_DIRECTORY = make_pseudo_handle(0x4, handle_types::directory);

    constexpr auto KNOWN_DLLS_SYMLINK = make_pseudo_handle(0x1, handle_types::symlink);
    constexpr auto KNOWN_DLLS32_SYMLINK = make_pseudo_handle(0x2, handle_types::symlink);
    constexpr auto SHARED_SECTION = make_pseudo_handle(0x1, handle_types::section);
    constexpr auto DBWIN_BUFFER = make_pseudo_handle(0x2, handle_types::section);

    constexpr auto WER_PORT_READY = make_pseudo_handle(0x1, handle_types::event);
    constexpr auto DBWIN_DATA_READY = make_pseudo_handle(0x2, handle_types::event);
    constexpr auto DBWIN_BUFFER_READY = make_pseudo_handle(0x3, handle_types::event);
    constexpr auto SVCCTRL_START_EVENT = make_pseudo_handle(0x4, handle_types::event);
    constexpr auto LSA_AUTHENTICATION_INITIALIZED = make_pseudo_handle(0x5, handle_types::event);

    constexpr auto CONSOLE_HANDLE = make_pseudo_handle(0x1, handle_types::file);
    constexpr auto STDOUT_HANDLE = make_pseudo_handle(0x2, handle_types::file);
    constexpr auto STDIN_HANDLE = make_pseudo_handle(0x3, handle_types::file);
    constexpr auto NUL_HANDLE = make_pseudo_handle(0x4, handle_types::file);

    constexpr auto DUMMY_IMPERSONATION_TOKEN = make_pseudo_handle(0x1, handle_types::token);
    constexpr auto ANONYMOUS_IMPERSONATION_TOKEN = make_pseudo_handle(0x2, handle_types::token);

    constexpr auto GUEST_PROCESS_HANDLE = make_handle(0x1, handle_types::process, false);

    // Synthetic "Steam client" process. A guest steam_api reads a pid from
    // HKCU\...\Valve\Steam\ActiveProcess\pid and opens it to confirm Steam is running; we hand back a
    // pseudo handle for that one pid so the liveness check passes. STEAM_FAKE_PROCESS_ID must match the
    // pid value seeded into that registry key.
    constexpr uint32_t STEAM_FAKE_PROCESS_ID = 0x8B0;
    constexpr auto STEAM_PROCESS_HANDLE = make_pseudo_handle(0x1, handle_types::process);

    constexpr auto CURRENT_PROCESS = make_handle(~0ULL);
    constexpr auto CURRENT_THREAD = make_handle(~1ULL);

    constexpr auto CURRENT_PROCESS_TOKEN = make_handle(~3ULL);
    constexpr auto CURRENT_THREAD_TOKEN = make_handle(~4ULL);
    constexpr auto CURRENT_THREAD_EFFECTIVE_TOKEN = make_handle(~5ULL);

} // namespace sogen

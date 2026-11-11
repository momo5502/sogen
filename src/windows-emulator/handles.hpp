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
            window_station,
            debug_object,
            cursor,
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

    // Object identity is independent of the handle values that name it. Objects are keyed by a monotonically increasing
    // index from a space the store never forms handles from, so the index can never be mistaken for a handle value and
    // stays meaningful while an internal reference keeps the object alive after its last handle closes.
    struct object_reference
    {
        handle_types::type type{};
        uint64_t index{};
        // Pseudo handles name objects the store does not hold (the current process and thread, the console and so on).
        // They never close, so an internal reference keeps the value itself instead of an index.
        handle pseudo{};

        explicit operator bool() const
        {
            return this->index != 0 || this->pseudo.bits != 0;
        }

        bool is_pseudo() const
        {
            return this->pseudo.bits != 0;
        }

        handle_types::type object_type() const
        {
            if (this->is_pseudo())
            {
                return static_cast<handle_types::type>(this->pseudo.value.type);
            }

            return this->type;
        }

        friend bool operator==(const object_reference& left, const object_reference& right)
        {
            return left.type == right.type && left.index == right.index && left.pseudo.bits == right.pseudo.bits;
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->type);
            buffer.write(this->index);
            buffer.write(this->pseudo);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->type);
            buffer.read(this->index);
            buffer.read(this->pseudo);
        }
    };

    // These belong to a handle value, not to the object, so two handles to one object can differ.
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

        virtual handle_attributes attributes_of(handle) const
        {
            return {};
        }

        virtual bool set_attributes(handle, handle_attributes)
        {
            return false;
        }

        virtual std::optional<object_reference> retain(handle)
        {
            return std::nullopt;
        }

        virtual std::optional<object_reference> retain(uint64_t)
        {
            return std::nullopt;
        }

        virtual void release(object_reference& reference)
        {
            reference = {};
        }

        virtual ref_counted_object* get_object(handle)
        {
            return nullptr;
        }

        virtual std::optional<object_reference> reference_of(handle)
        {
            return std::nullopt;
        }

        virtual ref_counted_object* get_object(const object_reference&)
        {
            return nullptr;
        }

        virtual uint64_t generation() const
        {
            return 0;
        }
    };

    template <handle_types::type Type, typename T>
        requires(utils::Serializable<T> && std::is_base_of_v<ref_counted_object, T>)
    class handle_store : public generic_handle_store
    {
      public:
        using index_type = uint64_t;
        using handle_id_type = uint32_t;
        using value_map = std::map<index_type, T>;
        using handle_map = std::map<handle_id_type, index_type>;
        using iterator = value_map::iterator;

        static constexpr index_type first_object_index = 1ULL << 32;
        static constexpr handle_id_type max_handle_id = (1u << 21) - 1;

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

            const auto index = this->allocate_object_index();
            const auto it = this->store_.emplace(index, std::move(value)).first;
            const auto handle =
                this->open_index(index, false, {.inherit = it->second.inherit, .protect_from_close = it->second.protect_from_close});

            return {handle, &it->second};
        }

        handle store(T value)
        {
            return this->store_and_get(std::move(value)).first;
        }

        bool store_at(const handle h, T value)
        {
            if (this->block_mutation_)
            {
                throw std::runtime_error("Mutation of handle store is blocked!");
            }
            const auto id = static_cast<handle_id_type>(h.value.id);
            if (h.value.type != Type || h.value.is_pseudo || id == 0 || this->handles_.contains(id) || this->internal_handles_.contains(id))
            {
                return false;
            }

            const auto index = this->allocate_object_index();
            const auto [entry, inserted] = this->store_.try_emplace(index, std::move(value));
            if (!inserted)
            {
                return false;
            }

            this->handles_.emplace(id, index);
            this->attributes_[id] = {.inherit = entry->second.inherit, .protect_from_close = entry->second.protect_from_close};
            return true;
        }

        // Opens another handle for the object at index, as opening a named object does. The caller must have verified
        // that the object is open to guest handles or is otherwise still alive.
        handle open_by_index(const index_type index, const handle_attributes& attributes = {})
        {
            if (!this->get_by_index(index))
            {
                return {};
            }

            return this->open_index(index, true, attributes);
        }

        // A stable value for emulator-internal resolution when the guest has closed every value naming the object. It is
        // not counted, enumerated or handed to the guest, and it does not keep the object alive.
        handle internal_handle(const index_type index)
        {
            for (const auto& [id, target] : this->internal_handles_)
            {
                if (target == index)
                {
                    return this->make_handle_from_id(id, true);
                }
            }

            if (!this->get_by_index(index))
            {
                return {};
            }

            const auto id = this->find_free_handle_id();
            this->internal_handles_.emplace(id, index);
            return this->make_handle_from_id(id, true);
        }

        handle internal_handle(const T& value)
        {
            const auto entry = this->find(value);
            return entry == this->store_.end() ? handle{} : this->internal_handle(entry->first);
        }

        // The handle value that currently names the object at index, if any. A value the guest closed is gone from the
        // map, so it can never come back as long as the handle id is not reused.
        handle make_handle(const index_type index) const
        {
            for (const auto& [id, target] : this->handles_)
            {
                if (target == index)
                {
                    return this->make_handle_from_id(id);
                }
            }

            return {};
        }

        T* get_by_index(const index_type index)
        {
            const auto entry = this->store_.find(index);
            return entry == this->store_.end() ? nullptr : &entry->second;
        }

        const T* get_by_index(const index_type index) const
        {
            const auto entry = this->store_.find(index);
            return entry == this->store_.end() ? nullptr : &entry->second;
        }

        ref_counted_object* get_object(const handle h) override
        {
            return this->get(h);
        }

        ref_counted_object* get_object(const object_reference& reference) override
        {
            if (reference.is_pseudo() || reference.type != Type)
            {
                return nullptr;
            }

            return this->get_by_index(reference.index);
        }

        T* get(const handle_value h)
        {
            const auto index = this->resolve_index(h);
            return index ? this->get_by_index(*index) : nullptr;
        }

        const T* get(const handle_value h) const
        {
            const auto index = this->resolve_index(h);
            return index ? this->get_by_index(*index) : nullptr;
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

        size_t size() const
        {
            return this->store_.size();
        }

        size_t handle_count() const
        {
            return this->handles_.size();
        }

        handle handle_for_id(const handle_id_type id) const
        {
            const auto entry = this->handles_.find(id);
            return entry == this->handles_.end() ? handle{} : this->make_handle_from_id(id);
        }

        std::optional<index_type> index_of(const T& value) const
        {
            const auto entry = this->find(value);
            if (entry == this->store_.end())
            {
                return std::nullopt;
            }

            return entry->first;
        }

        // Advances whenever an object of this store is destroyed or the store is rebuilt. Map entries never move, so a
        // pointer to one of its objects stays valid while the generation is unchanged.
        uint64_t generation() const override
        {
            return this->generation_;
        }

        // Every duplicate is a handle value of its own, as on Windows, referring to the same object.
        std::optional<handle> duplicate(const handle h) override
        {
            if (this->block_mutation_)
            {
                throw std::runtime_error("Mutation of handle store is blocked!");
            }

            const auto index = this->resolve_index(h.value);
            if (!index)
            {
                return std::nullopt;
            }

            return this->open_index(*index, true, this->attributes_of(h));
        }

        handle_attributes attributes_of(const handle h) const override
        {
            const auto index = this->resolve_index(h.value);
            if (!index)
            {
                return {};
            }

            const auto entry = this->attributes_.find(static_cast<handle_id_type>(h.value.id));
            return entry == this->attributes_.end() ? handle_attributes{} : entry->second;
        }

        bool set_attributes(const handle h, const handle_attributes attributes) override
        {
            const auto index = this->resolve_index(h.value);
            if (!index)
            {
                return false;
            }

            this->attributes_[static_cast<handle_id_type>(h.value.id)] = attributes;
            return true;
        }

        // Identifies the object without keeping it alive.
        std::optional<object_reference> reference_of(const handle h) override
        {
            const auto index = this->resolve_index(h.value);
            if (!index)
            {
                return std::nullopt;
            }

            return object_reference{.type = Type, .index = *index};
        }

        // Keeps the object alive without giving the guest a handle value for it.
        std::optional<object_reference> retain(const handle h) override
        {
            const auto index = this->resolve_index(h.value);
            if (!index)
            {
                return std::nullopt;
            }

            return this->retain_index(*index);
        }

        std::optional<object_reference> retain(const index_type index) override
        {
            return this->retain_index(index);
        }

        void release(object_reference& reference) override
        {
            if (reference.is_pseudo() || reference.type != Type || reference.index == 0)
            {
                reference = {};
                return;
            }

            this->release_index(reference.index);
            reference = {};
        }

        // Drops one reference. Used by forced object deletion for objects whose last reference is already gone.
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

            return {this->erase_object(entry, true), true};
        }

        bool erase(const handle_value h)
        {
            if (h.type != Type || h.is_pseudo)
            {
                return false;
            }

            const auto index = this->resolve_index(h);
            if (!index)
            {
                return false;
            }

            if (h.is_system)
            {
                this->internal_handles_.erase(static_cast<handle_id_type>(h.id));
                return true;
            }

            const auto entry = this->handles_.find(static_cast<handle_id_type>(h.id));
            if (entry == this->handles_.end() || entry->second != *index)
            {
                return false;
            }

            this->handles_.erase(entry);
            this->attributes_.erase(static_cast<handle_id_type>(h.id));
            this->release_index(*index);
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
            return this->erase(entry);
        }

        // One entry per guest handle, so handle enumeration and counts see duplicates as separate handles.
        std::vector<std::pair<handle, const T*>> handle_entries() const
        {
            std::vector<std::pair<handle, const T*>> entries{};
            entries.reserve(this->handles_.size());

            for (const auto& [id, index] : this->handles_)
            {
                if (const auto* object = this->get_by_index(index))
                {
                    entries.emplace_back(this->make_handle_from_id(id), object);
                }
            }

            return entries;
        }

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->block_mutation_);
            buffer.write(this->next_object_index_);
            buffer.write_map(this->store_);
            buffer.write_map(this->handles_);
            buffer.write_map(this->internal_handles_);
            buffer.write_map(this->attributes_);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->block_mutation_);
            buffer.read(this->next_object_index_);
            buffer.read_map(this->store_);
            buffer.read_map(this->handles_);
            buffer.read_map(this->internal_handles_);
            buffer.read_map(this->attributes_);
            ++this->generation_;
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
        handle make_handle_from_id(const handle_id_type id, const bool is_system = false) const
        {
            handle h{};
            h.bits = 0;
            h.value.is_pseudo = false;
            h.value.type = Type;
            h.value.is_system = is_system;
            h.value.id = id;

            return h;
        }

        std::optional<index_type> resolve_index(const handle_value h) const
        {
            if (h.type != Type || h.is_pseudo)
            {
                return std::nullopt;
            }

            const auto id = static_cast<handle_id_type>(h.id);
            if (h.is_system)
            {
                const auto internal = this->internal_handles_.find(id);
                return internal == this->internal_handles_.end() ? std::nullopt : std::optional<index_type>{internal->second};
            }

            const auto entry = this->handles_.find(id);
            return entry == this->handles_.end() ? std::nullopt : std::optional<index_type>{entry->second};
        }

        std::optional<object_reference> retain_index(const index_type index)
        {
            auto* object = this->get_by_index(index);
            if (!object)
            {
                return std::nullopt;
            }

            ++object->ref_count;
            return object_reference{.type = Type, .index = index};
        }

        void release_index(const index_type index)
        {
            const auto entry = this->store_.find(index);
            if (entry == this->store_.end())
            {
                return;
            }

            if constexpr (handle_detail::has_deleter_function<T>())
            {
                if (!T::deleter(entry->second))
                {
                    return;
                }
            }

            (void)this->erase_object(entry, false);
        }

        iterator erase_object(const typename value_map::iterator& entry, const bool remove_guest_handles)
        {
            const auto index = entry->first;

            for (auto it = this->internal_handles_.begin(); it != this->internal_handles_.end();)
            {
                it = it->second == index ? this->internal_handles_.erase(it) : std::next(it);
            }

            if (remove_guest_handles)
            {
                for (auto it = this->handles_.begin(); it != this->handles_.end();)
                {
                    it = it->second == index ? this->handles_.erase(it) : std::next(it);
                }

                std::erase_if(this->attributes_, [&](const auto& attribute) {
                    const auto handle = this->handles_.find(attribute.first);
                    return handle == this->handles_.end();
                });
            }

            auto new_iter = this->store_.erase(entry);
            ++this->generation_;
            return new_iter;
        }

        index_type allocate_object_index()
        {
            return this->next_object_index_++;
        }

        handle open_index(const index_type index, const bool add_reference, const handle_attributes& attributes)
        {
            auto* object = this->get_by_index(index);
            if (!object)
            {
                throw std::runtime_error("Cannot open a handle for a missing object!");
            }

            if (add_reference)
            {
                ++object->ref_count;
            }

            const auto id = this->find_free_handle_id();
            this->handles_.emplace(id, index);
            this->attributes_[id] = attributes;
            return this->make_handle_from_id(id);
        }

        handle_id_type find_free_handle_id()
        {
            for (handle_id_type id = 1; id <= max_handle_id; ++id)
            {
                if (!this->handles_.contains(id) && !this->internal_handles_.contains(id))
                {
                    return id;
                }
            }

            throw std::runtime_error("Handle store is exhausted!");
        }

        bool block_mutation_{false};
        index_type next_object_index_{first_object_index};
        value_map store_{};
        handle_map handles_{};
        handle_map internal_handles_{};
        std::map<handle_id_type, handle_attributes> attributes_{};
        uint64_t generation_{};
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
    constexpr auto DOS_DEVICES_DIRECTORY = make_pseudo_handle(0x5, handle_types::directory);

    constexpr auto KNOWN_DLLS_SYMLINK = make_pseudo_handle(0x1, handle_types::symlink);
    constexpr auto KNOWN_DLLS32_SYMLINK = make_pseudo_handle(0x2, handle_types::symlink);
    constexpr uint32_t DRIVE_SYMLINK_BASE = 0x100;
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
    constexpr auto PARENT_PROCESS_HANDLE = make_pseudo_handle(0x2, handle_types::process);

    constexpr auto CURRENT_PROCESS = make_handle(~0ULL);
    constexpr auto CURRENT_THREAD = make_handle(~1ULL);

    constexpr auto CURRENT_PROCESS_TOKEN = make_handle(~3ULL);
    constexpr auto CURRENT_THREAD_TOKEN = make_handle(~4ULL);
    constexpr auto CURRENT_THREAD_EFFECTIVE_TOKEN = make_handle(~5ULL);

} // namespace sogen

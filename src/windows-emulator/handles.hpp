#pragma once

#include <serialization.hpp>

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
        timer,
    };
};

#pragma pack(push)
#pragma pack(1)
struct handle_value
{
    uint64_t id : 32;
    uint64_t type : 16;
    uint64_t padding : 14;
    uint64_t is_system : 1;
    uint64_t is_pseudo : 1;
};
#pragma pack(pop)

static_assert(sizeof(handle_value) == 8);

// TODO: this is a concrete 64bit handle
union handle
{
    handle_value value;
    uint64_t bits;
    std::uint64_t h;
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
    return h1.bits == h2.bits;
}

inline bool operator==(const handle& h1, const uint64_t& h2)
{
    return h1.bits == h2;
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

    value.padding = 0;
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

    void serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->ref_count);
        this->serialize_object(buffer);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->ref_count);
        this->deserialize_object(buffer);
    }

    static bool deleter(ref_counted_object& e)
    {
        return --e.ref_count == 0;
    }

  private:
    virtual void serialize_object(utils::buffer_serializer& buffer) const = 0;
    virtual void deserialize_object(utils::buffer_deserializer& buffer) = 0;
};

struct generic_handle_store
{
    virtual ~generic_handle_store() = default;
    virtual bool erase(handle h) = 0;
    virtual std::optional<handle> duplicate(handle h) = 0;
};

template <handle_types::type Type, typename T, uint32_t IndexShift = 0>
    requires(utils::Serializable<T> && std::is_base_of_v<ref_counted_object, T>)
class handle_store : public generic_handle_store
{
  public:
    using index_type = uint32_t;
    using value_map = std::map<index_type, T>;

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

    handle make_handle(const index_type index) const
    {
        handle h{};
        h.bits = 0;
        h.value.is_pseudo = false;
        h.value.type = Type;
        h.value.id = index << IndexShift;

        return h;
    }

    T* get_by_index(const uint32_t index)
    {
        return this->get(this->make_handle(index));
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

        ++static_cast<ref_counted_object*>(entry)->ref_count;
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
        const auto entry = this->get_iterator(h);
        return this->erase(entry).second;
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
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->block_mutation_);
        buffer.read_map(this->store_);
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

  private:
    typename value_map::iterator get_iterator(const handle_value h)
    {
        if (h.type != Type || h.is_pseudo)
        {
            return this->store_.end();
        }

        return this->store_.find(static_cast<uint32_t>(h.id) >> IndexShift);
    }

    uint32_t find_free_index()
    {
        uint32_t index = 1;
        for (; index > 0; ++index)
        {
            if (!this->store_.contains(index))
            {
                break;
            }
        }

        return index;
    }

    bool block_mutation_{false};
    value_map store_{};
};

constexpr auto NULL_HANDLE = make_handle(0ULL);

constexpr auto KNOWN_DLLS_DIRECTORY = make_pseudo_handle(0x1, handle_types::directory);
constexpr auto BASE_NAMED_OBJECTS_DIRECTORY = make_pseudo_handle(0x2, handle_types::directory);

constexpr auto KNOWN_DLLS_SYMLINK = make_pseudo_handle(0x1, handle_types::symlink);
constexpr auto SHARED_SECTION = make_pseudo_handle(0x1, handle_types::section);
constexpr auto DBWIN_BUFFER = make_pseudo_handle(0x2, handle_types::section);

constexpr auto WER_PORT_READY = make_pseudo_handle(0x1, handle_types::event);
constexpr auto DBWIN_DATA_READY = make_pseudo_handle(0x2, handle_types::event);
constexpr auto DBWIN_BUFFER_READY = make_pseudo_handle(0x3, handle_types::event);
constexpr auto SVCCTRL_START_EVENT = make_pseudo_handle(0x4, handle_types::event);

constexpr auto CONSOLE_HANDLE = make_pseudo_handle(0x1, handle_types::file);
constexpr auto STDOUT_HANDLE = make_pseudo_handle(0x2, handle_types::file);
constexpr auto STDIN_HANDLE = make_pseudo_handle(0x3, handle_types::file);

constexpr auto DUMMY_IMPERSONATION_TOKEN = make_pseudo_handle(0x1, handle_types::token);

constexpr auto CURRENT_PROCESS = make_handle(~0ULL);
constexpr auto CURRENT_THREAD = make_handle(~1ULL);

constexpr auto CURRENT_PROCESS_TOKEN = make_handle(~3ULL);
constexpr auto CURRENT_THREAD_TOKEN = make_handle(~4ULL);
constexpr auto CURRENT_THREAD_EFFECTIVE_TOKEN = make_handle(~5ULL);

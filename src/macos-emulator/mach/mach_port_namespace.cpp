#include "../std_include.hpp"
#include "mach_port_namespace.hpp"
#include "mach_msg.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sogen::mach
{
    namespace
    {
        constexpr uint64_t MAX_SNAPSHOT_ENTRIES = 0x100000;
        constexpr uint64_t MAX_SNAPSHOT_QUEUE_LENGTH = 0x10000;
    }

    port_name_t mach_port_namespace::allocate(port_entry entry)
    {
        const auto index = this->take_index();
        if (!index)
        {
            return PORT_NULL;
        }

        const auto generation = this->next_generation(*index);
        const auto id = this->next_port_id_++;

        entry.index = *index;
        entry.generation = generation;
        entry.port_id = id;
        this->entries_[*index] = std::move(entry);
        this->indices_by_port_id_[id] = *index;

        return make_port_name(*index, generation);
    }

    port_name_t mach_port_namespace::allocate_receive_right(const kernel_object object)
    {
        return this->allocate({.has_receive = true, .object = object});
    }

    port_name_t mach_port_namespace::allocate_port_set()
    {
        return this->allocate({.is_port_set = true});
    }

    port_name_t mach_port_namespace::allocate_send_once_right(const port_name_t receive_name)
    {
        const auto* target = this->find(receive_name);
        if (!target || !target->has_receive)
        {
            return PORT_NULL;
        }

        return this->allocate({.has_send_once = true, .send_once_target_id = target->port_id});
    }

    port_name_t mach_port_namespace::insert_send_right(const port_name_t receive_name)
    {
        auto* entry = this->find(receive_name);
        if (!entry || !entry->has_receive || entry->dead || entry->send_urefs >= MAX_UREFS)
        {
            return PORT_NULL;
        }

        ++entry->send_urefs;
        return make_port_name(entry->index, entry->generation);
    }

    std::optional<uint32_t> mach_port_namespace::take_index()
    {
        if (!this->free_indices_.empty())
        {
            const auto index = *this->free_indices_.begin();
            this->free_indices_.erase(this->free_indices_.begin());
            return index;
        }

        if (this->next_index_ > MAX_INDEX)
        {
            return std::nullopt;
        }

        return this->next_index_++;
    }

    uint8_t mach_port_namespace::next_generation(const uint32_t index)
    {
        auto& generation = this->generations_[index];
        generation = static_cast<uint8_t>(generation + 1);
        if (generation == 0)
        {
            generation = 1;
        }

        return generation;
    }

    void mach_port_namespace::detach_from_sets(const uint32_t index)
    {
        for (auto& entry : this->entries_)
        {
            std::erase(entry.second.members, index);
        }
    }

    void mach_port_namespace::destroy_entry(const uint32_t index)
    {
        const auto entry = this->entries_.find(index);
        if (entry != this->entries_.end())
        {
            this->indices_by_port_id_.erase(entry->second.port_id);
        }

        this->detach_from_sets(index);
        this->entries_.erase(index);
        this->free_indices_.insert(index);
    }

    void mach_port_namespace::release_receive_right(port_entry& entry)
    {
        const auto index = entry.index;

        if (entry.send_urefs == 0)
        {
            this->destroy_entry(index);
            return;
        }

        entry.has_receive = false;
        entry.dead = true;
        entry.is_port_set = false;
        entry.strict_guard = false;
        entry.guard.reset();
        entry.queue.clear();
        entry.members.clear();
        this->detach_from_sets(index);
    }

    kern_return_t mach_port_namespace::adjust_urefs(port_entry& entry, const int32_t delta, const bool release_at_zero)
    {
        const auto updated = static_cast<int64_t>(entry.send_urefs) + delta;
        if (updated < 0)
        {
            return kr::invalid_value;
        }

        if (updated > static_cast<int64_t>(MAX_UREFS))
        {
            return kr::urefs_overflow;
        }

        entry.send_urefs = static_cast<uint32_t>(updated);

        if (release_at_zero && entry.send_urefs == 0)
        {
            this->destroy_entry(entry.index);
        }

        return kr::success;
    }

    bool mach_port_namespace::exists(const port_name_t name) const
    {
        return this->find(name) != nullptr;
    }

    port_entry* mach_port_namespace::find(const port_name_t name)
    {
        return const_cast<port_entry*>(std::as_const(*this).find(name));
    }

    const port_entry* mach_port_namespace::find(const port_name_t name) const
    {
        if (name == PORT_NULL || name == PORT_DEAD)
        {
            return nullptr;
        }

        const auto entry = this->entries_.find(port_index(name));
        if (entry == this->entries_.end() || entry->second.generation != port_generation(name))
        {
            return nullptr;
        }

        return &entry->second;
    }

    port_entry* mach_port_namespace::find_by_port_id(const port_id_t id)
    {
        return const_cast<port_entry*>(std::as_const(*this).find_by_port_id(id));
    }

    const port_entry* mach_port_namespace::find_by_port_id(const port_id_t id) const
    {
        const auto mapping = this->indices_by_port_id_.find(id);
        if (mapping == this->indices_by_port_id_.end())
        {
            return nullptr;
        }

        const auto entry = this->entries_.find(mapping->second);
        return entry == this->entries_.end() ? nullptr : &entry->second;
    }

    port_entry* mach_port_namespace::destination_of(const port_name_t name)
    {
        return const_cast<port_entry*>(std::as_const(*this).destination_of(name));
    }

    // Resolved in a single hop rather than by following a chain: allocate_send_once_right only ever
    // targets a receive right, so a longer chain can only come from a corrupted snapshot.
    const port_entry* mach_port_namespace::destination_of(const port_name_t name) const
    {
        const auto* entry = this->find(name);
        if (!entry)
        {
            return nullptr;
        }

        const auto* target = entry->has_send_once ? this->find_by_port_id(entry->send_once_target_id) : entry;
        return (target && target->has_receive) ? target : nullptr;
    }

    kernel_object mach_port_namespace::object_of(const port_name_t name) const
    {
        const auto* entry = this->find(name);
        if (!entry)
        {
            return {};
        }

        if (!entry->has_send_once)
        {
            return entry->object;
        }

        const auto* target = this->find_by_port_id(entry->send_once_target_id);
        return target ? target->object : kernel_object{};
    }

    kern_return_t mach_port_namespace::mod_refs(const port_name_t name, const right_kind right, const int32_t delta)
    {
        auto* entry = this->find(name);
        if (!entry)
        {
            return kr::invalid_name;
        }

        switch (right)
        {
        case right_kind::send:
            if (entry->dead || entry->send_urefs == 0)
            {
                return kr::invalid_right;
            }
            return this->adjust_urefs(*entry, delta, false);

        case right_kind::dead_name:
            if (!entry->dead)
            {
                return kr::invalid_right;
            }
            return this->adjust_urefs(*entry, delta, true);

        case right_kind::receive:
            if (!entry->has_receive)
            {
                return kr::invalid_right;
            }
            if (delta == 0)
            {
                return kr::success;
            }
            if (delta != -1)
            {
                return kr::invalid_value;
            }
            this->release_receive_right(*entry);
            return kr::success;

        case right_kind::send_once:
            if (!entry->has_send_once)
            {
                return kr::invalid_right;
            }
            if (delta == 0)
            {
                return kr::success;
            }
            if (delta != -1)
            {
                return kr::invalid_value;
            }
            this->destroy_entry(entry->index);
            return kr::success;

        case right_kind::port_set:
            if (!entry->is_port_set)
            {
                return kr::invalid_right;
            }
            if (delta == 0)
            {
                return kr::success;
            }
            if (delta != -1)
            {
                return kr::invalid_value;
            }
            this->destroy_entry(entry->index);
            return kr::success;

        default:
            return kr::invalid_value;
        }
    }

    kern_return_t mach_port_namespace::deallocate(const port_name_t name)
    {
        auto* entry = this->find(name);
        if (!entry)
        {
            return kr::invalid_name;
        }

        if (entry->dead)
        {
            return this->adjust_urefs(*entry, -1, true);
        }

        if (entry->has_send_once)
        {
            this->destroy_entry(entry->index);
            return kr::success;
        }

        if (entry->send_urefs > 0)
        {
            return this->adjust_urefs(*entry, -1, false);
        }

        return kr::invalid_right;
    }

    kern_return_t mach_port_namespace::destruct(const port_name_t name, const int32_t send_right_delta, const uint64_t guard)
    {
        auto* entry = this->find(name);
        if (!entry)
        {
            return kr::invalid_name;
        }

        if (!entry->has_receive)
        {
            return kr::invalid_right;
        }

        if (entry->guard.value_or(0) != guard)
        {
            return kr::invalid_argument;
        }

        const auto updated = static_cast<int64_t>(entry->send_urefs) + send_right_delta;
        if (updated < 0)
        {
            return kr::invalid_value;
        }

        if (updated > static_cast<int64_t>(MAX_UREFS))
        {
            return kr::urefs_overflow;
        }

        entry->send_urefs = static_cast<uint32_t>(updated);
        this->release_receive_right(*entry);
        return kr::success;
    }

    kern_return_t mach_port_namespace::guard(const port_name_t name, const uint64_t context, const bool strict)
    {
        auto* entry = this->find(name);
        if (!entry)
        {
            return kr::invalid_name;
        }

        if (!entry->has_receive)
        {
            return kr::invalid_right;
        }

        if (entry->guard.has_value())
        {
            return kr::invalid_argument;
        }

        entry->guard = context;
        entry->strict_guard = strict;
        return kr::success;
    }

    kern_return_t mach_port_namespace::unguard(const port_name_t name, const uint64_t context)
    {
        auto* entry = this->find(name);
        if (!entry)
        {
            return kr::invalid_name;
        }

        if (!entry->guard.has_value() || *entry->guard != context)
        {
            return kr::invalid_argument;
        }

        entry->guard.reset();
        entry->strict_guard = false;
        return kr::success;
    }

    kern_return_t mach_port_namespace::move_member(const port_name_t member, const port_name_t set)
    {
        auto* member_entry = this->find(member);
        if (!member_entry)
        {
            return kr::invalid_name;
        }

        if (!member_entry->has_receive)
        {
            return kr::invalid_right;
        }

        port_entry* set_entry = nullptr;
        if (set != PORT_NULL)
        {
            set_entry = this->find(set);
            if (!set_entry)
            {
                return kr::invalid_name;
            }

            if (!set_entry->is_port_set)
            {
                return kr::invalid_right;
            }
        }

        const auto member_index = member_entry->index;
        this->detach_from_sets(member_index);

        if (set_entry)
        {
            set_entry->members.push_back(member_index);
        }

        return kr::success;
    }

    kern_return_t mach_port_namespace::insert_member(const port_name_t member, const port_name_t set)
    {
        auto* member_entry = this->find(member);
        if (!member_entry)
        {
            return kr::invalid_name;
        }

        if (!member_entry->has_receive || member_entry->is_port_set)
        {
            return kr::invalid_right;
        }

        auto* set_entry = this->find(set);
        if (!set_entry)
        {
            return kr::invalid_name;
        }

        if (!set_entry->is_port_set)
        {
            return kr::invalid_right;
        }

        const auto index = member_entry->index;
        if (std::find(set_entry->members.begin(), set_entry->members.end(), index) != set_entry->members.end())
        {
            return kr::already_in_set;
        }

        set_entry->members.push_back(index);
        return kr::success;
    }

    kern_return_t mach_port_namespace::extract_member(const port_name_t member, const port_name_t set)
    {
        auto* member_entry = this->find(member);
        if (!member_entry)
        {
            return kr::invalid_name;
        }

        auto* set_entry = this->find(set);
        if (!set_entry)
        {
            return kr::invalid_name;
        }

        if (!set_entry->is_port_set)
        {
            return kr::invalid_right;
        }

        const auto index = member_entry->index;
        const auto removed = std::erase(set_entry->members, index);
        return removed == 0 ? kr::not_in_set : kr::success;
    }

    port_entry* mach_port_namespace::first_queued_member(const port_name_t set)
    {
        auto* set_entry = this->find(set);
        if (!set_entry || !set_entry->is_port_set)
        {
            return nullptr;
        }

        for (const auto index : set_entry->members)
        {
            const auto member = this->entries_.find(index);
            if (member != this->entries_.end() && !member->second.queue.empty())
            {
                return &member->second;
            }
        }

        return nullptr;
    }

    std::vector<port_name_t> mach_port_namespace::sets_containing(const port_name_t member) const
    {
        std::vector<port_name_t> names{};

        const auto* member_entry = this->find(member);
        if (!member_entry)
        {
            return names;
        }

        const auto index = member_entry->index;
        for (const auto& [set_index, entry] : this->entries_)
        {
            if (!entry.is_port_set)
            {
                continue;
            }

            if (std::find(entry.members.begin(), entry.members.end(), index) != entry.members.end())
            {
                names.push_back(make_port_name(entry.index, entry.generation));
            }
        }

        return names;
    }

    uint32_t mach_port_namespace::type_of(const port_name_t name) const
    {
        const auto* entry = this->find(name);
        if (!entry)
        {
            return 0;
        }

        if (entry->dead)
        {
            return port_type::dead_name;
        }

        uint32_t type = 0;
        if (entry->has_receive)
        {
            type |= port_type::receive;
        }
        if (entry->send_urefs > 0)
        {
            type |= port_type::send;
        }
        if (entry->has_send_once)
        {
            type |= port_type::send_once;
        }
        if (entry->is_port_set)
        {
            type |= port_type::port_set;
        }

        return type;
    }

    size_t mach_port_namespace::live_port_count() const
    {
        return this->entries_.size();
    }

    std::vector<mach_port_namespace::queued_summary> mach_port_namespace::non_empty_queues() const
    {
        std::vector<queued_summary> result{};

        for (const auto& [index, entry] : this->entries_)
        {
            if (entry.queue.empty())
            {
                continue;
            }

            const auto name = make_port_name(index, entry.generation);
            int32_t id = 0;
            const auto& first = entry.queue.front();
            if (first.size() >= MSG_HEADER_SIZE)
            {
                id = read_msg_header(first).id;
            }
            result.push_back({.name = name, .depth = entry.queue.size(), .first_message_id = id});
        }

        return result;
    }

    void mach_port_namespace::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write<uint64_t>(this->entries_.size());

        for (const auto& [index, entry] : this->entries_)
        {
            buffer.write<uint32_t>(index);
            buffer.write<uint8_t>(entry.generation);
            buffer.write<uint64_t>(entry.port_id);
            buffer.write<bool>(entry.has_receive);
            buffer.write<bool>(entry.has_send_once);
            buffer.write<uint64_t>(entry.send_once_target_id);
            buffer.write<uint32_t>(entry.send_urefs);
            buffer.write<bool>(entry.dead);
            buffer.write<uint8_t>(static_cast<uint8_t>(entry.object.kind));
            buffer.write<uint64_t>(entry.object.id);
            buffer.write<uint32_t>(entry.queue_limit);
            buffer.write_optional(entry.guard);
            buffer.write<bool>(entry.strict_guard);
            buffer.write<bool>(entry.is_port_set);

            buffer.write<uint64_t>(entry.members.size());
            for (const auto member : entry.members)
            {
                buffer.write<uint32_t>(member);
            }

            buffer.write<uint64_t>(entry.queue.size());
            for (const auto& message : entry.queue)
            {
                buffer.write<uint64_t>(message.size());
                if (!message.empty())
                {
                    buffer.write(message.data(), message.size());
                }
            }
        }

        buffer.write<uint32_t>(this->next_index_);
        buffer.write<uint64_t>(this->next_port_id_);

        buffer.write<uint64_t>(this->free_indices_.size());
        for (const auto index : this->free_indices_)
        {
            buffer.write<uint32_t>(index);
        }

        buffer.write_map(this->generations_);
    }

    void mach_port_namespace::deserialize(utils::buffer_deserializer& buffer)
    {
        const auto read_count = [&buffer](const uint64_t limit, const char* what) {
            const auto count = buffer.read<uint64_t>();
            if (count > limit)
            {
                throw std::runtime_error(std::string("Mach port snapshot declares an implausible ") + what);
            }
            return count;
        };

        std::map<uint32_t, port_entry> entries{};
        std::map<port_id_t, uint32_t> indices_by_port_id{};

        const auto entry_count = read_count(MAX_SNAPSHOT_ENTRIES, "port count");
        for (uint64_t i = 0; i < entry_count; ++i)
        {
            const auto index = buffer.read<uint32_t>();
            if (index > MAX_INDEX)
            {
                throw std::runtime_error("Mach port snapshot declares an out of range port index");
            }

            port_entry entry{};
            entry.index = index;
            entry.generation = buffer.read<uint8_t>();
            entry.port_id = buffer.read<uint64_t>();
            entry.has_receive = buffer.read<bool>();
            entry.has_send_once = buffer.read<bool>();
            entry.send_once_target_id = buffer.read<uint64_t>();
            entry.send_urefs = buffer.read<uint32_t>();
            entry.dead = buffer.read<bool>();
            entry.object.kind = static_cast<kernel_object_kind>(buffer.read<uint8_t>());
            entry.object.id = buffer.read<uint64_t>();
            entry.queue_limit = buffer.read<uint32_t>();
            buffer.read_optional(entry.guard);
            entry.strict_guard = buffer.read<bool>();
            entry.is_port_set = buffer.read<bool>();

            const auto member_count = read_count(MAX_SNAPSHOT_ENTRIES, "port set membership");
            entry.members.reserve(static_cast<size_t>(member_count));
            for (uint64_t member = 0; member < member_count; ++member)
            {
                entry.members.push_back(buffer.read<uint32_t>());
            }

            const auto message_count = read_count(MAX_SNAPSHOT_QUEUE_LENGTH, "message queue length");
            for (uint64_t message = 0; message < message_count; ++message)
            {
                const auto size = read_count(MSG_SIZE_MAX, "queued message size");

                std::vector<uint8_t> bytes(static_cast<size_t>(size));
                if (size > 0)
                {
                    buffer.read(bytes.data(), bytes.size());
                }

                entry.queue.push_back(std::move(bytes));
            }

            if (!indices_by_port_id.emplace(entry.port_id, index).second)
            {
                throw std::runtime_error("Mach port snapshot reuses a port id");
            }

            entries[index] = std::move(entry);
        }

        const auto next_index = buffer.read<uint32_t>();
        const auto next_port_id = buffer.read<uint64_t>();

        // A restored id at or above the counter would be handed out again, so a later port would answer to
        // an id a live send-once link already points at.
        if (next_port_id == INVALID_PORT_ID || indices_by_port_id.contains(INVALID_PORT_ID) ||
            (!indices_by_port_id.empty() && indices_by_port_id.rbegin()->first >= next_port_id))
        {
            throw std::runtime_error("Mach port snapshot declares an out of range port id");
        }

        std::set<uint32_t> free_indices{};
        const auto free_count = read_count(MAX_SNAPSHOT_ENTRIES, "free index count");
        for (uint64_t i = 0; i < free_count; ++i)
        {
            free_indices.insert(buffer.read<uint32_t>());
        }

        std::map<uint32_t, uint8_t> generations{};
        buffer.read_map(generations);

        this->entries_ = std::move(entries);
        this->indices_by_port_id_ = std::move(indices_by_port_id);
        this->generations_ = std::move(generations);
        this->free_indices_ = std::move(free_indices);
        this->next_index_ = next_index;
        this->next_port_id_ = next_port_id;
    }
}

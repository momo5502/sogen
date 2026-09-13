#pragma once

#include "mach_types.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <serialization.hpp>

namespace sogen::mach
{
    enum class kernel_object_kind : uint8_t
    {
        none,
        task,
        host,
        host_priv,
        thread,
        semaphore,
        clock,
        voucher,
        bootstrap,
        exception_handler,
        workqueue,
        xpc_service,
        memory_entry,
        window_server,
        render_server,
        window_server_event,
        timer,
        io_master,
        io_object,
        window_server_connection,
    };

    struct kernel_object
    {
        kernel_object_kind kind{kernel_object_kind::none};
        uint64_t id{};
    };

    namespace port_type
    {
        constexpr uint32_t send = 0x10000;
        constexpr uint32_t receive = 0x20000;
        constexpr uint32_t send_once = 0x40000;
        constexpr uint32_t port_set = 0x80000;
        constexpr uint32_t dead_name = 0x100000;
    }

    using port_id_t = uint64_t;

    constexpr port_id_t INVALID_PORT_ID = 0;

    struct port_entry
    {
        uint32_t index{};
        uint8_t generation{};
        port_id_t port_id{INVALID_PORT_ID};
        bool has_receive{};
        bool has_send_once{};
        // xnu's send-once right holds an ipc_port_t pointer, so no amount of name recycling can re-aim it.
        // A name here would be strictly weaker: the generation is 8 bits, so 255 guest-driven alloc/free
        // cycles on one index bring the stale name back onto a different port.
        port_id_t send_once_target_id{INVALID_PORT_ID};
        uint32_t send_urefs{};
        bool dead{};
        kernel_object object{};
        uint32_t queue_limit{PORT_QLIMIT_DEFAULT};
        std::optional<uint64_t> guard{};
        bool strict_guard{};
        std::vector<uint32_t> members{};
        bool is_port_set{};
        std::deque<std::vector<uint8_t>> queue{};
    };

    class mach_port_namespace
    {
      public:
        // A name packs the index into its upper 24 bits, so the namespace runs out of indices long before
        // it runs out of memory.
        static constexpr uint32_t MAX_INDEX = 0x00FFFFFFu;

        // xnu's MACH_PORT_UREFS_MAX; the SDK does not export it.
        static constexpr uint32_t MAX_UREFS = 0x0000FFFFu;

        port_name_t allocate_receive_right(kernel_object object = {});
        port_name_t allocate_port_set();
        port_name_t allocate_send_once_right(port_name_t receive_name);
        port_name_t insert_send_right(port_name_t receive_name);

        bool exists(port_name_t name) const;
        port_entry* find(port_name_t name);
        const port_entry* find(port_name_t name) const;

        port_entry* find_by_port_id(port_id_t id);
        const port_entry* find_by_port_id(port_id_t id) const;

        // The receive right a message sent to `name` lands on: a send-once name resolves to the port it
        // was made against, every other name to itself.
        port_entry* destination_of(port_name_t name);
        const port_entry* destination_of(port_name_t name) const;

        kernel_object object_of(port_name_t name) const;

        kern_return_t mod_refs(port_name_t name, right_kind right, int32_t delta);
        kern_return_t deallocate(port_name_t name);
        kern_return_t destruct(port_name_t name, int32_t send_right_delta, uint64_t guard);
        kern_return_t guard(port_name_t name, uint64_t context, bool strict);
        kern_return_t unguard(port_name_t name, uint64_t context);
        kern_return_t move_member(port_name_t member, port_name_t set);

        // xnu keeps a port's set memberships as waitq links, so a receive right can sit in several sets
        // at once; only move_member is the "leave the others" form. CFRunLoop relies on the difference --
        // it inserts the same wake port into every mode's set.
        kern_return_t insert_member(port_name_t member, port_name_t set);
        kern_return_t extract_member(port_name_t member, port_name_t set);

        // The member of `set` whose queue is non-empty, in insertion order. Null when `set` is not a port
        // set, or when every member is empty.
        port_entry* first_queued_member(port_name_t set);

        std::vector<port_name_t> sets_containing(port_name_t member) const;
        uint32_t type_of(port_name_t name) const;

        size_t live_port_count() const;

        // For the deadlock reports: which ports hold messages nobody drained, and what the first one is.
        struct queued_summary
        {
            port_name_t name{};
            size_t depth{};
            int32_t first_message_id{};
        };

        std::vector<queued_summary> non_empty_queues() const;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        port_name_t allocate(port_entry entry);
        std::optional<uint32_t> take_index();
        uint8_t next_generation(uint32_t index);
        void detach_from_sets(uint32_t index);
        void destroy_entry(uint32_t index);
        void release_receive_right(port_entry& entry);
        kern_return_t adjust_urefs(port_entry& entry, int32_t delta, bool release_at_zero);

        std::map<uint32_t, port_entry> entries_{};
        std::map<port_id_t, uint32_t> indices_by_port_id_{};
        std::map<uint32_t, uint8_t> generations_{};
        std::set<uint32_t> free_indices_{};
        uint32_t next_index_{1};
        port_id_t next_port_id_{1};
    };
}

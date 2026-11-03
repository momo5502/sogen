#pragma once

#include "../std_include.hpp"
#include "../handles.hpp"
#include <serialization_helper.hpp>

namespace sogen
{
    struct syscall_context;
    struct registry_key;

    constexpr ULONG reg_notify_change_name = 0x00000001;
    constexpr ULONG reg_notify_change_last_set = 0x00000004;
    constexpr ULONG reg_notify_thread_agnostic = 0x10000000;
    constexpr ULONG reg_legal_change_filter = 0x1000000F;

    struct registry_notify_request
    {
        uint32_t thread_id{};
        uint64_t thread_ended_waits{};
        handle event{};
        uint64_t apc_routine{};
        uint64_t apc_context{};
        uint64_t io_status_block{};
        bool synchronous{};
        bool thread_agnostic{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->thread_id);
            buffer.write(this->thread_ended_waits);
            buffer.write(this->event);
            buffer.write(this->apc_routine);
            buffer.write(this->apc_context);
            buffer.write(this->io_status_block);
            buffer.write(this->synchronous);
            buffer.write(this->thread_agnostic);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->thread_id);
            buffer.read(this->thread_ended_waits);
            buffer.read(this->event);
            buffer.read(this->apc_routine);
            buffer.read(this->apc_context);
            buffer.read(this->io_status_block);
            buffer.read(this->synchronous);
            buffer.read(this->thread_agnostic);
        }
    };

    // Windows keeps the filter and subtree flag of the first request on a key object for every later request on it,
    // and remembers a change that happens while no request is pending.
    struct registry_notify_block
    {
        ULONG filter{};
        bool watch_tree{};
        bool change_pending{};
        std::vector<registry_notify_request> requests{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->filter);
            buffer.write(this->watch_tree);
            buffer.write(this->change_pending);
            buffer.write_vector(this->requests);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->filter);
            buffer.read(this->watch_tree);
            buffer.read(this->change_pending);
            buffer.read_vector(this->requests);
        }
    };

    namespace registry_notification
    {
        bool has_watchers(const syscall_context& c);
        bool write_pending_status(const syscall_context& c, uint64_t io_status_block);
        NTSTATUS watch(const syscall_context& c, registry_key& key, registry_notify_request request, ULONG filter, bool watch_tree);
        void report_value_change(const syscall_context& c, const registry_key& key);
        void report_sub_key_change(const syscall_context& c, const registry_key& sub_key);
        void close(const syscall_context& c, registry_key& key);
        void end_deleted_key_requests(const syscall_context& c);
        void end_thread_requests(const syscall_context& c, uint32_t thread_id);
    }
} // namespace sogen

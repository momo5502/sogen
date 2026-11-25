#pragma once

#include "syscall_dispatcher.hpp"

namespace sogen
{
    struct clipboard_message_state : completion_state
    {
        uint64_t result{};

      private:
        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write(this->result);
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read(this->result);
        }
    };

    namespace syscalls
    {
        void dispatch_clipboard_message(const syscall_context& c, callback_id id, clipboard_message_state state, const window& win,
                                        uint32_t message, uint64_t w_param);
    }
}

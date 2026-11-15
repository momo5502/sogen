#pragma once

#include "process_manager.hpp"

#include <memory>
#include <optional>

namespace sogen
{
    struct process_session_record
    {
        process_identity identity{};
        std::optional<process_exit> exit{};
    };

    struct process_session_lookup_result
    {
        process_session_record record{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    class process_session
    {
      public:
        explicit process_session(process_session_endpoint remote = {});
        ~process_session();

        process_session(const process_session&) = delete;
        process_session& operator=(const process_session&) = delete;
        process_session(process_session&&) = delete;
        process_session& operator=(process_session&&) = delete;

        process_session_endpoint endpoint() const;
        bool owns_registry() const;
        process_error register_process(process_identity identity);
        process_session_lookup_result lookup(uint32_t process_id) const;
        process_error mark_exit(uint32_t process_id, const process_exit& exit);

      private:
        struct implementation;
        std::unique_ptr<implementation> implementation_;
    };
}

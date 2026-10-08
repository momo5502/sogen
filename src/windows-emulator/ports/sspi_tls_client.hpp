#pragma once

#include "sspi_context.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace sogen::sspi
{
    enum class handshake_status
    {
        continue_needed,
        incomplete_message,
        complete,
        failed,
    };

    struct handshake_result
    {
        handshake_status status{handshake_status::failed};
        std::vector<uint8_t> output_token{};
        size_t missing_size{};
        size_t extra_size{};
    };

    class tls_client
    {
      public:
        static std::unique_ptr<tls_client> create(std::string_view target);

        ~tls_client();
        tls_client(const tls_client&) = delete;
        tls_client& operator=(const tls_client&) = delete;

        handshake_result process(std::span<const uint8_t> input);
        std::optional<provider_context_input> take_handoff_state();

      private:
        struct impl;

        explicit tls_client(std::unique_ptr<impl> state);

        std::unique_ptr<impl> state_;
    };
}

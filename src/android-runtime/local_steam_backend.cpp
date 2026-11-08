#include "std_include.hpp"
#include "local_steam_backend.hpp"

#ifdef SOGEN_HAS_STEAM_LOCAL_PROVIDER
#include <steam_host_backend.h>
#endif

namespace sogen::android
{
#ifdef SOGEN_HAS_STEAM_LOCAL_PROVIDER
    namespace
    {
        class local_steam_backend final : public steam_backend
        {
          public:
            local_steam_backend() = default;

            ~local_steam_backend() override
            {
                sogen_steam_backend_end_session();
            }

            local_steam_backend(const local_steam_backend&) = delete;
            local_steam_backend& operator=(const local_steam_backend&) = delete;
            local_steam_backend(local_steam_backend&&) = delete;
            local_steam_backend& operator=(local_steam_backend&&) = delete;

            uint64_t create_interface(const std::string_view version) override
            {
                const std::string name{version};
                return sogen_steam_backend_create_interface(name.c_str());
            }

            void release_interface(const uint64_t handle) override
            {
                sogen_steam_backend_release(handle);
            }

            int32_t invoke(const uint64_t handle, const uint32_t method_index, const std::span<const std::byte> args,
                           std::vector<std::byte>& out, uint64_t& ret, const uint32_t out_cap) override
            {
                thread_local std::vector<uint8_t> buffer{};
                if (buffer.size() < out_cap)
                {
                    buffer.resize(out_cap);
                }
                uint32_t out_len = 0;
                const int status = sogen_steam_backend_invoke(handle, method_index, reinterpret_cast<const uint8_t*>(args.data()),
                                                              static_cast<uint32_t>(args.size()), buffer.data(), out_cap, &out_len, &ret);
                if (out_len > out_cap)
                {
                    out.clear();
                    return status;
                }
                out.assign(reinterpret_cast<const std::byte*>(buffer.data()), reinterpret_cast<const std::byte*>(buffer.data()) + out_len);
                return status;
            }

            // The provider posts no callbacks and completes no asynchronous calls, so it is given no buffer to fill.
            void run_callbacks(const int32_t pipe, std::vector<std::byte>& out, uint32_t& normal_count, uint32_t& normal_bytes,
                               uint32_t& reverse_count) override
            {
                uint32_t reverse_bytes = 0;
                sogen_steam_backend_run_callbacks(pipe, nullptr, 0, &normal_bytes, &normal_count, &reverse_bytes, &reverse_count);
                out.clear();
            }

            bool get_api_call_result(const int32_t pipe, const uint64_t call, const int32_t callback_id, const uint32_t data_bytes,
                                     std::vector<std::byte>& out, bool& io_failure) override
            {
                uint32_t out_len = 0;
                uint8_t failed = 0;
                sogen_steam_backend_get_api_call_result(pipe, call, callback_id, data_bytes, nullptr, 0, &out_len, &failed);
                io_failure = false;
                out.clear();
                return false;
            }
        };
    }

    std::unique_ptr<steam_backend> create_local_steam_backend(const steam_launch& launch)
    {
        sogen_steam_launch_state state{
            .version = sogen_steam_launch_state_version,
            .app_id = launch.app_id,
            .steam_id = launch.steam_id,
            .language = {},
            .ownership_ticket = launch.ownership_ticket.empty() ? nullptr : launch.ownership_ticket.data(),
            .ownership_ticket_size = static_cast<uint32_t>(launch.ownership_ticket.size()),
        };
        launch.language.copy(state.language, sizeof(state.language) - 1);
        sogen_steam_backend_start_session(&state);
        if (!sogen_steam_backend_init())
        {
            sogen_steam_backend_end_session();
            return nullptr;
        }
        return std::make_unique<local_steam_backend>();
    }
#else
    std::unique_ptr<steam_backend> create_local_steam_backend(const steam_launch&)
    {
        return nullptr;
    }
#endif
}

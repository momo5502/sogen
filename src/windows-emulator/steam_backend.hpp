#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sogen
{
    // Answers the Steamworks calls a guest makes through the SogenSteam device. An embedder can provide one through
    // emulator_interfaces; without one, the device relays to the host's Steam client when the build has it.
    // SECURITY: `args` is guest-controlled and must be treated as hostile; every implementation bounds its reads and caps
    // its allocations.
    class steam_backend
    {
      public:
        steam_backend() = default;
        virtual ~steam_backend() = default;

        steam_backend(const steam_backend&) = delete;
        steam_backend& operator=(const steam_backend&) = delete;
        steam_backend(steam_backend&&) = delete;
        steam_backend& operator=(steam_backend&&) = delete;

        // Returns 0 when the interface does not exist.
        virtual uint64_t create_interface(std::string_view version) = 0;
        virtual void release_interface(uint64_t handle) = 0;

        // `out_cap` is the caller's reply capacity (the guest-sized output buffer). The reply is bounded to it and never
        // truncated silently: a reply that does not fit leaves `out` empty and returns output_too_small, so the guest sees a
        // failed call rather than corrupted data. Returns a steam_bridge::invoke_status.
        virtual int32_t invoke(uint64_t handle, uint32_t method_index, std::span<const std::byte> args, std::vector<std::byte>& out,
                               uint64_t& ret, uint32_t out_cap) = 0;

        // Drains pending host callbacks for `pipe`. `out` receives the normal-record blob (bytes in `normal_bytes`, count
        // `normal_count`) followed by the reverse-record blob (count `reverse_count`).
        virtual void run_callbacks(int32_t pipe, std::vector<std::byte>& out, uint32_t& normal_count, uint32_t& normal_bytes,
                                   uint32_t& reverse_count) = 0;

        // Fetches a completed async-call result; returns true if it is available (`out` holds the result bytes).
        virtual bool get_api_call_result(int32_t pipe, uint64_t call, int32_t callback_id, uint32_t data_bytes, std::vector<std::byte>& out,
                                         bool& io_failure) = 0;
    };
}

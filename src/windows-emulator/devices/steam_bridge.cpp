#include "../std_include.hpp"
#include "steam_bridge.hpp"
#include "../windows_emulator.hpp"

#include <steam_bridge_protocol.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#ifdef SOGEN_STEAM_REAL_BACKEND
#include <steam_host_backend.h>
#endif

namespace sogen
{
    namespace sb = steam_bridge;

    namespace
    {
        // Upper bound on a single invoke's argument blob and its reply, so a guest cannot drive a
        // multi-gigabyte host allocation by declaring a huge (but validly-sized) IOCTL buffer. Comfortably
        // above any real Steamworks call (the guest marshaller caps each variable payload at 16 MiB).
        constexpr uint32_t max_bridge_payload_bytes = 64u << 20;

        // Abstracts whatever answers Steamworks calls, so the transport is independent of the responder.
        // SECURITY: `args` is guest-controlled and must be treated as hostile; every implementation bounds
        // its reads and caps its allocations.
        struct steam_backend
        {
            virtual ~steam_backend() = default;
            virtual sb::interface_handle create_interface(std::string_view version) = 0;
            virtual void release_interface(sb::interface_handle handle) = 0;
            // `out_cap` is the caller's reply capacity (the guest-sized output buffer). The reply is bounded to
            // it and never truncated silently: a reply that does not fit leaves `out` empty and returns
            // output_too_small, so the guest sees a failed call rather than corrupted data.
            virtual int32_t invoke(sb::interface_handle handle, uint32_t method_index, std::span<const std::byte> args,
                                   std::vector<std::byte>& out, uint64_t& ret, uint32_t out_cap) = 0;
            // Drains pending host callbacks for `pipe`. `out` receives the normal-record blob (bytes in
            // `normal_bytes`, count `normal_count`) followed by the reverse-record blob (count `reverse_count`).
            virtual void run_callbacks(int32_t pipe, std::vector<std::byte>& out, uint32_t& normal_count, uint32_t& normal_bytes,
                                       uint32_t& reverse_count) = 0;
            // Fetches a completed async-call result; returns true if available (out = result bytes).
            virtual bool get_api_call_result(int32_t pipe, uint64_t call, int32_t callback_id, uint32_t data_bytes,
                                             std::vector<std::byte>& out, bool& io_failure) = 0;
        };

        // Fallback when the real host Steam backend is unavailable (no SDK at build time, or Steam not
        // running): every call fails cleanly so the guest sees "no interface" rather than a crash.
        struct null_backend : steam_backend
        {
            sb::interface_handle create_interface(std::string_view) override
            {
                return sb::null_interface;
            }

            void release_interface(sb::interface_handle) override
            {
            }

            int32_t invoke(sb::interface_handle, uint32_t, std::span<const std::byte>, std::vector<std::byte>&, uint64_t& ret,
                           uint32_t) override
            {
                ret = 0;
                return static_cast<int32_t>(sb::invoke_status::unknown_interface);
            }

            void run_callbacks(int32_t, std::vector<std::byte>&, uint32_t& normal_count, uint32_t& normal_bytes,
                               uint32_t& reverse_count) override
            {
                normal_count = normal_bytes = reverse_count = 0;
            }

            bool get_api_call_result(int32_t, uint64_t, int32_t, uint32_t, std::vector<std::byte>&, bool& io_failure) override
            {
                io_failure = false;
                return false;
            }
        };

#ifdef SOGEN_STEAM_REAL_BACKEND
        // Forwards to the steam-host-backend static lib, which owns the SDK headers, the real
        // steamclient64.dll connection, and the generated marshalling thunks.
        struct real_backend : steam_backend
        {
            sb::interface_handle create_interface(std::string_view version) override
            {
                const std::string v{version};
                return sogen_steam_backend_create_interface(v.c_str());
            }

            void release_interface(const sb::interface_handle handle) override
            {
                sogen_steam_backend_release(handle);
            }

            int32_t invoke(const sb::interface_handle handle, const uint32_t method_index, std::span<const std::byte> args,
                           std::vector<std::byte>& out, uint64_t& ret, const uint32_t out_cap) override
            {
                // Size the reply buffer to the guest's own output capacity: the backend writes at most this
                // much and reports the reply's true length, so nothing is ever truncated behind our back.
                thread_local std::vector<uint8_t> buffer;
                if (buffer.size() < out_cap)
                {
                    buffer.resize(out_cap);
                }
                uint32_t out_len = 0;
                uint64_t r = 0;
                const int status = sogen_steam_backend_invoke(handle, method_index, reinterpret_cast<const uint8_t*>(args.data()),
                                                              static_cast<uint32_t>(args.size()), buffer.data(), out_cap, &out_len, &r);
                ret = r;
                if (out_len > out_cap)
                {
                    out.clear(); // reply exceeded the guest buffer; the backend wrote nothing (status == output_too_small)
                    return status;
                }
                out.assign(reinterpret_cast<const std::byte*>(buffer.data()), reinterpret_cast<const std::byte*>(buffer.data()) + out_len);
                return status;
            }

            void run_callbacks(int32_t pipe, std::vector<std::byte>& out, uint32_t& normal_count, uint32_t& normal_bytes,
                               uint32_t& reverse_count) override
            {
                thread_local std::vector<uint8_t> buffer(sb::max_callback_batch_bytes);
                uint32_t normal_len = 0, rev_len = 0;
                sogen_steam_backend_run_callbacks(pipe, buffer.data(), static_cast<uint32_t>(buffer.size()), &normal_len, &normal_count,
                                                  &rev_len, &reverse_count);
                normal_bytes = normal_len;
                out.assign(reinterpret_cast<const std::byte*>(buffer.data()),
                           reinterpret_cast<const std::byte*>(buffer.data()) + normal_len + rev_len);
            }

            bool get_api_call_result(int32_t pipe, uint64_t call, int32_t callback_id, uint32_t data_bytes, std::vector<std::byte>& out,
                                     bool& io_failure) override
            {
                thread_local std::vector<uint8_t> buffer(64u * 1024u);
                const uint32_t cap = data_bytes < buffer.size() ? data_bytes : static_cast<uint32_t>(buffer.size());
                uint32_t out_len = 0;
                uint8_t failed = 0;
                const int status =
                    sogen_steam_backend_get_api_call_result(pipe, call, callback_id, cap, buffer.data(), cap, &out_len, &failed);
                io_failure = failed != 0;
                out.assign(reinterpret_cast<const std::byte*>(buffer.data()), reinterpret_cast<const std::byte*>(buffer.data()) + out_len);
                return status == 0;
            }
        };
#endif

        std::unique_ptr<steam_backend> make_backend(windows_emulator& win_emu)
        {
#ifdef SOGEN_STEAM_REAL_BACKEND
            if (sogen_steam_backend_init())
            {
                win_emu.log.info("[steam-bridge] connected to host Steam via steam-host-backend\n");
                return std::make_unique<real_backend>();
            }
            win_emu.log.warn("[steam-bridge] host Steam unavailable; guest Steam calls will fail\n");
#else
            win_emu.log.info("[steam-bridge] built without the Steamworks SDK; guest Steam calls will fail\n");
#endif
            return std::make_unique<null_backend>();
        }

        // Guest opens \\.\SogenSteam and issues one IOCTL per bridge command. Live Steam state cannot be
        // serialized, so (like the GPU bridge) this device does not participate in snapshots.
        //
        // SECURITY: the guest is untrusted. Every buffer length from the io_device_context is validated
        // against the payload struct size before use, the invoke arg blob is bounded by input_buffer_length,
        // and the reply is bounded by output_buffer_length.
        struct steam_bridge_device : io_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
            {
                switch (context.io_control_code)
                {
                case sb::ioctl_get_version:
                    return handle_get_version(win_emu, context);
                case sb::ioctl_create_interface:
                    return handle_create_interface(win_emu, context);
                case sb::ioctl_release_interface:
                    return handle_release_interface(win_emu, context);
                case sb::ioctl_invoke_method:
                    return handle_invoke_method(win_emu, context);
                case sb::ioctl_run_callbacks:
                    return handle_run_callbacks(win_emu, context);
                case sb::ioctl_get_api_call_result:
                    return handle_get_api_call_result(win_emu, context);
                default:
                    win_emu.log.warn("[steam-bridge] Unsupported IOCTL: 0x%X\n", static_cast<unsigned>(context.io_control_code));
                    return STATUS_NOT_SUPPORTED;
                }
            }

            void serialize_object(utils::buffer_serializer&) const override
            {
            }

            void deserialize_object(utils::buffer_deserializer&) override
            {
            }

          private:
            std::unique_ptr<steam_backend> backend_{};

            steam_backend& backend(windows_emulator& win_emu)
            {
                if (!this->backend_)
                {
                    this->backend_ = make_backend(win_emu);
                }
                return *this->backend_;
            }

            static void set_information(const io_device_context& context, const ULONG bytes)
            {
                if (context.io_status_block)
                {
                    context.io_status_block.access([&](IO_STATUS_BLOCK<EmulatorTraits<Emu64>>& block) { block.Information = bytes; });
                }
            }

            template <typename Response>
            static NTSTATUS write_output(windows_emulator& win_emu, const io_device_context& context, const Response& response)
            {
                if (!context.output_buffer || context.output_buffer_length < sizeof(Response))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                emulator_object<Response>{win_emu.emu(), context.output_buffer}.write(response);
                set_information(context, static_cast<ULONG>(sizeof(Response)));
                return STATUS_SUCCESS;
            }

            static NTSTATUS handle_get_version(windows_emulator& win_emu, const io_device_context& context)
            {
                constexpr sb::version_response response{.magic = sb::protocol_magic, .version = sb::protocol_version};
                return write_output(win_emu, context, response);
            }

            NTSTATUS handle_create_interface(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length < sizeof(sb::create_interface_request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const auto request = emulator_object<sb::create_interface_request>{win_emu.emu(), context.input_buffer}.read();

                // The version name is fixed-size and must be NUL-terminated within the buffer.
                const auto& name = request.version;
                const std::string_view raw{name.data(), name.size()};
                const auto nul = raw.find('\0');
                if (nul == std::string_view::npos)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const std::string_view version = raw.substr(0, nul);

                const sb::create_interface_response response{.handle = this->backend(win_emu).create_interface(version)};
                win_emu.log.info("[steam-bridge] create_interface('%.*s') -> handle %llu\n", static_cast<int>(version.size()),
                                 version.data(), static_cast<unsigned long long>(response.handle));
                if (response.handle == sb::null_interface)
                {
                    win_emu.log.warn("[steam-bridge] interface not created: %.*s\n", static_cast<int>(version.size()), version.data());
                }
                return write_output(win_emu, context, response);
            }

            NTSTATUS handle_release_interface(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length < sizeof(sb::release_interface_request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const auto request = emulator_object<sb::release_interface_request>{win_emu.emu(), context.input_buffer}.read();
                this->backend(win_emu).release_interface(request.handle);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_invoke_method(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length < sizeof(sb::invoke_header))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const auto header = emulator_object<sb::invoke_header>{win_emu.emu(), context.input_buffer}.read();

                // Bound the argument blob strictly within the input buffer (subtraction avoids overflow), and
                // cap it so a huge input buffer can't force a multi-gigabyte host allocation below.
                if (header.arg_bytes > context.input_buffer_length - sizeof(sb::invoke_header) ||
                    header.arg_bytes > max_bridge_payload_bytes)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<std::byte> args(header.arg_bytes);
                if (header.arg_bytes)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(sb::invoke_header), args.data(), args.size());
                }

                std::vector<std::byte> out{};
                uint64_t ret = 0;
                uint32_t out_cap = (context.output_buffer && context.output_buffer_length >= sizeof(sb::invoke_response))
                                       ? static_cast<uint32_t>(context.output_buffer_length - sizeof(sb::invoke_response))
                                       : 0;
                // Cap the reply staging buffer the backend sizes from this; a guest-declared 4 GiB output
                // buffer must not force a 4 GiB host allocation. Real replies are far smaller.
                out_cap = std::min(out_cap, max_bridge_payload_bytes);
                const int32_t status = this->backend(win_emu).invoke(header.handle, header.method_index, args, out, ret, out_cap);
                if (status != 0)
                {
                    win_emu.log.warn("[steam-bridge] invoke handle=%llu method=%u -> status %d\n",
                                     static_cast<unsigned long long>(header.handle), header.method_index, status);
                }

                const uint64_t needed = sizeof(sb::invoke_response) + out.size();
                if (!context.output_buffer || context.output_buffer_length < needed)
                {
                    if (context.output_buffer && context.output_buffer_length >= sizeof(sb::invoke_response))
                    {
                        const sb::invoke_response resp{.status = static_cast<int32_t>(sb::invoke_status::output_too_small),
                                                       .out_bytes = static_cast<uint32_t>(out.size()),
                                                       .return_value = 0};
                        emulator_object<sb::invoke_response>{win_emu.emu(), context.output_buffer}.write(resp);
                        set_information(context, static_cast<ULONG>(sizeof(sb::invoke_response)));
                    }
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const sb::invoke_response resp{.status = status, .out_bytes = static_cast<uint32_t>(out.size()), .return_value = ret};
                emulator_object<sb::invoke_response>{win_emu.emu(), context.output_buffer}.write(resp);
                if (!out.empty())
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(sb::invoke_response), out.data(), out.size());
                }
                set_information(context, static_cast<ULONG>(needed));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_run_callbacks(windows_emulator& win_emu, const io_device_context& context)
            {
                int32_t pipe = 0;
                if (context.input_buffer && context.input_buffer_length >= sizeof(sb::run_callbacks_request))
                {
                    pipe =
                        static_cast<int32_t>(emulator_object<sb::run_callbacks_request>{win_emu.emu(), context.input_buffer}.read().pipe);
                }

                std::vector<std::byte> blob{};
                uint32_t normal_count = 0;
                uint32_t normal_bytes = 0;
                uint32_t reverse_count = 0;
                this->backend(win_emu).run_callbacks(pipe, blob, normal_count, normal_bytes, reverse_count);
                if (normal_bytes > blob.size())
                {
                    normal_bytes = static_cast<uint32_t>(blob.size()); // defensive: keep reverse_bytes from underflowing
                }
                if (reverse_count)
                {
                    win_emu.log.info("[steam-bridge] %u reverse callback(s)\n", reverse_count);
                }

                const uint64_t needed = sizeof(sb::run_callbacks_response) + blob.size();
                if (!context.output_buffer || context.output_buffer_length < needed)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                const sb::run_callbacks_response response{.count = normal_count,
                                                          .blob_bytes = normal_bytes,
                                                          .reverse_count = reverse_count,
                                                          .reverse_bytes = static_cast<uint32_t>(blob.size() - normal_bytes)};
                emulator_object<sb::run_callbacks_response>{win_emu.emu(), context.output_buffer}.write(response);
                if (!blob.empty())
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(sb::run_callbacks_response), blob.data(), blob.size());
                }
                set_information(context, static_cast<ULONG>(needed));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_api_call_result(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length < sizeof(sb::api_call_result_request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const auto request = emulator_object<sb::api_call_result_request>{win_emu.emu(), context.input_buffer}.read();

                std::vector<std::byte> data{};
                bool io_failure = false;
                const bool ok = this->backend(win_emu).get_api_call_result(request.pipe, request.call, request.callback_id,
                                                                           request.data_bytes, data, io_failure);

                const uint64_t needed = sizeof(sb::api_call_result_response) + data.size();
                if (!context.output_buffer || context.output_buffer_length < needed)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                const sb::api_call_result_response response{.ok = ok ? 1 : 0,
                                                            .io_failure = static_cast<uint8_t>(io_failure ? 1 : 0),
                                                            .reserved = {},
                                                            .data_bytes = static_cast<uint32_t>(data.size())};
                emulator_object<sb::api_call_result_response>{win_emu.emu(), context.output_buffer}.write(response);
                if (!data.empty())
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(sb::api_call_result_response), data.data(), data.size());
                }
                set_information(context, static_cast<ULONG>(needed));
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<io_device> create_steam_bridge(const device_creation_context&)
    {
        return std::make_unique<steam_bridge_device>();
    }
}

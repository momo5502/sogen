#include "core_messaging_registrar.hpp"

#include "../logger.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        constexpr uint32_t k_request_kind = 0x00000002;
        constexpr uint32_t k_sync_flags = 0x00010000;
        constexpr ULONG k_min_request_size = 0x30;

        enum class registrar_method : uint32_t
        {
            register_thread = 0x00010001,
            register_port = 0x000E0001,
            unregister_port = 0x000F0001,
            resolve_service = 0x00170001,
            open_endpoint = 0x000A0001,
            query_coreui_port = 0x00030001,
            make_identity = 0x00100001,
            bind_service = 0x00180001,
        };

        enum class registrar_reply : uint32_t
        {
            reply_bootstrap = 0x00040000,
            reply_register_port = 0x00110000,
            reply_unregister_port = 0x00120000,
            reply_resolve_service = 0x001D0000,
            reply_open_endpoint = 0x000D0000,
            reply_coreui_port = 0x00060000,
            reply_make_identity = 0x00130000,
            reply_bind_service = 0x001E0000,
        };

        constexpr ULONG o_kind = 0x10;
        constexpr ULONG o_id = 0x14;
        constexpr ULONG o_flags = 0x18;
        constexpr ULONG o_nested = 0x20;
        constexpr ULONG o_body0 = 0x28;
        constexpr ULONG o_body1 = 0x2C;

        constexpr std::array<uint8_t, 16> guid_generated_identity = {
            0xA7, 0x78, 0x8F, 0xF0, 0x89, 0x98, 0x26, 0x4C, 0xBE, 0x93, 0x92, 0x36, 0x31, 0xDD, 0xA2, 0x0F,
        };

        constexpr std::array<uint8_t, 16> guid_input_delivery = {
            0x46, 0x97, 0x07, 0x7D, 0x22, 0xFC, 0xA7, 0x4D, 0xAA, 0x55, 0x13, 0xC6, 0x0D, 0x91, 0x36, 0xB0,
        };

        constexpr std::array<uint8_t, 16> guid_input_system_conversation = {
            0xF8, 0x5B, 0x7D, 0x1E, 0xBD, 0x6D, 0x12, 0x42, 0xA1, 0xC5, 0x59, 0xFE, 0x49, 0xDD, 0x38, 0x50,
        };

        struct request
        {
            ULONG length{};
            uint32_t kind{};
            uint32_t id{};
            uint32_t flags{};
            uint32_t nested_size{};
            uint32_t body0{};
            registrar_method body1{};

            bool common_shape() const
            {
                return kind == k_request_kind && flags == k_sync_flags;
            }

            bool has_body() const
            {
                return nested_size >= 0x08 && length >= 0x30;
            }

            bool matches(const registrar_method method, const uint32_t min_nested, const ULONG min_length) const
            {
                return body1 == method && nested_size >= min_nested && length >= min_length;
            }

            bool bootstrap() const
            {
                return length == 0x30 && nested_size == 0x08 && body0 == 0x02 && body1 == registrar_method::register_thread;
            }
        };

        struct packet
        {
            explicit packet(const uint32_t length, const uint32_t request_id, const uint32_t nested_size)
                : data(length, 0)
            {
                u32(o_kind, 0);
                u32(o_id, request_id);
                u32(o_flags, k_sync_flags);
                u32(o_nested, nested_size);
            }

            std::vector<uint8_t> data;

            void u16(const ULONG offset, const uint16_t value)
            {
                if (offset + sizeof(value) <= data.size())
                {
                    std::memcpy(data.data() + offset, &value, sizeof(value));
                }
            }

            void u32(const ULONG offset, const uint32_t value)
            {
                if (offset + sizeof(value) <= data.size())
                {
                    std::memcpy(data.data() + offset, &value, sizeof(value));
                }
            }

            void u32s(ULONG offset, std::initializer_list<uint32_t> values)
            {
                for (const auto value : values)
                {
                    u32(offset, value);
                    offset += sizeof(uint32_t);
                }
            }

            void bytes(const ULONG offset, const void* source, const size_t size)
            {
                if (source && offset + size <= data.size())
                {
                    std::memcpy(data.data() + offset, source, size);
                }
            }

            void guid(const ULONG offset, const std::array<uint8_t, 16>& value)
            {
                bytes(offset, value.data(), value.size());
            }

            void utf16z(ULONG offset, const char16_t* text)
            {
                if (!text)
                {
                    return;
                }

                for (;;)
                {
                    const auto ch = static_cast<uint16_t>(*text++);
                    u16(offset, ch);
                    offset += sizeof(uint16_t);

                    if (ch == 0)
                    {
                        return;
                    }
                }
            }

            std::vector<uint8_t> finish()
            {
                return std::move(data);
            }
        };

        size_t utf16z_size(const char16_t* text)
        {
            if (!text)
            {
                return 0;
            }

            size_t chars = 0;
            while (text[chars])
            {
                ++chars;
            }

            return (chars + 1) * sizeof(uint16_t);
        }

        bool guid_equal(const uint8_t* lhs, const std::array<uint8_t, 16>& rhs)
        {
            return std::memcmp(lhs, rhs.data(), rhs.size()) == 0;
        }

        request read_request(windows_emulator& win_emu, const lpc_request_context& c)
        {
            return {
                .length = c.send_buffer_length,
                .kind = win_emu.emu().read_memory<uint32_t>(c.send_buffer + o_kind),
                .id = win_emu.emu().read_memory<uint32_t>(c.send_buffer + o_id),
                .flags = win_emu.emu().read_memory<uint32_t>(c.send_buffer + o_flags),
                .nested_size = win_emu.emu().read_memory<uint32_t>(c.send_buffer + o_nested),
                .body0 = win_emu.emu().read_memory<uint32_t>(c.send_buffer + o_body0),
                .body1 = win_emu.emu().read_memory<registrar_method>(c.send_buffer + o_body1),
            };
        }

        std::vector<uint8_t> make_simple_reply(const uint32_t request_id, const uint32_t reply_method)
        {
            packet out(0x30, request_id, 0x08);
            out.u32s(o_body0, {0x02, reply_method});
            return out.finish();
        }

        std::vector<uint8_t> make_bootstrap_reply(const uint32_t request_id)
        {
            packet out(0x60, request_id, 0x38);
            out.u32s(o_body0, {0x0E, static_cast<unsigned>(registrar_reply::reply_bootstrap), 0x04, 0x00, 0x08, 0x7B, 0x00, 0x10, 0x00,
                               0x00, 0x00, 0x00, 0x04, 0x01});
            return out.finish();
        }

        std::vector<uint8_t> make_open_endpoint_reply(const uint32_t request_id)
        {
            packet out(0x4C, request_id, 0x24);
            out.u32s(o_body0, {0x09, static_cast<unsigned>(registrar_reply::reply_open_endpoint), 0x04, 0x00, 0x10});
            return out.finish();
        }

        std::vector<uint8_t> make_identity_reply(const uint32_t request_id)
        {
            packet out(0x44, request_id, 0x1C);
            out.u32s(o_body0, {0x07, static_cast<unsigned>(registrar_reply::reply_make_identity), 0x10});
            out.guid(0x34, guid_generated_identity);
            return out.finish();
        }

        std::vector<uint8_t> make_bind_service_reply(const uint32_t request_id)
        {
            packet out(0x38, request_id, 0x10);
            out.u32s(o_body0, {0x04, static_cast<unsigned>(registrar_reply::reply_bind_service), 0x04, 0x00});
            return out.finish();
        }

        void append_resolve_payload(packet& out, const bool conversation_variant)
        {
            if (conversation_variant)
            {
                out.u32s(0x3C, {0x23, 0x08, 0xF6, 0x00, 0x10, 0x03, 0x03, 0x02, 0x00, 0x38, 0x6C4, 0x8A8, 0x00200004, 0x8000, 0x02, 0x00});
                out.guid(0x7C, guid_input_system_conversation);
                out.u32s(0x8C, {0x00200005, 0x8000, 0x01, 0x280});
                return;
            }

            out.u32s(0x3C, {0x0A, 0x08, 0xF5, 0x00, 0x10, 0x05, 0x03, 0x01, 0x00, 0x38, 0x6C4, 0x8A8, 0x00200010, 0x8000, 0x02, 0x00});
            out.guid(0x7C, guid_input_delivery);
            out.u32s(0x8C, {0x00200011, 0x8000, 0x01, 0x280});
        }

        std::vector<uint8_t> make_resolve_service_reply(const request& req)
        {
            packet out(0x9C, req.id, 0x74);
            out.u32s(o_body0, {0x1D, static_cast<unsigned>(registrar_reply::reply_resolve_service), 0x04, 0x00, 0x04});
            append_resolve_payload(out, req.body0 >= 0x35);
            return out.finish();
        }

        std::vector<uint8_t> make_coreui_port_reply(windows_emulator& win_emu, const lpc_request_context& c, const request& req)
        {
            std::array<uint8_t, 16> requested_guid{};
            win_emu.emu().read_memory(c.send_buffer + 0x4C, requested_guid.data(), requested_guid.size());

            const bool conversation_variant = guid_equal(requested_guid.data(), guid_input_system_conversation);
            const char16_t* name = conversation_variant
                                       ? u"\\BaseNamedObjects\\[CoreUI]-PID(1732)-TID(2216) 1e7d5bf8-6dbd-4212-a1c5-59fe49dd3850"
                                       : u"\\BaseNamedObjects\\[CoreUI]-PID(1732)-TID(2216) 7d079746-fc22-4da7-aa55-13c60d9136b0";

            const uint32_t tail_slot0 = conversation_variant ? 0x03 : 0x02;
            const uint32_t tail_slot1 = conversation_variant ? 0x4A : 0x49;
            const std::array<uint32_t, 25> tail = {
                0x10,       0x00,   0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,       0x00,       0x38,       0x00,   0x00,
                0x00200000, 0x8000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, tail_slot0, tail_slot1, 0x00200001, 0x8000,
            };

            const auto name_size = static_cast<uint32_t>(utf16z_size(name));
            const auto tail_size = static_cast<uint32_t>(sizeof(tail));
            const auto data_size = static_cast<uint32_t>(0x3C + name_size + tail_size);

            packet out(data_size, req.id, data_size - o_body0);
            out.u32s(o_body0, {0x48, static_cast<unsigned>(registrar_reply::reply_coreui_port), 0x04, 0x00, name_size});
            out.utf16z(0x3C, name);

            auto tail_offset = 0x3C + name_size;
            for (const auto value : tail)
            {
                out.u32(tail_offset, value);
                tail_offset += static_cast<ULONG>(sizeof(uint32_t));
            }

            return out.finish();
        }

        struct core_messaging_registrar_port : port
        {
            lpc_request_result handle_request(windows_emulator& win_emu, const lpc_request_context& c) override
            {
                if (c.send_buffer_length < k_min_request_size)
                {
                    win_emu.log.error("CoreMessagingRegistrar request too small: 0x%X\n", static_cast<uint32_t>(c.send_buffer_length));
                    return STATUS_INVALID_PARAMETER;
                }

                const auto req = read_request(win_emu, c);

                if (!req.common_shape())
                {
                    win_emu.log.error("Unsupported CoreMessagingRegistrar common shape: kind=0x%X flags=0x%X\n", req.kind, req.flags);
                    return STATUS_NOT_SUPPORTED;
                }

                if (!req.has_body())
                {
                    win_emu.log.error("Unsupported CoreMessagingRegistrar nested body: len=0x%X nested=0x%X body=[0x%X, 0x%X]\n",
                                      static_cast<uint32_t>(req.length), req.nested_size, req.body0, static_cast<uint32_t>(req.body1));
                    return STATUS_NOT_SUPPORTED;
                }

                switch (req.body1)
                {
                case registrar_method::register_thread:
                    if (req.bootstrap())
                    {
                        return {STATUS_SUCCESS, make_bootstrap_reply(req.id)};
                    }
                    break;

                case registrar_method::register_port:
                    if (req.matches(registrar_method::register_port, 0x1C, 0x44))
                    {
                        return {STATUS_SUCCESS, make_simple_reply(req.id, static_cast<uint32_t>(registrar_reply::reply_register_port))};
                    }
                    break;

                case registrar_method::resolve_service:
                    if (req.matches(registrar_method::resolve_service, 0x40, 0x68))
                    {
                        return {STATUS_SUCCESS, make_resolve_service_reply(req)};
                    }
                    break;

                case registrar_method::open_endpoint:
                    if (req.matches(registrar_method::open_endpoint, 0x24, 0x5C))
                    {
                        return {STATUS_SUCCESS, make_open_endpoint_reply(req.id)};
                    }
                    break;

                case registrar_method::query_coreui_port:
                    if (req.matches(registrar_method::query_coreui_port, 0x44, 0x6C))
                    {
                        return {STATUS_SUCCESS, make_coreui_port_reply(win_emu, c, req)};
                    }
                    break;

                case registrar_method::make_identity:
                    if (req.matches(registrar_method::make_identity, 0x1C, 0x44))
                    {
                        return {STATUS_SUCCESS, make_identity_reply(req.id)};
                    }
                    break;

                case registrar_method::bind_service:
                    if (req.matches(registrar_method::bind_service, 0x10, 0x38))
                    {
                        return {STATUS_SUCCESS, make_bind_service_reply(req.id)};
                    }
                    break;

                case registrar_method::unregister_port:
                    if (req.matches(registrar_method::unregister_port, 0x1C, 0x44))
                    {
                        return {STATUS_SUCCESS, make_simple_reply(req.id, static_cast<uint32_t>(registrar_reply::reply_unregister_port))};
                    }
                    break;
                }

                win_emu.log.error("Unsupported CoreMessagingRegistrar request: len=0x%X kind=0x%X id=0x%X flags=0x%X "
                                  "nested=0x%X body=[0x%X, 0x%X]\n",
                                  static_cast<uint32_t>(req.length), req.kind, req.id, req.flags, req.nested_size, req.body0,
                                  static_cast<uint32_t>(req.body1));
                return STATUS_NOT_SUPPORTED;
            }
        };
    }

    std::unique_ptr<port> create_core_messaging_registrar_port()
    {
        return std::make_unique<core_messaging_registrar_port>();
    }

} // namespace sogen

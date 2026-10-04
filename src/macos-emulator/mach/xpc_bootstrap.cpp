#include "../std_include.hpp"
#include "xpc_bootstrap.hpp"

#include "../macos_emulator.hpp"
#include "mig_kernel_servers.hpp"
#include "xpc_services.hpp"

#include "../gui/macos_window_server_mig.hpp"

#include <algorithm>
#include <set>

namespace sogen::xpc
{
    namespace
    {
        constexpr size_t ALIGNMENT = 4;
        constexpr size_t UUID_SIZE = 16;

        size_t padded(const size_t length)
        {
            return (length + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        }

        void append_u32(std::vector<uint8_t>& out, const uint32_t v)
        {
            std::array<uint8_t, sizeof(uint32_t)> scratch{};
            mach::write_u32(scratch, 0, v);
            out.insert(out.end(), scratch.begin(), scratch.end());
        }

        void append_u64(std::vector<uint8_t>& out, const uint64_t v)
        {
            std::array<uint8_t, sizeof(uint64_t)> scratch{};
            mach::write_u64(scratch, 0, v);
            out.insert(out.end(), scratch.begin(), scratch.end());
        }

        // Keys are NUL-terminated and NUL-padded to a 4-byte boundary.
        void append_key(std::vector<uint8_t>& out, const std::string& key)
        {
            out.insert(out.end(), key.begin(), key.end());
            out.resize(out.size() + (padded(key.size() + 1) - key.size()), 0);
        }

        void write_value(std::vector<uint8_t>& out, const value& v);

        void write_dictionary(std::vector<uint8_t>& out, const value& v)
        {
            std::vector<uint8_t> entries{};
            for (const auto& entry : v.entries)
            {
                append_key(entries, entry.first);
                write_value(entries, entry.second);
            }

            // size counts from the count field to the end of the entries, not from the type tag.
            append_u32(out, static_cast<uint32_t>(sizeof(uint32_t) + entries.size()));
            append_u32(out, static_cast<uint32_t>(v.entries.size()));
            out.insert(out.end(), entries.begin(), entries.end());
        }

        void write_value(std::vector<uint8_t>& out, const value& v)
        {
            append_u32(out, v.type_tag);

            if (v.type_tag == type::uint64 || v.type_tag == type::int64)
            {
                append_u64(out, v.number);
                return;
            }

            if (v.type_tag == type::boolean)
            {
                append_u32(out, v.number != 0 ? 1u : 0u);
                return;
            }

            // An endpoint is a bare tag: the port right it names travels as a mach descriptor, never
            // inline. Measured 2026-08-27 from launchd's lookup replies.
            if (v.type_tag == type::endpoint)
            {
                return;
            }

            if (v.type_tag == type::uuid)
            {
                out.insert(out.end(), v.bytes.begin(), v.bytes.end());
                out.resize(out.size() + (UUID_SIZE - std::min(v.bytes.size(), UUID_SIZE)), 0);
                return;
            }

            if (v.type_tag == type::data)
            {
                // A data's declared length excludes any padding; its bytes are then padded to 4.
                append_u32(out, static_cast<uint32_t>(v.bytes.size()));
                out.insert(out.end(), v.bytes.begin(), v.bytes.end());
                out.resize(padded(out.size()), 0);
                return;
            }

            if (v.type_tag == type::string)
            {
                // A string's declared length INCLUDES its NUL; its bytes are then padded to 4.
                append_u32(out, static_cast<uint32_t>(v.text.size() + 1));
                out.insert(out.end(), v.text.begin(), v.text.end());
                out.resize(out.size() + (padded(v.text.size() + 1) - v.text.size()), 0);
                return;
            }

            if (v.type_tag == type::dictionary)
            {
                write_dictionary(out, v);
            }
        }

        bool read_u32_at(const std::span<const uint8_t> bytes, const size_t offset, uint32_t& out)
        {
            if (offset > bytes.size() || sizeof(uint32_t) > bytes.size() - offset)
            {
                return false;
            }

            out = mach::read_u32(bytes, offset);
            return true;
        }

        std::optional<value> read_value(std::span<const uint8_t> bytes, size_t& offset, uint32_t* unsupported_tag);

        std::optional<value> read_dictionary(const std::span<const uint8_t> bytes, size_t& offset, uint32_t* unsupported_tag)
        {
            uint32_t size = 0;
            uint32_t count = 0;
            if (!read_u32_at(bytes, offset, size) || !read_u32_at(bytes, offset + 4, count))
            {
                return std::nullopt;
            }

            offset += 8;

            value result{};
            result.type_tag = type::dictionary;

            for (uint32_t i = 0; i < count; ++i)
            {
                // The third observed launchd send declares two entries it has no room for. Returning what
                // is readable and flagging the value beats hard-failing on a message the real system sends.
                if (offset >= bytes.size())
                {
                    result.incomplete = true;
                    break;
                }

                size_t key_length = 0;
                while (offset + key_length < bytes.size() && bytes[offset + key_length] != 0)
                {
                    ++key_length;
                }

                if (offset + key_length >= bytes.size())
                {
                    result.incomplete = true;
                    break;
                }

                std::string key(reinterpret_cast<const char*>(bytes.data() + offset), key_length);
                offset += padded(key_length + 1);

                auto entry = read_value(bytes, offset, unsupported_tag);
                if (!entry.has_value())
                {
                    result.incomplete = true;
                    break;
                }

                result.entries.emplace_back(std::move(key), std::move(*entry));
            }

            return result;
        }

        std::optional<value> read_value(const std::span<const uint8_t> bytes, size_t& offset, uint32_t* const unsupported_tag)
        {
            uint32_t tag = 0;
            if (!read_u32_at(bytes, offset, tag))
            {
                return std::nullopt;
            }

            offset += 4;

            value result{};
            result.type_tag = tag;

            if (tag == type::uint64 || tag == type::int64)
            {
                if (offset > bytes.size() || sizeof(uint64_t) > bytes.size() - offset)
                {
                    return std::nullopt;
                }

                result.number = mach::read_u64(bytes, offset);
                offset += 8;
                return result;
            }

            if (tag == type::boolean)
            {
                uint32_t raw = 0;
                if (!read_u32_at(bytes, offset, raw))
                {
                    return std::nullopt;
                }

                result.number = raw;
                offset += 4;
                return result;
            }

            if (tag == type::endpoint || tag == type::null)
            {
                return result;
            }

            if (tag == type::string)
            {
                uint32_t length = 0;
                if (!read_u32_at(bytes, offset, length))
                {
                    return std::nullopt;
                }

                offset += 4;

                if (length == 0 || offset > bytes.size() || length > bytes.size() - offset)
                {
                    return std::nullopt;
                }

                result.text.assign(reinterpret_cast<const char*>(bytes.data() + offset), length - 1);
                offset += padded(length);
                return result;
            }

            if (tag == type::uuid)
            {
                if (offset > bytes.size() || UUID_SIZE > bytes.size() - offset)
                {
                    return std::nullopt;
                }

                result.bytes.assign(bytes.begin() + static_cast<ptrdiff_t>(offset),
                                    bytes.begin() + static_cast<ptrdiff_t>(offset + UUID_SIZE));
                offset += UUID_SIZE;
                return result;
            }

            if (tag == type::data)
            {
                uint32_t length = 0;
                if (!read_u32_at(bytes, offset, length))
                {
                    return std::nullopt;
                }

                offset += 4;

                if (offset > bytes.size() || length > bytes.size() - offset)
                {
                    return std::nullopt;
                }

                result.bytes.assign(bytes.begin() + static_cast<ptrdiff_t>(offset),
                                    bytes.begin() + static_cast<ptrdiff_t>(offset + length));
                offset += padded(length);
                return result;
            }

            if (tag == type::dictionary)
            {
                return read_dictionary(bytes, offset, unsupported_tag);
            }

            if (unsupported_tag != nullptr)
            {
                *unsupported_tag = tag;
            }

            return std::nullopt;
        }
    }

    value make_uint64(const uint64_t v)
    {
        return {.type_tag = type::uint64, .number = v};
    }

    value make_string(std::string v)
    {
        return {.type_tag = type::string, .text = std::move(v)};
    }

    value make_endpoint(const uint32_t v)
    {
        return {.type_tag = type::endpoint, .number = v};
    }

    value make_data(std::vector<uint8_t> v)
    {
        return {.type_tag = type::data, .bytes = std::move(v)};
    }

    value make_dictionary(dictionary entries)
    {
        return {.type_tag = type::dictionary, .entries = std::move(entries)};
    }

    std::vector<uint8_t> serialize(const value& root)
    {
        std::vector<uint8_t> out{};
        append_u32(out, MAGIC);
        append_u32(out, VERSION);
        write_value(out, root);
        return out;
    }

    std::optional<value> parse(const std::span<const uint8_t> bytes, uint32_t* const unsupported_tag)
    {
        uint32_t magic = 0;
        uint32_t version = 0;
        if (!read_u32_at(bytes, 0, magic) || !read_u32_at(bytes, 4, version) || magic != MAGIC)
        {
            return std::nullopt;
        }

        size_t offset = 8;
        return read_value(bytes, offset, unsupported_tag);
    }

    const value* find(const value& dictionary_value, const std::string_view key)
    {
        for (const auto& entry : dictionary_value.entries)
        {
            if (entry.first == key)
            {
                return &entry.second;
            }
        }

        return nullptr;
    }

    namespace
    {
        // The two routine ids libxpc's initialiser sends to the bootstrap port before main(). It calls
        // abort_with_payload if they fail, so answering them is what keeps a guest alive -- sogen does not
        // need launchd itself. 0x400000cf is the richer service-lookup variant libxpc and
        // bootstrap_look_up3 use once CoreFoundation is in play; its reply shape is identical to a
        // 0x40000324 lookup's (both measured 2026-08-27).
        constexpr int32_t XPC_ROUTINE_CHECKIN = 0x40000323;
        constexpr int32_t XPC_ROUTINE_LOOKUP = 0x40000324;
        constexpr int32_t XPC_ROUTINE_LOOKUP_BY_NAME = 0x400000cf;

        // The bit libxpc sets on every routine it sends to the bootstrap port. A message without it is
        // not one of these at all, and answering it as if it were would be worse than declining.
        constexpr int32_t XPC_ROUTINE_FLAG = 0x40000000;
    }

    bool bootstrap_responder::is_xpc_routine(const int32_t routine)
    {
        return (routine & XPC_ROUTINE_FLAG) != 0;
    }

    namespace
    {
        const char* lookup_outcome(const xpc_service_decision decision)
        {
            switch (decision)
            {
            case xpc_service_decision::refuse_connection_invalid:
                return "refused per message";
            case xpc_service_decision::window_server:
                return "the window server";
            case xpc_service_decision::answer:
                break;
            }

            return "answered";
        }

        std::vector<uint8_t> answer_service_lookup(macos_emulator& emu, const mach::msg_call& call, const std::string_view name,
                                                   const xpc_service_decision decision)
        {
            const auto service_port = decision == xpc_service_decision::window_server
                                          ? macos_window_server_session_port(emu)
                                          : emu.mach.ports.allocate_receive_right({.kind = mach::kernel_object_kind::xpc_service});

            emu.log.info("XPC lookup of %.*s -> service port 0x%x (%s)\n", static_cast<int>(name.size()), name.data(), service_port,
                         lookup_outcome(decision));

            mach::mig_reply_builder builder{call, emu.mach.ports};
            builder.append_port_descriptor({
                .name = service_port,
                .disposition =
                    decision == xpc_service_decision::window_server ? mach::disposition::copy_send : mach::disposition::make_send,
                .type = mach::descriptor_type::port,
            });

            // Measured 2026-08-27 from launchd's reply to the same lookup on the host: the port right is
            // descriptor 0, and the dictionary pairs it with an endpoint placeholder.
            builder.append_bytes(serialize(make_dictionary({
                {"rec_execcnt", make_uint64(1)},
                {"req_pid", make_uint64(emu.process.pid)},
                {"port", make_endpoint(0)},
            })));
            return builder.finish_with_id(XPC_REPLY_ID);
        }

        // Measured 2026-08-27 from launchd's reply to a lookup of a service that does not exist: no
        // descriptor, and an "error" entry instead of the "port" entry.
        std::vector<uint8_t> answer_unknown_service_lookup(macos_emulator& emu, const mach::msg_call& call, const std::string_view name)
        {
            emu.log.info("XPC lookup of %.*s -> no such service\n", static_cast<int>(name.size()), name.data());

            mach::mig_reply_builder builder{call, emu.mach.ports};
            builder.append_bytes(serialize(make_dictionary({
                {"rec_execcnt", make_uint64(1)},
                {"req_pid", make_uint64(emu.process.pid)},
                {"error", {.type_tag = type::int64, .number = 3}},
            })));
            return builder.finish_with_id(XPC_REPLY_ID);
        }
    }

    std::optional<std::vector<uint8_t>> bootstrap_responder::respond(macos_emulator& emu, const mach::msg_call& call,
                                                                     const std::span<const uint8_t> body)
    {
        const bool is_lookup = call.header.id == XPC_ROUTINE_LOOKUP || call.header.id == XPC_ROUTINE_LOOKUP_BY_NAME;
        if (call.header.id != XPC_ROUTINE_CHECKIN && !is_lookup)
        {
            return std::nullopt;
        }

        // A by-name lookup arrives complex (it carries a bootstrap-port descriptor); the TLV starts after
        // the descriptors, which is exactly the offset make_mig_request computes.
        const auto tlv_offset = mach::make_mig_request(call, body, mach::kernel_object_kind::bootstrap).args_offset;

        uint64_t handle = 0;
        uint64_t kind = VERSION;

        // Echo what the request carried where it carried it. A malformed or truncated request still gets a
        // well-formed reply -- the third observed launchd send is itself truncated.
        uint32_t unsupported_tag = 0;
        const auto request = tlv_offset <= body.size() ? parse(body.subspan(tlv_offset), &unsupported_tag) : std::nullopt;
        if (unsupported_tag != 0)
        {
            static std::set<uint32_t> reported{};
            if (reported.insert(unsupported_tag).second)
            {
                emu.log.warn("unimplemented XPC value tag 0x%x in a message to the bootstrap port\n", unsupported_tag);
            }
        }
        if (request.has_value())
        {
            if (is_lookup)
            {
                if (const auto* name = find(*request, "name"); name != nullptr)
                {
                    if (const auto decision = decide_xpc_service(name->text); decision.has_value())
                    {
                        return answer_service_lookup(emu, call, name->text, *decision);
                    }

                    return answer_unknown_service_lookup(emu, call, name->text);
                }
            }

            if (const auto* value = find(*request, "handle"); value != nullptr)
            {
                handle = value->number;
            }

            if (const auto* value = find(*request, "type"); value != nullptr)
            {
                kind = value->number;
            }
        }

        mach::mig_reply_builder builder{call, emu.mach.ports};
        builder.append_bytes(serialize(make_dictionary({
            {"handle", make_uint64(handle)},
            {"type", make_uint64(kind)},
        })));
        return builder.finish(0);
    }
}

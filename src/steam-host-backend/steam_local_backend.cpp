// Local backend: answers the guest's Steam calls inside the emulator, for hosts without a Steam client. It vends
// the exact SteamClient roots the guest asks for and the interfaces their getters name, and reports every other call
// with its interface and method name, so the calls a game makes can be listed before they are implemented.

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "steam_host_runtime.hpp"
#include "steam_host_backend.h"

#include "steam_tags.generated.hxx"
#define SOGEN_STEAM_DECL_METADATA(tag)                                       \
    extern "C" uint32_t sogen_steam_method_count_##tag(const char* version); \
    extern "C" const char* sogen_steam_method_name_##tag(const char* version, uint32_t index);
SOGEN_STEAM_TAGS(SOGEN_STEAM_DECL_METADATA)
#undef SOGEN_STEAM_DECL_METADATA

namespace
{
    namespace sh = sogen::steam_host;

    constexpr uint64_t local_pipe = 1;
    constexpr uint64_t local_user = 1;

    // An interface the guest opened, with its method names looked up once, since a game calls some of them every frame.
    struct open_interface_record
    {
        std::string version{};
        std::vector<const char*> method_names{};
        std::vector<bool> reported{};
    };

    std::mutex g_mutex;
    std::unordered_map<uint64_t, open_interface_record> g_interfaces;
    uint64_t g_next_handle = 1;
    sogen_steam_launch_state g_launch{};

    void report(const char* format, ...);

    // Where the fields a game asks about lie in an app ownership ticket: the signed part starts with its own length,
    // then a version, the SteamID at 8 and the AppID at 16; the signature covers it and follows it.
    struct ownership_ticket
    {
        std::vector<uint8_t> bytes{};
        uint32_t signature_offset{};
    };

    ownership_ticket g_ticket{};

    uint32_t read_u32(const std::vector<uint8_t>& bytes, const size_t offset)
    {
        uint32_t value{};
        for (size_t i = 0; i < 4; ++i)
        {
            value |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
        }
        return value;
    }

    uint64_t read_u64(const std::vector<uint8_t>& bytes, const size_t offset)
    {
        return static_cast<uint64_t>(read_u32(bytes, offset)) | (static_cast<uint64_t>(read_u32(bytes, offset + 4)) << 32);
    }

    constexpr size_t ticket_steam_id_offset = 8;
    constexpr size_t ticket_app_id_offset = 16;

    // Keeps a ticket only when it is for the launch's account and app and its signed part lies inside it.
    ownership_ticket validate_ticket(const uint8_t* data, const uint32_t size)
    {
        if (!data || size < ticket_app_id_offset + 4)
        {
            return {};
        }

        std::vector<uint8_t> bytes(data, data + size);
        const auto signed_length = read_u32(bytes, 0);
        report("ownership ticket: %u bytes, signed length field %u", size, signed_length);
        if (signed_length < ticket_app_id_offset + 4 || signed_length >= size)
        {
            report("ownership ticket rejected: signed length outside the ticket");
            return {};
        }

        if (read_u64(bytes, ticket_steam_id_offset) != g_launch.steam_id || read_u32(bytes, ticket_app_id_offset) != g_launch.app_id)
        {
            report("ownership ticket rejected: not for this account and app");
            return {};
        }

        return {.bytes = std::move(bytes), .signature_offset = signed_length};
    }

    void report(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
#ifdef __ANDROID__
        __android_log_vprint(ANDROID_LOG_INFO, "sogen-steam", format, args);
#else
        std::vfprintf(stderr, format, args);
        std::fputc('\n', stderr);
#endif
        va_end(args);
    }

    // Like the guest shim, takes the snapshot with the most methods for a version, so method indices mean the same.
    struct method_table
    {
        uint32_t count{};
        const char* (*name)(const char*, uint32_t){};
    };

    method_table find_method_table(const char* version)
    {
        method_table table{};
#define SOGEN_STEAM_PICK_TABLE(tag)                                                              \
    if (const uint32_t methods = sogen_steam_method_count_##tag(version); methods > table.count) \
    {                                                                                            \
        table = {.count = methods, .name = &sogen_steam_method_name_##tag};                      \
    }
        SOGEN_STEAM_TAGS(SOGEN_STEAM_PICK_TABLE)
#undef SOGEN_STEAM_PICK_TABLE
        return table;
    }

    // Interfaces whose calls a game uses to decide that Steam is there and must be trusted, while the provider cannot
    // answer them yet. Without the interface the game takes its path for a missing Steam client; with it, a refused
    // call fails a check the game would otherwise skip (the app ownership ticket a DRM wrapper verifies).
    bool is_withheld(const std::string_view version)
    {
        return version.starts_with("STEAMAPPTICKET_INTERFACE_VERSION") && g_ticket.bytes.empty();
    }

    uint64_t open_interface(const std::string_view version)
    {
        const std::string name{version};
        const auto table = find_method_table(name.c_str());
        if (table.count == 0)
        {
            report("unknown interface %s", name.c_str());
            return 0;
        }

        if (is_withheld(name))
        {
            report("withheld interface %s", name.c_str());
            return 0;
        }

        for (const auto& [handle, record] : g_interfaces)
        {
            if (record.version == name)
            {
                return handle;
            }
        }

        open_interface_record record{.version = name, .reported = std::vector<bool>(table.count)};
        record.method_names.reserve(table.count);
        for (uint32_t method = 0; method < table.count; ++method)
        {
            record.method_names.push_back(table.name(name.c_str(), method));
        }

        const auto handle = g_next_handle++;
        g_interfaces.emplace(handle, std::move(record));
        return handle;
    }

    bool is_root(const std::string& version)
    {
        return version.starts_with("SteamClient");
    }

    // The SteamClient calls that hand out the pipe, the user and the other interfaces.
    bool answer_root_call(const std::string_view method, sh::steam_host_reader& in, uint64_t& ret)
    {
        if (method == "CreateSteamPipe")
        {
            ret = local_pipe;
            return true;
        }

        if (method == "BReleaseSteamPipe" || method == "BShutdownIfAllPipesClosed")
        {
            ret = 1;
            return true;
        }

        if (method == "ConnectToGlobalUser")
        {
            ret = local_user;
            return true;
        }

        if (method == "ReleaseUser")
        {
            return true;
        }

        if (method.starts_with("GetISteam"))
        {
            // GetISteamUtils(pipe, version); every other getter takes (user, pipe, version).
            const auto handle_arguments = method == "GetISteamUtils" ? 1 : 2;
            for (int i = 0; i < handle_arguments; ++i)
            {
                uint64_t ignored{};
                in.get_scalar(&ignored, sizeof(ignored));
            }

            ret = open_interface(in.get_cstr());
            return true;
        }

        return false;
    }

    // A string answer, written the way the generated thunks write one: its bytes and a NUL, with a nonzero return
    // value when it is not empty.
    struct string_reply
    {
        std::string_view value{};
        bool present{};
    };

    // Per-frame calls that only pump input events Steam would deliver; there is nothing for them to do here.
    bool is_input_pump(const std::string& version, const std::string_view method)
    {
        return method == "RunFrame" && (version.starts_with("SteamInput") || version.starts_with("SteamController") ||
                                        version.starts_with("STEAMCONTROLLER_INTERFACE_VERSION") || version.starts_with("SteamUtils"));
    }

    bool answer_apps_call(const std::string_view method, string_reply& reply)
    {
        if (method == "GetCurrentGameLanguage" && g_launch.language[0] != '\0')
        {
            reply = {.value = g_launch.language, .present = true};
            return true;
        }

        return false;
    }

    // Reports each unanswered (interface, method) once, so that a call made every frame costs no log line per frame.
    void report_unsupported(open_interface_record& record, const char* name, const uint32_t method)
    {
        if (!record.reported[method])
        {
            record.reported[method] = true;
            report("%s %s #%u unsupported", record.version.c_str(), name, method);
        }
    }

    bool answer_user_call(const std::string_view method, uint64_t& ret)
    {
        if (method == "GetSteamID")
        {
            ret = g_launch.steam_id;
            return true;
        }

        return false;
    }

    bool answer_utils_call(const std::string_view method, uint64_t& ret)
    {
        if (method == "GetAppID")
        {
            ret = g_launch.app_id;
            return true;
        }

        return false;
    }
}

namespace sogen::steam_host
{
    uint64_t register_returned_interface(const char* version, void*)
    {
        return version ? open_interface(version) : 0;
    }

    void* host_resolve_fallback(const char*)
    {
        return nullptr;
    }
}

extern "C" int sogen_steam_backend_init(void)
{
    return 1;
}

extern "C" void sogen_steam_backend_start_session(const sogen_steam_launch_state* state)
{
    std::lock_guard lock{g_mutex};
    g_interfaces.clear();
    g_next_handle = 1;
    g_launch = {};
    if (state && state->version >= 1)
    {
        g_launch.version = sogen_steam_launch_state_version;
        g_launch.app_id = state->app_id;
    }
    if (state && state->version >= 2)
    {
        g_launch.steam_id = state->steam_id;
    }
    if (state && state->version >= 3)
    {
        std::memcpy(g_launch.language, state->language, sizeof(g_launch.language));
        g_launch.language[sizeof(g_launch.language) - 1] = '\0';
    }
    g_ticket = state && state->version >= 4 ? validate_ticket(state->ownership_ticket, state->ownership_ticket_size) : ownership_ticket{};
    report("session for app %u", g_launch.app_id);
}

extern "C" void sogen_steam_backend_end_session(void)
{
    std::lock_guard lock{g_mutex};
    g_interfaces.clear();
    g_next_handle = 1;
    g_launch = {};
    // A plain fill could be dropped as a dead store, since the bytes are released right after.
    volatile auto* ticket = g_ticket.bytes.data();
    for (size_t i = 0; i < g_ticket.bytes.size(); ++i)
    {
        ticket[i] = 0;
    }
    g_ticket = {};
}

extern "C" uint64_t sogen_steam_backend_create_interface(const char* version)
{
    if (!version)
    {
        return 0;
    }

    const std::string_view name{version, strnlen(version, 64)};
    std::lock_guard lock{g_mutex};
    if (!name.starts_with("SteamClient"))
    {
        report("refused root %.*s", static_cast<int>(name.size()), name.data());
        return 0;
    }

    return open_interface(name);
}

extern "C" void sogen_steam_backend_release(uint64_t handle)
{
    std::lock_guard lock{g_mutex};
    g_interfaces.erase(handle);
}

extern "C" int sogen_steam_backend_invoke(uint64_t handle, uint32_t method, const uint8_t* in, uint32_t in_len, uint8_t* out,
                                          uint32_t out_cap, uint32_t* out_len, uint64_t* ret)
{
    if (out_len)
    {
        *out_len = 0;
    }
    if (ret)
    {
        *ret = 0;
    }

    std::lock_guard lock{g_mutex};
    const auto entry = g_interfaces.find(handle);
    if (entry == g_interfaces.end())
    {
        return sh::steam_host_unknown_interface;
    }

    auto& record = entry->second;
    const auto& version = record.version;
    const char* name = method < record.method_names.size() ? record.method_names[method] : nullptr;
    if (!name)
    {
        report("%s #%u: no such method", version.c_str(), method);
        return sh::steam_host_unknown_method;
    }

    if (is_input_pump(version, name))
    {
        return sh::steam_host_ok;
    }

    if (version.starts_with("STEAMAPPTICKET_INTERFACE_VERSION") && std::string_view{name} == "GetAppOwnershipTicketData")
    {
        sh::steam_host_reader args{in, in + (in ? in_len : 0)};
        uint32_t app_id{};
        uint32_t capacity{};
        args.get_scalar(&app_id, sizeof(app_id));
        args.get_scalar(&capacity, sizeof(capacity));

        const auto& ticket = g_ticket.bytes;
        const bool fits = app_id == g_launch.app_id && !ticket.empty() && ticket.size() <= capacity;
        const auto ticket_size = static_cast<uint32_t>(ticket.size());
        const uint32_t offsets[] = {
            static_cast<uint32_t>(ticket_app_id_offset),
            static_cast<uint32_t>(ticket_steam_id_offset),
            g_ticket.signature_offset,
            ticket_size - g_ticket.signature_offset,
        };

        // The reply holds the guest's whole buffer, then the four offsets.
        const uint64_t needed = uint64_t{capacity} + sizeof(offsets);
        if (needed > out_cap || !out)
        {
            if (out_len)
            {
                *out_len = static_cast<uint32_t>(std::min<uint64_t>(needed, UINT32_MAX));
            }
            return sh::steam_host_output_too_small;
        }
        if (out_len)
        {
            *out_len = static_cast<uint32_t>(needed);
        }
        std::memset(out, 0, static_cast<size_t>(needed));
        if (fits)
        {
            std::memcpy(out, ticket.data(), ticket.size());
            std::memcpy(out + capacity, offsets, sizeof(offsets));
        }
        if (ret)
        {
            *ret = fits ? ticket_size : 0;
        }
        report("%s %s #%u -> %s", version.c_str(), name, method, fits ? "ticket" : "none");
        return sh::steam_host_ok;
    }

    if (string_reply reply{}; version.starts_with("STEAMAPPS_INTERFACE_VERSION") && answer_apps_call(name, reply))
    {
        const auto needed = static_cast<uint32_t>(reply.value.size() + 1);
        if (out_len)
        {
            *out_len = needed;
        }
        if (needed > out_cap)
        {
            return sh::steam_host_output_too_small;
        }
        if (out)
        {
            std::memcpy(out, reply.value.data(), reply.value.size());
            out[reply.value.size()] = 0;
        }
        if (ret)
        {
            *ret = reply.present && !reply.value.empty() ? 1 : 0;
        }
        report("%s %s #%u -> %.*s", version.c_str(), name, method, static_cast<int>(reply.value.size()), reply.value.data());
        return sh::steam_host_ok;
    }

    sh::steam_host_reader reader{in, in + (in ? in_len : 0)};
    uint64_t result{};
    bool answered = false;
    if (is_root(version))
    {
        answered = answer_root_call(name, reader, result);
    }
    else if (version.starts_with("SteamUtils"))
    {
        answered = answer_utils_call(name, result);
    }
    else if (version.starts_with("SteamUser"))
    {
        answered = answer_user_call(name, result);
    }
    if (answered)
    {
        report("%s %s #%u -> %llu", version.c_str(), name, method, static_cast<unsigned long long>(result));
        if (ret)
        {
            *ret = result;
        }
        return sh::steam_host_ok;
    }

    report_unsupported(record, name, method);
    return sh::steam_host_unsupported;
}

extern "C" int sogen_steam_backend_run_callbacks(int32_t /*pipe*/, uint8_t* /*out*/, uint32_t /*out_cap*/, uint32_t* normal_len,
                                                 uint32_t* normal_count, uint32_t* reverse_len, uint32_t* reverse_count)
{
    for (auto* value : {normal_len, normal_count, reverse_len, reverse_count})
    {
        if (value)
        {
            *value = 0;
        }
    }
    return sh::steam_host_ok;
}

extern "C" int sogen_steam_backend_get_api_call_result(int32_t /*pipe*/, uint64_t call, int32_t callback_id, uint32_t /*data_bytes*/,
                                                       uint8_t* /*out*/, uint32_t /*out_cap*/, uint32_t* out_len, uint8_t* io_failure)
{
    if (out_len)
    {
        *out_len = 0;
    }
    if (io_failure)
    {
        *io_failure = 0;
    }
    report("call result %llu (callback %d) unsupported", static_cast<unsigned long long>(call), callback_id);
    return sh::steam_host_unsupported;
}

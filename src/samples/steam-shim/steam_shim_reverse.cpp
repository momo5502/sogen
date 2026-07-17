// Reverse-callback dispatch for the guest shim. Game-implemented response objects (the matchmaking server
// browser) are registered here; the host replays their calls back and we invoke the real object. Compiled
// once against the base SDK -- the ISteamMatchmaking*Response interfaces are stable across versions, so one
// definition serves every version tag. register_response_object itself is SDK-type-free (void* + type id).
#include "steam_shim_runtime.hpp" // base SDK: response interface types + the register_response_object decl

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    struct response_entry
    {
        void* obj;
        int32_t type;
    };

    std::mutex g_resp_mutex;
    std::unordered_map<uint64_t, response_entry> g_responses;
    std::unordered_map<void*, uint64_t> g_obj_tokens; // dedup by object pointer -> one token per object
    uint64_t g_resp_next = 1;

    // Sequential reader over a reverse-call arg blob.
    struct rd
    {
        const unsigned char* p;
        const unsigned char* end;

        template <typename T>
        T get()
        {
            T v{};
            if (p + sizeof(T) <= end)
            {
                std::memcpy(&v, p, sizeof(T));
                p += sizeof(T);
            }
            return v;
        }

        void bytes(void* dst, size_t n)
        {
            if (p + n <= end)
            {
                std::memcpy(dst, p, n);
                p += n;
            }
            else
            {
                std::memset(dst, 0, n);
            }
        }

        const char* cstr()
        {
            const char* s = reinterpret_cast<const char*>(p);

            while (p < end && *p)
            {
                ++p;
            }
            if (p >= end)
            {
                return ""; // no terminator within the record
            }
            ++p;
            return s;
        }
    };
}

namespace sogen::steam_shim
{
    uint64_t register_response_object(void* obj, int32_t type)
    {
        if (!obj)
        {
            return 0;
        }
        std::lock_guard lock{g_resp_mutex};
        // Reuse the token (and thus the single host proxy) for a response object we've seen before, so a
        // game that refreshes repeatedly with the same object doesn't accumulate host proxies.
        if (const auto it = g_obj_tokens.find(obj); it != g_obj_tokens.end())
        {
            g_responses[it->second] = {.obj = obj, .type = type};
            return it->second;
        }
        const uint64_t token = g_resp_next++;
        g_responses[token] = {.obj = obj, .type = type};
        g_obj_tokens[obj] = token;
        return token;
    }
}

// Called from the shim's callback drain when the host reports a reverse call for `token`.
extern "C" void sogen_dispatch_reverse(uint64_t token, int32_t method, const void* data, uint32_t bytes)
{
    response_entry e{};
    {
        std::lock_guard lock{g_resp_mutex};
        const auto it = g_responses.find(token);
        if (it == g_responses.end())
        {
            return;
        }
        e = it->second;
    }
    rd r{.p = static_cast<const unsigned char*>(data), .end = static_cast<const unsigned char*>(data) + bytes};

    switch (static_cast<sogen::steam_bridge::response_type>(e.type))
    {
    case sogen::steam_bridge::response_type::matchmaking_server_list: {
        auto* o = static_cast<ISteamMatchmakingServerListResponse*>(e.obj);
        auto* const h = reinterpret_cast<HServerListRequest>(r.get<uint64_t>());
        const int iServer = r.get<int32_t>();
        if (method == 0)
        {
            o->ServerResponded(h, iServer);
        }
        else if (method == 1)
        {
            o->ServerFailedToRespond(h, iServer);
        }
        else if (method == 2)
        {
            o->RefreshComplete(h, static_cast<EMatchMakingServerResponse>(iServer));
        }
        break;
    }
    case sogen::steam_bridge::response_type::matchmaking_ping: {
        auto* o = static_cast<ISteamMatchmakingPingResponse*>(e.obj);
        if (method == 0)
        {
            gameserveritem_t item{};
            r.bytes(&item, sizeof(item));
            o->ServerResponded(item);
        }
        else if (method == 1)
        {
            o->ServerFailedToRespond();
        }
        break;
    }
    case sogen::steam_bridge::response_type::matchmaking_players: {
        auto* o = static_cast<ISteamMatchmakingPlayersResponse*>(e.obj);
        if (method == 0)
        {
            const char* name = r.cstr();
            const int score = r.get<int32_t>();
            const auto t = r.get<float>();
            o->AddPlayerToList(name, score, t);
        }
        else if (method == 1)
        {
            o->PlayersFailedToRespond();
        }
        else if (method == 2)
        {
            o->PlayersRefreshComplete();
        }
        break;
    }
    case sogen::steam_bridge::response_type::matchmaking_rules: {
        auto* o = static_cast<ISteamMatchmakingRulesResponse*>(e.obj);
        if (method == 0)
        {
            const char* rule = r.cstr();
            const char* value = r.cstr();
            o->RulesResponded(rule, value);
        }
        else if (method == 1)
        {
            o->RulesFailedToRespond();
        }
        else if (method == 2)
        {
            o->RulesRefreshComplete();
        }
        break;
    }
    default:
        break;
    }
}

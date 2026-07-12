// Host reverse-callback channel implementation. Provides host-side proxy objects for the game-implemented
// matchmaking response interfaces: Steam calls these, and each call is serialized into a queue that
// run_callbacks ships back to the guest, which replays it on the real game object.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "steam_api.h" // ISteamMatchmaking*Response, gameserveritem_t, HServerListRequest
#include "steam_reverse.hpp"

namespace sogen::steam_host
{
    namespace
    {
        std::mutex g_mutex;
        // Queued reverse calls: each record is uint64 token, int32 method, uint32 bytes, then the args.
        std::vector<unsigned char> g_queue;
        std::unordered_map<uint64_t, void*> g_proxies;       // token -> host proxy object (one per token)
        std::unordered_map<uint64_t, uint32_t> g_proxy_refs; // token -> count of Steam requests still using it
        std::unordered_map<uint64_t, int32_t> g_proxy_types; // token -> response interface type it was created as
        std::unordered_set<uint64_t> g_issued_handles;       // opaque void* handles the host actually returned

        // Serializes one reverse call's argument buffer (built by each proxy method).
        struct args
        {
            std::vector<unsigned char> b;
            void put(const void* p, size_t n)
            {
                const auto* c = static_cast<const unsigned char*>(p);
                b.insert(b.end(), c, c + n);
            }
            void u64(uint64_t v)
            {
                put(&v, 8);
            }
            void i32(int32_t v)
            {
                put(&v, 4);
            }
            void f32(float v)
            {
                put(&v, 4);
            }
            void cstr(const char* s)
            {
                if (!s)
                {
                    s = "";
                }
                put(s, std::strlen(s) + 1);
            }
        };

        void enqueue(uint64_t token, int32_t method, const args& a)
        {
            std::lock_guard lock{g_mutex};
            const auto bytes = static_cast<uint32_t>(a.b.size());
            const size_t off = g_queue.size();
            g_queue.resize(off + 16 + bytes);
            std::memcpy(g_queue.data() + off, &token, 8);
            std::memcpy(g_queue.data() + off + 8, &method, 4);
            std::memcpy(g_queue.data() + off + 12, &bytes, 4);
            if (bytes)
            {
                std::memcpy(g_queue.data() + off + 16, a.b.data(), bytes);
            }
        }

        // A terminal reverse callback fired (RefreshComplete / *FailedToRespond): drop one active-request ref.
        // Steam issues exactly one terminal callback per request, so this balances create_response_proxy's
        // increments; when the last request clears, the proxy is retired. Returns true if the caller now owns
        // `self` and must delete it (done outside the lock).
        [[nodiscard]] bool retire_proxy(uint64_t token, void* self)
        {
            std::lock_guard lock{g_mutex};
            const auto it = g_proxies.find(token);
            if (it == g_proxies.end() || it->second != self)
            {
                return false;
            }
            if (const auto rc = g_proxy_refs.find(token); rc != g_proxy_refs.end() && rc->second > 1)
            {
                --rc->second;
                return false;
            }
            g_proxy_refs.erase(token);
            g_proxy_types.erase(token);
            g_proxies.erase(it);
            return true;
        }

        struct ServerListProxy : ISteamMatchmakingServerListResponse
        {
            uint64_t token;
            void ServerResponded(HServerListRequest hReq, int iServer) override
            {
                args a;
                a.u64(reinterpret_cast<uint64_t>(hReq));
                a.i32(iServer);
                enqueue(token, 0, a);
            }
            void ServerFailedToRespond(HServerListRequest hReq, int iServer) override
            {
                args a;
                a.u64(reinterpret_cast<uint64_t>(hReq));
                a.i32(iServer);
                enqueue(token, 1, a);
            }
            void RefreshComplete(HServerListRequest hReq, EMatchMakingServerResponse response) override
            {
                args a;
                a.u64(reinterpret_cast<uint64_t>(hReq));
                a.i32(static_cast<int32_t>(response));
                enqueue(token, 2, a);
                if (retire_proxy(token, this))
                {
                    delete this;
                }
            }
        };

        struct PingProxy : ISteamMatchmakingPingResponse
        {
            uint64_t token;
            void ServerResponded(gameserveritem_t& server) override
            {
                args a;
                a.put(&server, sizeof(server));
                enqueue(token, 0, a);
                if (retire_proxy(token, this))
                {
                    delete this;
                }
            }
            void ServerFailedToRespond() override
            {
                args a;
                enqueue(token, 1, a);
                if (retire_proxy(token, this))
                {
                    delete this;
                }
            }
        };

        struct PlayersProxy : ISteamMatchmakingPlayersResponse
        {
            uint64_t token;
            void AddPlayerToList(const char* pchName, int nScore, float flTimePlayed) override
            {
                args a;
                a.cstr(pchName);
                a.i32(nScore);
                a.f32(flTimePlayed);
                enqueue(token, 0, a);
            }
            void PlayersFailedToRespond() override
            {
                args a;
                enqueue(token, 1, a);
                if (retire_proxy(token, this))
                {
                    delete this;
                }
            }
            void PlayersRefreshComplete() override
            {
                args a;
                enqueue(token, 2, a);
                if (retire_proxy(token, this))
                {
                    delete this;
                }
            }
        };

        struct RulesProxy : ISteamMatchmakingRulesResponse
        {
            uint64_t token;
            void RulesResponded(const char* pchRule, const char* pchValue) override
            {
                args a;
                a.cstr(pchRule);
                a.cstr(pchValue);
                enqueue(token, 0, a);
            }
            void RulesFailedToRespond() override
            {
                args a;
                enqueue(token, 1, a);
                if (retire_proxy(token, this))
                {
                    delete this;
                }
            }
            void RulesRefreshComplete() override
            {
                args a;
                enqueue(token, 2, a);
                if (retire_proxy(token, this))
                {
                    delete this;
                }
            }
        };
    }

    // `type` is authoritative -- the generated thunk passes the response interface the method actually takes
    // (a compile-time constant), never a value from the guest. That is what prevents a guest from asking for,
    // say, a Ping proxy where Steam expects a ServerList proxy and then virtual-dispatching through a vtable
    // slot the proxy does not implement (an out-of-bounds indirect call in the host).
    void* create_response_proxy(int32_t type, uint64_t token)
    {
        std::lock_guard lock{g_mutex};
        if (const auto it = g_proxies.find(token); it != g_proxies.end())
        {
            // A token may only be reused for the SAME interface type. Reusing it as a different type would
            // hand Steam a proxy of the wrong shape -> reject rather than type-confuse the existing object.
            const auto t = g_proxy_types.find(token);
            if (t == g_proxy_types.end() || t->second != type)
            {
                return nullptr;
            }
            ++g_proxy_refs[token]; // another concurrent request reuses this response object
            return it->second;
        }
        void* p = nullptr;
        switch (type)
        {
        case 0: {
            auto* o = new ServerListProxy();
            o->token = token;
            p = o;
            break;
        }
        case 1: {
            auto* o = new PingProxy();
            o->token = token;
            p = o;
            break;
        }
        case 2: {
            auto* o = new PlayersProxy();
            o->token = token;
            p = o;
            break;
        }
        case 3: {
            auto* o = new RulesProxy();
            o->token = token;
            p = o;
            break;
        }
        default:
            return nullptr;
        }
        g_proxies[token] = p;
        g_proxy_refs[token] = 1;
        g_proxy_types[token] = type;
        return p;
    }

    void register_opaque_handle(uint64_t handle)
    {
        if (handle == 0)
        {
            return;
        }
        std::lock_guard lock{g_mutex};
        g_issued_handles.insert(handle);
    }

    bool is_valid_opaque_handle(uint64_t handle)
    {
        if (handle == 0)
        {
            return true; // null is always safe; the real Steam method handles it
        }
        std::lock_guard lock{g_mutex};
        return g_issued_handles.count(handle) != 0;
    }

    uint32_t drain_reverse_callbacks(std::vector<unsigned char>& out)
    {
        std::lock_guard lock{g_mutex};
        out.insert(out.end(), g_queue.begin(), g_queue.end()); // records are already reverse_record + args
        uint32_t count = 0;
        size_t off = 0;

        while (off + 16 <= g_queue.size())
        {
            uint32_t bytes = 0;
            std::memcpy(&bytes, g_queue.data() + off + 12, 4);
            off += 16 + bytes;
            ++count;
        }
        g_queue.clear();
        return count;
    }
}

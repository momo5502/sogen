// Host backend: connects the emulator to the real host Steam client and runs the generated marshalling
// thunks. Compiled in its own translation unit so it can include the platform + Steamworks SDK headers
// without clashing with the emulator's hand-rolled Windows types. The host client is loaded dynamically
// (steamclient64.dll on Windows, steamclient.so/.dylib on Linux/macOS), so the backend is cross-platform.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "steam_host_runtime.hpp"
#include "steam_host_backend.h"

// Each SDK version tag is generated + compiled in its own TU and exposes sogen_steam_dispatch_<tag>.
#include "steam_tags.generated.hxx"
#define SOGEN_STEAM_DECL_DISPATCH(tag)                                                                                                     \
    extern "C" int sogen_steam_dispatch_##tag(const char* version, void* iface, uint32_t method, const unsigned char* in, uint32_t in_len, \
                                              unsigned char* out, uint32_t out_cap, uint32_t* out_len, uint64_t* ret);                     \
    extern "C" uint32_t sogen_steam_method_count_##tag(const char* version);
SOGEN_STEAM_TAGS(SOGEN_STEAM_DECL_DISPATCH)
#undef SOGEN_STEAM_DECL_DISPATCH

namespace
{
    namespace sh = sogen::steam_host;

    std::mutex g_mutex;
    bool g_init_attempted = false;
    bool g_connected = false;

    void* g_client = nullptr;
    int32_t g_pipe = 0;
    int32_t g_user = 0;
    void* (*g_get_generic)(void*, int32_t, int32_t, const char*) = nullptr;
    void* (*g_create_interface)(const char*, int*) = nullptr; // steamclient's CreateInterface (version-exact roots)

    // Low-level callback pump exported by steamclient64.dll (works on our own pipe; no SteamAPI_Init).
    // CallbackMsg_t is the SDK's type (m_hSteamUser, m_iCallback, m_pubParam, m_cubParam).
    bool (*g_bgetcallback)(int32_t, CallbackMsg_t*) = nullptr;
    void (*g_freelastcallback)(int32_t) = nullptr;
    bool (*g_getapicallresult)(int32_t, uint64_t, void*, int32_t, int32_t, bool*) = nullptr;

    struct entry
    {
        std::string version;
        void* iface;
    };
    std::unordered_map<uint64_t, entry> g_handles;
    uint64_t g_next_handle = 1;

    // Platform layer: load the host's real Steam client library and resolve its C exports. The generated
    // thunks call the interface methods directly, so the compiler emits each host's native C++ ABI.
#ifdef _WIN32
    using lib_handle = HMODULE;
    lib_handle load_client(const std::string& path)
    {
        if (const auto slash = path.find_last_of("\\/"); slash != std::string::npos)
        {
            SetDllDirectoryA(path.substr(0, slash).c_str()); // let steamclient's own deps resolve
        }
        constexpr uint32_t load_with_altered_search_path = 0x00000008;
        return LoadLibraryExA(path.c_str(), nullptr, load_with_altered_search_path);
    }
    void* client_symbol(lib_handle module, const char* name)
    {
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    }
    void set_env(const char* name, const char* value)
    {
        const std::string kv = std::string(name) + "=" + value;
        _putenv(kv.c_str()); // _putenv copies the string into the environment
    }
    std::string default_client_path()
    {
        return R"(C:\Program Files (x86)\Steam\steamclient64.dll)";
    }
#else
    using lib_handle = void*;
    lib_handle load_client(const std::string& path)
    {
        return dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL); // GLOBAL so steamclient's sibling libs resolve
    }
    void* client_symbol(lib_handle module, const char* name)
    {
        return dlsym(module, name);
    }
    void set_env(const char* name, const char* value)
    {
        ::setenv(name, value, 1);
    }
    std::string default_client_path()
    {
        const char* home = std::getenv("HOME");
        const std::string base = home ? home : ".";
#ifdef __APPLE__
        return base + "/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib";
#else
        return base + "/.steam/steam/linux64/steamclient.so";
#endif
    }
#endif

    bool connect_locked()
    {
        if (g_init_attempted)
        {
            return g_connected;
        }
        g_init_attempted = true;

        if (const char* app_id = std::getenv("SteamAppId"))
        {
            std::fprintf(stderr, "[steam] host session bound to app %s\n", app_id);
        }
        else
        {
            // Steam scopes lobbies, rich presence and matchmaking to the running app. Without a real app id
            // the host session lands in Spacewar (480), which can create/enter its own lobbies but fails to
            // join another app's lobbies with k_EChatRoomEnterResponseDoesntExist -- silently breaking real
            // multiplayer. Fall back so the SDK test harness still works, but say so loudly.
            set_env("SteamAppId", "480");
            std::fprintf(stderr, "[steam] SteamAppId not set; host session falls back to 480 (Spacewar). "
                                 "Set SteamAppId to the game's app id or real multiplayer lobbies will fail.\n");
        }
        std::fflush(stderr);

        const char* env = std::getenv("SOGEN_STEAMCLIENT");
        const std::string dll = env ? env : default_client_path();
        lib_handle module = load_client(dll);
        if (!module)
        {
            return false;
        }

        using create_interface_fn = void* (*)(const char*, int*);
        auto create_interface = reinterpret_cast<create_interface_fn>(client_symbol(module, "CreateInterface"));
        if (!create_interface)
        {
            return false;
        }
        g_create_interface = create_interface;
        for (const char* v : {"SteamClient021", "SteamClient020", "SteamClient019", "SteamClient018"})
        {
            int err = 0;
            g_client = create_interface(v, &err);
            if (g_client)
            {
                break;
            }
        }
        if (!g_client)
        {
            return false;
        }

        void** vt = *static_cast<void***>(g_client);
        g_pipe = reinterpret_cast<int32_t (*)(void*)>(vt[0])(g_client);                  // CreateSteamPipe
        g_user = reinterpret_cast<int32_t (*)(void*, int32_t)>(vt[2])(g_client, g_pipe); // ConnectToGlobalUser
        g_get_generic = reinterpret_cast<void* (*)(void*, int32_t, int32_t, const char*)>(vt[12]);

        g_bgetcallback = reinterpret_cast<bool (*)(int32_t, CallbackMsg_t*)>(client_symbol(module, "Steam_BGetCallback"));
        g_freelastcallback = reinterpret_cast<void (*)(int32_t)>(client_symbol(module, "Steam_FreeLastCallback"));
        g_getapicallresult =
            reinterpret_cast<bool (*)(int32_t, uint64_t, void*, int32_t, int32_t, bool*)>(client_symbol(module, "Steam_GetAPICallResult"));

        g_connected = (g_pipe != 0 && g_user != 0 && g_get_generic != nullptr);
        return g_connected;
    }
}

// Called from the generated thunks (holding g_mutex via invoke) to register a sub-interface returned by a
// GetISteamXxx method. Dedupes by pointer so repeated getters reuse one handle.
namespace sogen::steam_host
{
    uint64_t register_returned_interface(const char* version, void* iface)
    {
        if (!iface || !version)
        {
            if (std::getenv("SOGEN_STEAM_TRACE"))
            {
                std::fprintf(stderr, "[steam-returned-iface] version=%s -> NULL from host\n", version ? version : "(null)");
                std::fflush(stderr);
            }
            return 0;
        }
        for (const auto& [h, e] : g_handles)
        {
            if (e.iface == iface)
            {
                return h;
            }
        }
        const uint64_t handle = g_next_handle++;
        g_handles[handle] = {.version = std::string(version), .iface = iface};
        if (std::getenv("SOGEN_STEAM_TRACE"))
        {
            std::fprintf(stderr, "[steam-returned-iface] version=%s -> handle=%llu\n", version, static_cast<unsigned long long>(handle));
            std::fflush(stderr);
        }
        return handle;
    }

    void* host_resolve_fallback(const char* version)
    {
        if (!version || !g_get_generic || !g_client)
        {
            return nullptr;
        }
        // Split into the family prefix and its trailing numeric suffix (e.g. "...VERSION006" -> +"006").
        const size_t len = std::strlen(version);
        size_t p = len;
        while (p > 0 && version[p - 1] >= '0' && version[p - 1] <= '9')
        {
            --p;
        }
        if (p == len)
        {
            return nullptr; // no numeric suffix to bump
        }
        const std::string prefix(version, p);
        const int width = static_cast<int>(len - p);
        const int base = std::atoi(version + p);
        // Ask for progressively newer versions of the family; the modern client vends one of these.
        for (int cand = base + 1; cand <= base + 40; ++cand)
        {
            std::array<char, 96> buf{};
            std::snprintf(buf.data(), buf.size(), "%s%0*d", prefix.c_str(), width, cand);
            if (void* iface = g_get_generic(g_client, g_user, g_pipe, buf.data()))
            {
                if (std::getenv("SOGEN_STEAM_TRACE"))
                {
                    std::fprintf(stderr, "[steam-fallback] %s -> %s (host has no exact version)\n", version, buf);
                    std::fflush(stderr);
                }
                return iface;
            }
        }
        return nullptr;
    }
}

extern "C" int sogen_steam_backend_init(void)
{
    std::lock_guard lock{g_mutex};
    return connect_locked() ? 1 : 0;
}

extern "C" uint64_t sogen_steam_backend_create_interface(const char* version)
{
    if (!version)
    {
        return 0;
    }
    // Bound the untrusted version string before using it.
    char safe[64];
    size_t n = 0;
    for (; n + 1 < sizeof(safe) && version[n]; ++n)
    {
        safe[n] = version[n];
    }
    safe[n] = '\0';
    if (version[n] != '\0')
    {
        return 0; // over-long / unterminated: reject
    }

    std::lock_guard lock{g_mutex};
    if (!connect_locked())
    {
        return 0;
    }
    // "SteamClient0xx" is the root. Fetch the VERSION-EXACT object via steamclient's CreateInterface so its
    // vtable matches the version the guest was built against -- some root methods changed ABI across versions
    // (e.g. SetLocalIPBinding: uint32 IP -> const SteamIPAddress_t&), so dispatching an old-version call on the
    // modern g_client faults. Only fall back to the modern root if this exact version isn't vended.
    void* iface = nullptr;
    if (std::strncmp(safe, "SteamClient", 11) == 0)
    {
        int err = 0;
        iface = g_create_interface ? g_create_interface(safe, &err) : nullptr;
        if (!iface)
        {
            iface = g_client;
        }
        if (std::getenv("SOGEN_STEAM_TRACE"))
        {
            std::fprintf(stderr, "[steam-root] %s -> %s\n", safe, iface == g_client ? "modern g_client (no exact)" : "version-exact");
            std::fflush(stderr);
        }
    }
    else
    {
        iface = g_get_generic(g_client, g_user, g_pipe, safe);
    }
    if (!iface)
    {
        return 0;
    }
    return sogen::steam_host::register_returned_interface(safe, iface);
}

extern "C" void sogen_steam_backend_release(uint64_t handle)
{
    std::lock_guard lock{g_mutex};
    g_handles.erase(handle);
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
    const auto it = g_handles.find(handle);
    if (it == g_handles.end())
    {
        return sh::steam_host_unknown_interface;
    }

    const char* version = it->second.version.c_str();
    void* iface = it->second.iface;
    const unsigned char* inb = in ? in : reinterpret_cast<const unsigned char*>("");
    const uint32_t inlen = in ? in_len : 0;

    if (std::getenv("SOGEN_STEAM_TRACE"))
    {
        std::fprintf(stderr, "[steam-invoke] version=%s method=%u handle=%llu\n", version, method, static_cast<unsigned long long>(handle));
        std::fflush(stderr);
    }

    // Each version tag's isolated TU exposes sogen_steam_dispatch_<tag>. A version string is not unique across
    // snapshots (Valve appended methods without bumping it), so pick the snapshot with the MOST methods -- the
    // same rule the guest shim uses to build the proxy, so the method index we receive means the same thing here.
    using dispatch_fn =
        int (*)(const char*, void*, uint32_t, const unsigned char*, uint32_t, unsigned char*, uint32_t, uint32_t*, uint64_t*);
    dispatch_fn dispatch = nullptr;
    uint32_t best = 0;
#define SOGEN_STEAM_PICK_DISPATCH(tag)                                                    \
    if (const uint32_t methods = sogen_steam_method_count_##tag(version); methods > best) \
    {                                                                                     \
        best = methods;                                                                   \
        dispatch = &sogen_steam_dispatch_##tag;                                           \
    }
    SOGEN_STEAM_TAGS(SOGEN_STEAM_PICK_DISPATCH)
#undef SOGEN_STEAM_PICK_DISPATCH

    const int status =
        dispatch ? dispatch(version, iface, method, inb, inlen, out, out_cap, out_len, ret) : sh::steam_host_unknown_interface;
    if (std::getenv("SOGEN_STEAM_TRACE"))
    {
        std::fprintf(stderr, "[steam-invoke] version=%s method=%u -> status=%d ret=%llu\n", version, method, status,
                     static_cast<unsigned long long>(ret ? *ret : 0));
        std::fflush(stderr);
    }
    return status;
}

extern "C" int sogen_steam_backend_run_callbacks(int32_t pipe, uint8_t* out, uint32_t out_cap, uint32_t* normal_len, uint32_t* normal_count,
                                                 uint32_t* reverse_len, uint32_t* reverse_count)
{
    if (normal_len)
    {
        *normal_len = 0;
    }
    if (normal_count)
    {
        *normal_count = 0;
    }
    if (reverse_len)
    {
        *reverse_len = 0;
    }
    if (reverse_count)
    {
        *reverse_count = 0;
    }

    std::lock_guard lock{g_mutex};
    if (!g_connected || !g_bgetcallback || !g_freelastcallback)
    {
        return sh::steam_host_unsupported;
    }
    const int32_t use_pipe = (pipe != 0) ? pipe : g_pipe; // fall back to our own pipe if none supplied

    constexpr uint32_t max_records = 256;
    constexpr uint32_t max_param = 8u * 1024u;
    uint32_t written = 0;
    uint32_t n_records = 0;

    const bool trace = std::getenv("SOGEN_STEAM_TRACE") != nullptr;
    std::string traced_ids;

    // Our host Steam session was already connected before the guest attached, so Steam never re-fires the
    // "you're online now" callback to the guest's pipe. Retail MW2 waits for SteamServersConnected_t
    // (k_iSteamUserCallbacks + 1 = 101) before starting its IWNet connection. Synthesize it once per pipe so
    // the guest sees it. Record layout = [int32 callback id][uint32 payload bytes = 0].
    static std::unordered_set<int32_t> announced_connected;
    if (out && announced_connected.insert(use_pipe).second && written + 8u <= out_cap)
    {
        const int32_t servers_connected_id = 101;
        const uint32_t zero_bytes = 0;
        std::memcpy(out + written, &servers_connected_id, 4);
        std::memcpy(out + written + 4, &zero_bytes, 4);
        written += 8u;
        ++n_records;
        if (trace)
        {
            traced_ids += " 101(synthetic)";
        }
    }

    CallbackMsg_t msg{};
    while (n_records < max_records && g_bgetcallback(use_pipe, &msg))
    {
        uint32_t n = (msg.m_cubParam > 0) ? static_cast<uint32_t>(msg.m_cubParam) : 0;
        n = (std::min)(n, max_param);
        if (trace)
        {
            traced_ids += ' ';
            traced_ids += std::to_string(msg.m_iCallback);
        }
        // Record = int32 callback_id, uint32 data_bytes, then the payload. Only recorded if it fits;
        // the callback is freed either way so the client's queue never stalls.
        if (out && written + 8u + n <= out_cap)
        {
            std::memcpy(out + written, &msg.m_iCallback, 4);
            std::memcpy(out + written + 4, &n, 4);
            if (n && msg.m_pubParam)
            {
                std::memcpy(out + written + 8, msg.m_pubParam, n);
            }
            written += 8u + n;
            ++n_records;
        }
        g_freelastcallback(use_pipe);
    }

    if (normal_len)
    {
        *normal_len = written;
    }
    if (normal_count)
    {
        *normal_count = n_records;
    }

    // Steam calls our host response proxies during the pump above; drain those reverse calls now, appending
    // them after the normal records.
    std::vector<unsigned char> reverse;
    const uint32_t rcount = sh::drain_reverse_callbacks(reverse);
    if (out && !reverse.empty() && written + reverse.size() <= out_cap)
    {
        std::memcpy(out + written, reverse.data(), reverse.size());
        if (reverse_len)
        {
            *reverse_len = static_cast<uint32_t>(reverse.size());
        }
        if (reverse_count)
        {
            *reverse_count = rcount;
        }
    }
    if ((n_records || rcount) && trace)
    {
        std::fprintf(stderr, "[steam-callbacks] normal=%u reverse=%u ids:%s\n", n_records, rcount, traced_ids.c_str());
        std::fflush(stderr);
    }
    return sh::steam_host_ok;
}

extern "C" int sogen_steam_backend_get_api_call_result(int32_t pipe, uint64_t call, int32_t callback_id, uint32_t data_bytes, uint8_t* out,
                                                       uint32_t out_cap, uint32_t* out_len, uint8_t* io_failure)
{
    if (out_len)
    {
        *out_len = 0;
    }
    if (io_failure)
    {
        *io_failure = 0;
    }

    std::lock_guard lock{g_mutex};
    if (!g_connected || !g_getapicallresult)
    {
        return sh::steam_host_unsupported;
    }
    const int32_t use_pipe = (pipe != 0) ? pipe : g_pipe;

    uint32_t n = (data_bytes < out_cap) ? data_bytes : out_cap;
    bool failed = false;
    const bool ok = g_getapicallresult(use_pipe, call, out, static_cast<int32_t>(n), callback_id, &failed);
    if (std::getenv("SOGEN_STEAM_TRACE"))
    {
        std::fprintf(stderr, "[steam-callresult] call=%llu callback_id=%d -> ok=%d failed=%d\n", static_cast<unsigned long long>(call),
                     callback_id, ok ? 1 : 0, failed ? 1 : 0);
        std::fflush(stderr);
    }
    if (io_failure)
    {
        *io_failure = failed ? 1 : 0;
    }
    if (ok && out_len)
    {
        *out_len = n;
    }
    return ok ? sh::steam_host_ok : sh::steam_host_unsupported;
}

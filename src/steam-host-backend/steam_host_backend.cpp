// Host backend: connects the emulator to the real host Steam client (steamclient64.dll) and runs the
// generated marshalling thunks. Compiled in its own translation unit so it can include <windows.h> and
// the Steamworks SDK headers without clashing with the emulator's hand-rolled Windows types.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include "steam_host_runtime.hpp"
#include "steam_host_backend.h"

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

    constexpr unsigned long load_with_altered_search_path = 0x00000008;

    bool connect_locked()
    {
        if (g_init_attempted)
        {
            return g_connected;
        }
        g_init_attempted = true;

        if (!std::getenv("SteamAppId"))
        {
            _putenv("SteamAppId=480"); // Spacewar (free SDK test app) gives a valid app context
        }

        const char* env = std::getenv("SOGEN_STEAMCLIENT");
        const std::string dll = env ? env : R"(C:\Program Files (x86)\Steam\steamclient64.dll)";
        const auto slash = dll.find_last_of("\\/");
        if (slash != std::string::npos)
        {
            SetDllDirectoryA(dll.substr(0, slash).c_str()); // let steamclient's own deps resolve
        }

        HMODULE module = LoadLibraryExA(dll.c_str(), nullptr, load_with_altered_search_path);
        if (!module)
        {
            return false;
        }

        using create_interface_fn = void* (*)(const char*, int*);
        auto create_interface = reinterpret_cast<create_interface_fn>(GetProcAddress(module, "CreateInterface"));
        if (!create_interface)
        {
            return false;
        }
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
        g_pipe = reinterpret_cast<int32_t (*)(void*)>(vt[0])(g_client);                       // CreateSteamPipe
        g_user = reinterpret_cast<int32_t (*)(void*, int32_t)>(vt[2])(g_client, g_pipe);       // ConnectToGlobalUser
        g_get_generic = reinterpret_cast<void* (*)(void*, int32_t, int32_t, const char*)>(vt[12]);

        g_bgetcallback = reinterpret_cast<bool (*)(int32_t, CallbackMsg_t*)>(GetProcAddress(module, "Steam_BGetCallback"));
        g_freelastcallback = reinterpret_cast<void (*)(int32_t)>(GetProcAddress(module, "Steam_FreeLastCallback"));
        g_getapicallresult = reinterpret_cast<bool (*)(int32_t, uint64_t, void*, int32_t, int32_t, bool*)>(
            GetProcAddress(module, "Steam_GetAPICallResult"));

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
        g_handles[handle] = {std::string(version), iface};
        return handle;
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
    // "SteamClient0xx" is the root, obtained via steamclient's own CreateInterface (our g_client). Every
    // other interface is reached through it (GetISteamGenericInterface etc.), which the proxy forwards.
    void* iface = (std::strncmp(safe, "SteamClient", 11) == 0) ? g_client : g_get_generic(g_client, g_user, g_pipe, safe);
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

extern "C" int sogen_steam_backend_invoke(uint64_t handle, uint32_t method, const uint8_t* in, uint32_t in_len,
                                          uint8_t* out, uint32_t out_cap, uint32_t* out_len, uint64_t* ret)
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

    const unsigned char* begin = in ? in : reinterpret_cast<const unsigned char*>("");
    sh::steam_host_reader reader{begin, begin + (in ? in_len : 0)};
    std::vector<unsigned char> reply;
    uint64_t local_ret = 0;
    sh::steam_host_writer writer{reply, local_ret};

    const int status = sh::dispatch(it->second.version.c_str(), it->second.iface, method, reader, writer);

    if (ret)
    {
        *ret = local_ret;
    }
    if (out && out_cap && !reply.empty())
    {
        const uint32_t n = reply.size() < out_cap ? static_cast<uint32_t>(reply.size()) : out_cap;
        std::memcpy(out, reply.data(), n);
        if (out_len)
        {
            *out_len = n;
        }
    }
    return status;
}

extern "C" int sogen_steam_backend_run_callbacks(int32_t pipe, uint8_t* out, uint32_t out_cap, uint32_t* normal_len,
                                                 uint32_t* normal_count, uint32_t* reverse_len, uint32_t* reverse_count)
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

    CallbackMsg_t msg{};
    while (n_records < max_records && g_bgetcallback(use_pipe, &msg))
    {
        uint32_t n = (msg.m_cubParam > 0) ? static_cast<uint32_t>(msg.m_cubParam) : 0;
        if (n > max_param)
        {
            n = max_param;
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
    return sh::steam_host_ok;
}

extern "C" int sogen_steam_backend_get_api_call_result(int32_t pipe, uint64_t call, int32_t callback_id,
                                                       uint32_t data_bytes, uint8_t* out, uint32_t out_cap,
                                                       uint32_t* out_len, uint8_t* io_failure)
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

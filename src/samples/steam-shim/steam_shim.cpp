// Guest-side Steam shim: a drop-in steamclient64.dll / steamclient.dll. The game's OWN (unmodified)
// steam_api DLL loads this exactly as it would Valve's steamclient, so we never touch game files and stay
// independent of the game's steam_api version. We implement the steamclient boundary that steam_api uses:
//   - CreateInterface(version)  -> an ISteamClient proxy (the root; steam_api navigates from it)
//   - Steam_BGetCallback / Steam_FreeLastCallback / Steam_GetAPICallResult  -> the callback pump
// The game's steam_api keeps doing its own callback registration and CCallback dispatch; we just feed it
// the callbacks the host client produces.
//
// This TU deliberately does NOT include the SDK headers (they declare these entry points and would clash);
// the generated proxies live in steam_shim_interfaces.cpp, reached here via sogen_make_proxy.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <steam_bridge_protocol.hpp>

namespace sb = sogen::steam_bridge;

// Defined in steam_shim_interfaces.cpp (the SDK-including TU): construct the proxy for a version string,
// and replay a host reverse-call onto the registered game response object.
extern "C" void* sogen_make_proxy(const char* version, uint64_t handle);
extern "C" void sogen_dispatch_reverse(uint64_t token, int32_t method, const void* data, uint32_t bytes);

// The generated proxies call these (declared in steam_shim_runtime.hpp); defined here without the SDK
// headers -- the signatures use only primitive types, so the definitions match across the two TUs.
namespace sogen::steam_shim
{
    void bridge_invoke(uint64_t handle, uint32_t method, const void* in, uint32_t in_len, void* out, uint32_t out_cap,
                       uint32_t* out_len, uint64_t* ret);
    void report_unsupported(const char* /*iface*/, const char* /*method*/)
    {
    }
}

namespace
{
    HANDLE g_bridge = INVALID_HANDLE_VALUE;
    std::mutex g_mutex;
    std::unordered_map<std::string, void*> g_interfaces; // version -> proxy (Steam interfaces are singletons)

    HANDLE bridge()
    {
        if (g_bridge == INVALID_HANDLE_VALUE)
        {
            g_bridge = CreateFileA(R"(\\.\SogenSteam)", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        return g_bridge;
    }

    bool ioctl(const uint32_t code, const void* in, const uint32_t in_len, void* out, const uint32_t out_len, uint32_t& returned)
    {
        const HANDLE h = bridge();
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD got = 0;
        const BOOL ok = DeviceIoControl(h, code, const_cast<void*>(in), in_len, out, out_len, &got, nullptr);
        returned = static_cast<uint32_t>(got);
        return ok != FALSE;
    }

    // Confirms the bridge speaks our protocol; done once, lazily, before the first interface is created.
    bool handshake()
    {
        static bool ok = false;
        static bool done = false;
        if (done)
        {
            return ok;
        }
        done = true;
        uint32_t returned = 0;
        sb::version_response version{};
        ok = ioctl(sb::ioctl_get_version, nullptr, 0, &version, sizeof(version), returned) &&
             returned >= sizeof(version) && version.magic == sb::protocol_magic && version.version == sb::protocol_version;
        return ok;
    }
}

// Transport used by every generated proxy: pack (handle, method, in-blob) into an invoke IOCTL and
// scatter the reply back (raw return in *ret, out-parameter payload in out/out_len).
void sogen::steam_shim::bridge_invoke(const uint64_t handle, const uint32_t method, const void* in, const uint32_t in_len,
                                      void* out, const uint32_t out_cap, uint32_t* out_len, uint64_t* ret)
{
    *out_len = 0;
    *ret = 0;

    std::vector<unsigned char> input(sizeof(sb::invoke_header) + in_len);
    const sb::invoke_header header{.handle = handle, .method_index = method, .arg_bytes = in_len};
    std::memcpy(input.data(), &header, sizeof(header));
    if (in_len)
    {
        std::memcpy(input.data() + sizeof(header), in, in_len);
    }

    std::vector<unsigned char> output(sizeof(sb::invoke_response) + out_cap);
    uint32_t returned = 0;
    if (!ioctl(sb::ioctl_invoke_method, input.data(), static_cast<uint32_t>(input.size()), output.data(),
               static_cast<uint32_t>(output.size()), returned) ||
        returned < sizeof(sb::invoke_response))
    {
        return;
    }

    sb::invoke_response response{};
    std::memcpy(&response, output.data(), sizeof(response));
    *ret = response.return_value;

    uint32_t n = response.out_bytes;
    if (n > out_cap)
    {
        n = out_cap;
    }
    if (out && n)
    {
        std::memcpy(out, output.data() + sizeof(sb::invoke_response), n);
    }
    *out_len = n;
}

// --- callback pump (steamclient's Steam_* entry points) --------------------------------------------------
// The host drains all pending callbacks in one batch; we hand them to the caller one at a time, exactly as
// steamclient's Steam_BGetCallback / Steam_FreeLastCallback contract expects.
namespace
{
    struct CallbackMsg_t
    {
        int32_t m_hSteamUser;
        int32_t m_iCallback;
        uint8_t* m_pubParam;
        int32_t m_cubParam;
    };

    struct callback_queue
    {
        std::vector<unsigned char> blob; // whole run_callbacks reply
        uint32_t off = 0;                // read cursor within blob
        uint32_t end = 0;                // valid extent
        std::vector<unsigned char> current; // keeps the payload alive until the next BGetCallback
    };
    thread_local callback_queue g_cbq;

    bool drain_batch(int32_t pipe)
    {
        const sb::run_callbacks_request request{.pipe = static_cast<uint32_t>(pipe)};
        g_cbq.blob.assign(sizeof(sb::run_callbacks_response) + 64u * 1024u, 0);
        uint32_t returned = 0;
        if (!ioctl(sb::ioctl_run_callbacks, &request, sizeof(request), g_cbq.blob.data(),
                   static_cast<uint32_t>(g_cbq.blob.size()), returned) ||
            returned < sizeof(sb::run_callbacks_response))
        {
            g_cbq.off = g_cbq.end = 0;
            return false;
        }
        sb::run_callbacks_response resp{};
        std::memcpy(&resp, g_cbq.blob.data(), sizeof(resp));
        g_cbq.off = sizeof(sb::run_callbacks_response);
        g_cbq.end = g_cbq.off + resp.blob_bytes;
        if (g_cbq.end > returned)
        {
            g_cbq.end = returned;
        }

        // Reverse-callback records follow the normal ones: replay each on the game's response object now.
        uint32_t roff = sizeof(sb::run_callbacks_response) + resp.blob_bytes;
        uint32_t rend = roff + resp.reverse_bytes;
        if (rend > returned)
        {
            rend = returned;
        }
        while (roff + 16 <= rend)
        {
            uint64_t token = 0;
            int32_t method = 0;
            uint32_t bytes = 0;
            std::memcpy(&token, g_cbq.blob.data() + roff, 8);
            std::memcpy(&method, g_cbq.blob.data() + roff + 8, 4);
            std::memcpy(&bytes, g_cbq.blob.data() + roff + 12, 4);
            roff += 16;
            if (bytes > rend - roff)
            {
                break;
            }
            sogen_dispatch_reverse(token, method, g_cbq.blob.data() + roff, bytes);
            roff += bytes;
        }
        return g_cbq.off < g_cbq.end;
    }
}

extern "C"
{
    // steam_api calls this first to obtain the root ISteamClient (and, historically, other roots). We
    // return a proxy whose vtable matches the requested version.
    void* CreateInterface(const char* version, int* returnCode)
    {
        if (returnCode)
        {
            *returnCode = 0;
        }
        if (!version || !handshake())
        {
            return nullptr;
        }

        std::lock_guard lock{g_mutex};
        if (const auto it = g_interfaces.find(version); it != g_interfaces.end())
        {
            return it->second;
        }

        sb::create_interface_request request{};
        std::strncpy(request.version.data(), version, request.version.size() - 1);
        sb::create_interface_response response{};
        uint32_t returned = 0;
        if (!ioctl(sb::ioctl_create_interface, &request, sizeof(request), &response, sizeof(response), returned) ||
            returned < sizeof(response) || response.handle == sb::null_interface)
        {
            return nullptr;
        }

        void* proxy = sogen_make_proxy(version, response.handle);
        if (proxy)
        {
            g_interfaces.emplace(version, proxy);
        }
        return proxy;
    }

    // Retrieves the next queued callback; false when the queue is drained (steam_api's classic pump loops
    // this then calls Steam_FreeLastCallback).
    bool Steam_BGetCallback(int32_t hSteamPipe, CallbackMsg_t* pCallbackMsg)
    {
        if (!pCallbackMsg)
        {
            return false;
        }
        if (g_cbq.off >= g_cbq.end && !drain_batch(hSteamPipe))
        {
            return false;
        }
        if (g_cbq.off + 8 > g_cbq.end)
        {
            g_cbq.off = g_cbq.end;
            return false;
        }
        int32_t id = 0;
        uint32_t bytes = 0;
        std::memcpy(&id, g_cbq.blob.data() + g_cbq.off, 4);
        std::memcpy(&bytes, g_cbq.blob.data() + g_cbq.off + 4, 4);
        g_cbq.off += 8;
        if (bytes > g_cbq.end - g_cbq.off)
        {
            g_cbq.off = g_cbq.end;
            return false;
        }
        g_cbq.current.assign(g_cbq.blob.begin() + g_cbq.off, g_cbq.blob.begin() + g_cbq.off + bytes);
        g_cbq.off += bytes;

        pCallbackMsg->m_hSteamUser = 1;
        pCallbackMsg->m_iCallback = id;
        pCallbackMsg->m_pubParam = g_cbq.current.data();
        pCallbackMsg->m_cubParam = static_cast<int32_t>(bytes);
        return true;
    }

    void Steam_FreeLastCallback(int32_t /*hSteamPipe*/)
    {
        // The payload is owned by g_cbq.current and released on the next Steam_BGetCallback; nothing to do.
    }

    // Retrieves a completed async-call result (for the game's CCallResult dispatch).
    bool Steam_GetAPICallResult(int32_t hSteamPipe, uint64_t hSteamAPICall, void* pCallback, int cubCallback,
                                int iCallbackExpected, bool* pbFailed)
    {
        const sb::api_call_result_request request{.call = hSteamAPICall,
                                                  .callback_id = iCallbackExpected,
                                                  .data_bytes = static_cast<uint32_t>(cubCallback < 0 ? 0 : cubCallback),
                                                  .pipe = hSteamPipe,
                                                  .reserved = 0};
        std::vector<unsigned char> buf(sizeof(sb::api_call_result_response) + request.data_bytes);
        uint32_t returned = 0;
        if (!ioctl(sb::ioctl_get_api_call_result, &request, sizeof(request), buf.data(), static_cast<uint32_t>(buf.size()),
                   returned) ||
            returned < sizeof(sb::api_call_result_response))
        {
            if (pbFailed)
            {
                *pbFailed = true;
            }
            return false;
        }
        sb::api_call_result_response resp{};
        std::memcpy(&resp, buf.data(), sizeof(resp));
        if (!resp.ok)
        {
            if (pbFailed)
            {
                *pbFailed = true;
            }
            return false;
        }
        uint32_t n = resp.data_bytes;
        if (cubCallback >= 0 && n > static_cast<uint32_t>(cubCallback))
        {
            n = static_cast<uint32_t>(cubCallback);
        }
        if (pCallback && n)
        {
            std::memcpy(pCallback, buf.data() + sizeof(sb::api_call_result_response), n);
        }
        if (pbFailed)
        {
            *pbFailed = resp.io_failure != 0;
        }
        return true;
    }
}

// Drives the guest steamclient shim exactly the way a game's own steam_api DLL would: resolve
// CreateInterface + the Steam_* callback pump by name, get the root ISteamClient, open a pipe/user, fetch
// interfaces via GetISteamGenericInterface, call methods, and pump callbacks + a call-result. Everything
// forwards across the Sogen Steam bridge to the host's real steamclient.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "steam_api.h"

using create_interface_fn = void* (*)(const char*, int*);
using bgetcallback_fn = bool (*)(HSteamPipe, CallbackMsg_t*);
using freelastcallback_fn = void (*)(HSteamPipe);
using getapicallresult_fn = bool (*)(HSteamPipe, SteamAPICall_t, void*, int, int, bool*);

// A game-implemented server-browser response object. Steam calls into it; with our reverse channel those
// calls arrive here from the host.
struct ServerListResponse : ISteamMatchmakingServerListResponse
{
    volatile int responded = 0;
    volatile bool complete = false;
    volatile int response_code = -1;
    void ServerResponded(HServerListRequest, int) override
    {
        ++responded;
    }
    void ServerFailedToRespond(HServerListRequest, int) override
    {
    }
    void RefreshComplete(HServerListRequest, EMatchMakingServerResponse r) override
    {
        complete = true;
        response_code = static_cast<int>(r);
    }
};

int main(int argc, char** argv)
{
    const char* dll = (argc > 1) ? argv[1] : "steamclient64.dll";
    std::printf("[steam-test] loading %s\n", dll);
    const HMODULE mod = LoadLibraryA(dll);
    if (!mod)
    {
        std::printf("[steam-test] LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    const auto CreateInterface = reinterpret_cast<create_interface_fn>(reinterpret_cast<void*>(GetProcAddress(mod, "CreateInterface")));
    const auto BGetCallback = reinterpret_cast<bgetcallback_fn>(reinterpret_cast<void*>(GetProcAddress(mod, "Steam_BGetCallback")));
    const auto FreeLastCallback =
        reinterpret_cast<freelastcallback_fn>(reinterpret_cast<void*>(GetProcAddress(mod, "Steam_FreeLastCallback")));
    const auto GetAPICallResult =
        reinterpret_cast<getapicallresult_fn>(reinterpret_cast<void*>(GetProcAddress(mod, "Steam_GetAPICallResult")));
    if (!CreateInterface || !BGetCallback || !FreeLastCallback || !GetAPICallResult)
    {
        std::printf("[steam-test] missing steamclient exports\n");
        return 2;
    }

    int rc = 0;
    auto* client = static_cast<ISteamClient*>(CreateInterface(STEAMCLIENT_INTERFACE_VERSION, &rc));
    std::printf("[steam-test] ISteamClient=%p\n", static_cast<void*>(client));
    if (!client)
    {
        return 3;
    }

    const HSteamPipe pipe = client->CreateSteamPipe();
    const HSteamUser user = client->ConnectToGlobalUser(pipe);
    std::printf("[steam-test] pipe=%d user=%d\n", pipe, user);

    auto* iuser = static_cast<ISteamUser*>(client->GetISteamGenericInterface(user, pipe, STEAMUSER_INTERFACE_VERSION));
    auto* friends = static_cast<ISteamFriends*>(client->GetISteamGenericInterface(user, pipe, STEAMFRIENDS_INTERFACE_VERSION));
    auto* stats = static_cast<ISteamUserStats*>(client->GetISteamGenericInterface(user, pipe, STEAMUSERSTATS_INTERFACE_VERSION));
    std::printf("[steam-test] user=%p friends=%p stats=%p\n", static_cast<void*>(iuser), static_cast<void*>(friends),
                static_cast<void*>(stats));
    if (!iuser || !friends)
    {
        return 4;
    }

    const CSteamID id = iuser->GetSteamID();
    std::printf("[steam-test] BLoggedOn=%d  SteamID=%llu\n", iuser->BLoggedOn() ? 1 : 0,
                static_cast<unsigned long long>(id.ConvertToUint64()));
    std::printf("[steam-test] PersonaName=%s\n", friends->GetPersonaName());
    std::printf("[steam-test] FriendPersonaName(self)=%s\n", friends->GetFriendPersonaName(id));
    std::printf("[steam-test] FriendCount=%d\n", friends->GetFriendCount(k_EFriendFlagAll));

    char folder[512] = {0};
    std::printf("[steam-test] GetUserDataFolder=%d '%s'\n", iuser->GetUserDataFolder(folder, sizeof(folder)) ? 1 : 0, folder);

    // Pump callbacks the way steam_api does: loop Steam_BGetCallback, and complete our async call via
    // Steam_GetAPICallResult when its SteamAPICallCompleted_t arrives.
    SteamAPICall_t hc = k_uAPICallInvalid;
    if (stats)
    {
        hc = stats->RequestUserStats(id);
        std::printf("[steam-test] RequestUserStats -> call=%llu\n", static_cast<unsigned long long>(hc));
    }
    int plain = 0;
    bool result_done = false;
    for (int i = 0; i < 50 && !result_done; ++i)
    {
        CallbackMsg_t msg{};
        while (BGetCallback(pipe, &msg))
        {
            if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback)
            {
                auto* c = reinterpret_cast<SteamAPICallCompleted_t*>(msg.m_pubParam);
                if (c->m_hAsyncCall == hc && hc != k_uAPICallInvalid)
                {
                    UserStatsReceived_t r{};
                    bool failed = false;
                    if (GetAPICallResult(pipe, c->m_hAsyncCall, &r, sizeof(r), c->m_iCallback, &failed))
                    {
                        std::printf("[steam-test] CALLRESULT UserStatsReceived_t: io_failure=%d result=%d appid=%llu\n",
                                    failed ? 1 : 0, static_cast<int>(r.m_eResult),
                                    static_cast<unsigned long long>(r.m_nGameID));
                        result_done = true;
                    }
                }
            }
            else
            {
                ++plain;
            }
            FreeLastCallback(pipe);
        }
        Sleep(100);
    }
    std::printf("[steam-test] plain callbacks=%d, callresult done=%d\n", plain, result_done ? 1 : 0);

    // Server browser: a REVERSE callback — Steam calls into our game-implemented response object.
    auto* servers = static_cast<ISteamMatchmakingServers*>(
        client->GetISteamGenericInterface(user, pipe, STEAMMATCHMAKINGSERVERS_INTERFACE_VERSION));
    std::printf("[steam-test] ISteamMatchmakingServers=%p\n", static_cast<void*>(servers));
    if (servers)
    {
        ServerListResponse slr;
        const HServerListRequest req = servers->RequestInternetServerList(480, nullptr, 0, &slr);
        std::printf("[steam-test] RequestInternetServerList -> req=%p\n", static_cast<void*>(req));
        for (int i = 0; i < 300 && !slr.complete; ++i) // internet list can take 20-30s
        {
            CallbackMsg_t m{};
            while (BGetCallback(pipe, &m)) // pumping dispatches reverse callbacks into slr
            {
                FreeLastCallback(pipe);
            }
            Sleep(100);
        }
        std::printf("[steam-test] SERVER BROWSER: RefreshComplete=%d response=%d serversResponded=%d count=%d\n",
                    slr.complete ? 1 : 0, slr.response_code, slr.responded, servers->GetServerCount(req));
        servers->ReleaseRequest(req);
    }

    std::printf("[steam-test] done\n");
    return 0;
}

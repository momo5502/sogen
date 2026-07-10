// Isolated old-SDK (Steamworks 1.05) generation of the guest proxies. Compiled in its own object library
// against the 1.05 headers ONLY (a different steam_api.h than the latest TU), so its proxy classes inherit
// the EXACT 1.05 vtable layout the 2009-era game was built against. Each proxy method forwards over the
// single (latest) wire protocol -- see the generated file. The two SDK generations never share a TU because
// their identically-named ISteam* classes would collide.

#include "steam_shim_proxies.generated.hxx" // resolved from src/steam-generated/v105 (SDK-1.05 proxies)

extern "C" void* sogen_make_proxy_v105(const char* version, uint64_t handle)
{
    return sogen::steam_shim::v105::create_proxy(version, handle);
}

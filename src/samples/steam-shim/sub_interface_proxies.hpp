#pragma once

#include <cstdint>
#include <unordered_map>

namespace sogen::steam_shim
{
    // The guest proxies of Steam interfaces that other interfaces return. The host hands out one handle per
    // Steam object, so one proxy per handle keeps pointer comparisons valid. Keying by version instead would
    // keep serving an object that went away with its pipe once the game obtains the same version again.
    class sub_interface_proxies
    {
      public:
        template <typename MakeProxy>
        void* resolve(const uint64_t handle, const MakeProxy& make_proxy)
        {
            if (const auto it = this->proxies_.find(handle); it != this->proxies_.end())
            {
                return it->second;
            }

            void* proxy = make_proxy();
            if (proxy)
            {
                this->proxies_.emplace(handle, proxy);
            }
            return proxy;
        }

      private:
        std::unordered_map<uint64_t, void*> proxies_{};
    };
}

#include "macos_mig.hpp"

namespace sogen
{
    namespace
    {
        constexpr int32_t HOST_INFO_ID = 200;
    }

    mig_routine find_mig_routine(const mach_port_object object, const int32_t id)
    {
        if (object == mach_port_object::host && id == HOST_INFO_ID)
        {
            return mig_host_info;
        }

        return nullptr;
    }
}

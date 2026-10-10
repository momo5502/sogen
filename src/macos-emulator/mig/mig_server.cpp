#include "macos_mig.hpp"

namespace sogen
{
    namespace
    {
        constexpr int32_t HOST_INFO_ID = 200;
        constexpr int32_t HOST_GET_CLOCK_SERVICE_ID = 206;
        constexpr int32_t TASK_GET_SPECIAL_PORT_ID = 3409;
        constexpr int32_t SEMAPHORE_CREATE_ID = 3418;
        constexpr int32_t TASK_RESTARTABLE_RANGES_REGISTER_ID = 8000;
    }

    mig_routine find_mig_routine(const mach_port_object object, const int32_t id)
    {
        if (object == mach_port_object::host)
        {
            switch (id)
            {
            case HOST_INFO_ID:
                return mig_host_info;
            case HOST_GET_CLOCK_SERVICE_ID:
                return mig_host_get_clock_service;
            default:
                break;
            }
        }

        if (object == mach_port_object::task)
        {
            switch (id)
            {
            case TASK_GET_SPECIAL_PORT_ID:
                return mig_task_get_special_port;
            case SEMAPHORE_CREATE_ID:
                return mig_semaphore_create;
            case TASK_RESTARTABLE_RANGES_REGISTER_ID:
                return mig_task_restartable_ranges_register;
            default:
                break;
            }
        }

        return nullptr;
    }
}

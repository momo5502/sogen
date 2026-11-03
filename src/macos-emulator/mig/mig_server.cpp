#include "macos_mig.hpp"

namespace sogen
{
    namespace
    {
        constexpr int32_t HOST_INFO_ID = 200;
        constexpr int32_t HOST_GET_CLOCK_SERVICE_ID = 206;
        constexpr int32_t HOST_GET_SPECIAL_PORT_ID = 412;
        constexpr int32_t MACH_PORTS_REGISTER_ID = 3403;
        constexpr int32_t MACH_PORTS_LOOKUP_ID = 3404;
        constexpr int32_t TASK_INFO_ID = 3405;
        constexpr int32_t TASK_GET_SPECIAL_PORT_ID = 3409;
        constexpr int32_t TASK_SET_SPECIAL_PORT_ID = 3410;
        constexpr int32_t SEMAPHORE_CREATE_ID = 3418;
        constexpr int32_t MACH_VM_MAP_ID = 4811;
        constexpr int32_t TASK_RESTARTABLE_RANGES_REGISTER_ID = 8000;
        constexpr int32_t TASK_RESTARTABLE_RANGES_SYNCHRONIZE_ID = 8001;
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
            case HOST_GET_SPECIAL_PORT_ID:
                return mig_host_get_special_port;
            default:
                break;
            }
        }

        if (object == mach_port_object::task)
        {
            switch (id)
            {
            case MACH_PORTS_REGISTER_ID:
                return mig_mach_ports_register;
            case MACH_PORTS_LOOKUP_ID:
                return mig_mach_ports_lookup;
            case TASK_INFO_ID:
                return mig_task_info;
            case TASK_GET_SPECIAL_PORT_ID:
                return mig_task_get_special_port;
            case TASK_SET_SPECIAL_PORT_ID:
                return mig_task_set_special_port;
            case SEMAPHORE_CREATE_ID:
                return mig_semaphore_create;
            case MACH_VM_MAP_ID:
                return mig_mach_vm_map;
            case TASK_RESTARTABLE_RANGES_REGISTER_ID:
                return mig_task_restartable_ranges_register;
            case TASK_RESTARTABLE_RANGES_SYNCHRONIZE_ID:
                return mig_task_restartable_ranges_synchronize;
            default:
                break;
            }
        }

        return nullptr;
    }
}

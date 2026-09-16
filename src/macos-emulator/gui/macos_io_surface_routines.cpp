#include "../std_include.hpp"
#include "macos_io_surface_routines.hpp"

#include "macos_cf_bridge.hpp"
#include "../macos_emulator.hpp"

#include <set>

namespace sogen
{
    namespace
    {
        void report_once(macos_emulator& emu, const std::string& key, const std::string& message)
        {
            static std::set<std::string> reported{};
            if (reported.insert(key).second)
            {
                emu.log.warn("%s\n", message.c_str());
            }
        }

        // MTLCopyAllDevices reaches IOSurfaceClientCopyGPUPolicies, which asks the IOSurface kernel
        // service which accelerators exist. sogen emulates a CPU and no accelerator, and the honest
        // answer is the one a machine with no GPU gives: an empty device list. +[NSCGSWindow
        // createContext:] is measured to take that as setGPURegistryID:0 and carry on, which is the path
        // a real headless configuration takes.
        void mtl_copy_all_devices(const macos_native_call& call)
        {
            report_once(call.emu_ref, "metal-no-device",
                        "MTLCopyAllDevices: sogen emulates no accelerator, so the Metal device list is empty");

            if (macos_cf_build(call.emu_ref, macos_cf_value::array(),
                               [](macos_emulator& emu, const uint64_t root) { emu.emu().reg(arm64_register::x0, root); }))
            {
                return;
            }

            call.ret(0);
        }

        void mtl_create_system_default_device(const macos_native_call& call)
        {
            report_once(call.emu_ref, "metal-no-default-device",
                        "MTLCreateSystemDefaultDevice: sogen emulates no accelerator and answers nil");
            call.ret(0);
        }
    }

    void register_io_surface_routines(macos_native_dispatch& dispatch)
    {
        const std::string metal{MACOS_METAL_IMAGE_PATH};
        dispatch.register_routine(metal, "_MTLCopyAllDevices", mtl_copy_all_devices);
        dispatch.register_routine(metal, "_MTLCreateSystemDefaultDevice", mtl_create_system_default_device);
    }
}

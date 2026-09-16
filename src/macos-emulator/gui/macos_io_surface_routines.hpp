#pragma once

#include "../std_include.hpp"
#include "macos_native_dispatch.hpp"

#include <string_view>

namespace sogen
{
    // Metal is here because it fails for an IOSurface reason: enumerating devices reaches
    // IOSurfaceClientCopyGPUPolicies, which asks the kernel service for the accelerator inventory sogen
    // has none of. Everything else IOSurface does now goes through the real kernel service in
    // mach/io_surface_user_client.cpp rather than through an interception.
    constexpr std::string_view MACOS_METAL_IMAGE_PATH = "/System/Library/Frameworks/Metal.framework/Versions/A/Metal";

    void register_io_surface_routines(macos_native_dispatch& dispatch);
}

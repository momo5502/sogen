#pragma once

#include <cstdint>
#include <string_view>

namespace sogen::emulated_display
{
    // In-box Microsoft Basic Render Driver (WARP). Not a host GPU.
    constexpr uint32_t vendor_id = 0x1414;
    constexpr uint32_t device_id = 0x008C;
    constexpr uint32_t revision_id = 0;

    constexpr std::u16string_view description = u"Microsoft Basic Render Driver";
    constexpr std::u16string_view hardware_id = u"PCI\\VEN_1414&DEV_008C&SUBSYS_00000000&REV_00";
    constexpr std::u16string_view interface_path =
        u"\\\\?\\PCI#VEN_1414&DEV_008C&SUBSYS_00000000&REV_00#4&1234567&0&0008#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}";
} // namespace sogen::emulated_display

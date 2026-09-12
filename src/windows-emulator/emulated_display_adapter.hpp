#pragma once

#include <cstdio>
#include <cstring>
#include <string>

namespace sogen::emulated_display
{
    constexpr uint32_t vendor_id = 0x1414;
    constexpr uint32_t device_id = 0x008C;
    constexpr uint32_t revision_id = 0;
    constexpr LUID adapter_luid = {0x1000, 0};
    constexpr char16_t description[] = u"Microsoft Basic Render Driver";

    // Documented Windows display-adapter interface class, not a device instance.
    constexpr GUID interface_class = {0x5B45201D, 0xF2F2, 0x4F3B, {0x85, 0xBB, 0x30, 0xFF, 0x1F, 0x95, 0x35, 0x99}};

    // Stable emulator-only unique adapter id. Sequential, not a host device.
    constexpr GUID unique_id = {0x00000001, 0x0002, 0x0003, {0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

    inline std::u16string from_narrow(const char* text)
    {
        return {text, text + std::strlen(text)};
    }

    inline std::u16string hardware_id()
    {
        char buf[80]{};
        std::snprintf(buf, sizeof(buf), "PCI\\VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_%02X", vendor_id, device_id, revision_id);
        return from_narrow(buf);
    }

    inline std::u16string interface_path()
    {
        char buf[192]{};
        std::snprintf(buf, sizeof(buf),
                      "\\\\?\\PCI#VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_%02X#4&1234567&0&0008#{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                      vendor_id, device_id, revision_id, interface_class.Data1, interface_class.Data2, interface_class.Data3,
                      interface_class.Data4[0], interface_class.Data4[1], interface_class.Data4[2], interface_class.Data4[3],
                      interface_class.Data4[4], interface_class.Data4[5], interface_class.Data4[6], interface_class.Data4[7]);
        return from_narrow(buf);
    }
} // namespace sogen::emulated_display

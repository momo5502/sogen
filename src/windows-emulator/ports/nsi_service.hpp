#pragma once

#include "../port.hpp"

namespace sogen
{
    // The LRPC endpoint the endpoint mapper hands out for the Network Store Interface service.
    inline constexpr std::u16string_view nsi_service_endpoint = u"SogenNsiService";
    inline constexpr std::array<uint8_t, 16> nsi_service_interface{0xcf, 0x0b, 0xa7, 0x7e, 0xaf, 0x48, 0x6a, 0x4f,
                                                                   0x89, 0x68, 0x6a, 0x44, 0x07, 0x54, 0xd5, 0xfa};
    inline constexpr uint16_t nsi_service_interface_major_version = 1;

    std::unique_ptr<port> create_nsi_service_port();
}

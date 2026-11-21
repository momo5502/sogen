#pragma once

#include <cstdint>
#include <vector>

namespace sogen::smbios
{
    // The RawSMBIOSData that the 'RSMB' firmware table provider returns: Used20CallingMethod, the SMBIOS major and
    // minor version, DmiRevision, the table length, then the structure table. It describes a fixed virtual machine,
    // so no host identity reaches the guest and the identity is the same across runs and snapshots.
    const std::vector<uint8_t>& raw_smbios_data();
}

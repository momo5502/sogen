#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sogen
{
    struct macos_system_info;
    struct macos_process_context;

    struct macos_sysctl_value
    {
        enum class kind : uint8_t
        {
            integer32,
            integer64,
            string,
        };

        kind value_kind{};
        uint64_t number{};
        std::string text{};
    };

    // The table is closed: a name it does not carry is absent, and no query is ever forwarded to the
    // host's sysctl. Otherwise a guest would read the analyst's machine, and PAuth queries would be
    // answered by a QEMU ID register that reports architected QARMA where Apple silicon reports its own
    // implementation.
    std::optional<macos_sysctl_value> resolve_macos_sysctl(std::string_view name, const macos_system_info& info,
                                                           const macos_process_context& proc);

    // Nodes with an answer but no static oid. Real kernels register these at boot and hand out whatever
    // number is free, so the index here is as good as any -- a guest only learns it by asking name2oid,
    // and the reverse lookup turns it straight back into the name.
    constexpr int32_t MACOS_CTL_DYNAMIC = 0x50;

    constexpr std::array<std::string_view, 21> MACOS_SYSCTL_DYNAMIC_NAMES{
        "hw.activecpu",
        "hw.cpufamily",
        "hw.ephemeral_storage",
        "hw.logicalcpu",
        "hw.optional.arm.FEAT_BTI",
        "hw.optional.arm.FEAT_FP16",
        "hw.optional.arm.FEAT_LSE",
        "hw.optional.arm.FEAT_PAuth",
        "hw.optional.arm.caps",
        "hw.optional.arm64",
        "hw.optional.floatingpoint",
        "hw.optional.neon",
        "hw.pagesize32",
        "hw.physicalcpu",
        "kern.bootargs",
        "kern.hv_support",
        "kern.iossupportversion",
        "kern.osproductversion",
        "kern.osvariant_status",
        "kern.secure_kernel",
        "security.mac.lockdown_mode_state",
    };

    std::optional<std::string> macos_sysctl_mib_to_name(std::span<const int32_t> mib);
}

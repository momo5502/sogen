#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <arch_emulator.hpp>
#include <platform/platform.hpp>

namespace sogen
{

    enum class backend_type : uint8_t
    {
        unicorn,
        icicle,
        whp,
        kvm,
        fex,
        hvf,
    };

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator(backend_type backend = backend_type::unicorn, size_t vcpu_count = 1);
    std::unique_ptr<arm64_emulator> create_arm64_emulator(backend_type backend, size_t vcpu_count = 1);
    backend_type get_x86_64_emulator_backend_from_environment();

    // The host's hardware backend where one is usable (WHP on Windows ARM64; Hypervisor.framework on Apple
    // Silicon when the process holds the hypervisor entitlement), Unicorn otherwise. EMULATOR_UNICORN=1 and
    // EMULATOR_HVF=1 override the choice.
    backend_type get_arm64_emulator_backend_from_environment();
    std::unique_ptr<arm64_emulator> create_arm64_emulator_from_environment(size_t vcpu_count = 1);

    inline std::unique_ptr<arm64_emulator> create_arm64_emulator()
    {
        return create_arm64_emulator_from_environment();
    }

    // Creates the CPU for an executable's COFF machine: AMD64 and I386 (WoW64) images run on an x86-64
    // CPU, ARM64 images on an ARM64 CPU. Without an explicit backend, x86-64 follows the environment
    // selection and ARM64 the host default.
    std::unique_ptr<guest_machine> create_emulator_for_machine(PEMachineType machine, std::optional<backend_type> backend = std::nullopt,
                                                               size_t vcpu_count = 1);
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator_from_environment(size_t vcpu_count = 1);
} // namespace sogen

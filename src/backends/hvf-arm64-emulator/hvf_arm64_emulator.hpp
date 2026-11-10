#pragma once

#include <arch_emulator.hpp>
#include <platform/compiler.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

#ifdef HVF_ARM64_EMULATOR_IMPL
#define HVF_ARM64_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define HVF_ARM64_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace sogen::hvf
{
    // True when the host supports Hypervisor.framework and this process holds the
    // com.apple.security.hypervisor entitlement.
#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    bool is_available();

#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<arm64_emulator> create_arm64_emulator(size_t vcpu_count = 1);

    // For running host-resident code at EL0 inside the VM, such as FEX's translated x64 code (docs/hvf-backend.md).
    // Shared host memory appears in the VM at its own address, so host pointers stay valid there. Executable host
    // memory cannot be shared.

    // Shows [address, address + size) of the host's own memory with these permissions, or removes it without them.
#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    void share_host_memory(arm64_emulator& vm, uint64_t address, size_t size, std::optional<memory_permission> permissions);

    // The same for host memory at another address.
#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    void share_host_memory(arm64_emulator& vm, uint64_t address, size_t size, void* host, std::optional<memory_permission> permissions);

    // Shows a private copy of host memory at the same address, for constants in executable images.
#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    void copy_host_memory(arm64_emulator& vm, uint64_t address, size_t size, memory_permission permissions);

    // A page the VM's loads read from, refreshed from the read callback before every guest entry.
#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    void map_mmio(arm64_emulator& vm, uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb);

#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    void unmap(arm64_emulator& vm, uint64_t address, size_t size);

#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    bool can_share_host_memory(uint64_t address);

#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    bool is_mapped(arm64_emulator& vm, uint64_t address);

    struct el0_exception
    {
        int exception_class{};
        uint64_t syndrome{};
        uint64_t fault_address{};
    };

    // Sees every synchronous exception the guest raises at EL0 before the backend's own handling, with the vCPU at the
    // faulting instruction. Returning true resumes the guest with the registers the handler left.
    using el0_exception_handler = std::function<bool(arm64_cpu& cpu, const el0_exception& exception)>;

#if !SOGEN_BUILD_STATIC
    HVF_ARM64_EMULATOR_DLL_STORAGE
#endif
    void set_el0_exception_handler(arm64_emulator& vm, el0_exception_handler handler);
}

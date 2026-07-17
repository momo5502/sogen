#include "../std_include.hpp"
#include "module_manager.hpp"
#include "module_mapping.hpp"
#include "platform/win_pefile.hpp"
#include <logger.hpp>
#include "../wow64_heaven_gate.hpp"
#include "../version/windows_version_manager.hpp"
#include "../process_context.hpp"

#include <serialization_helper.hpp>
#include <cinttypes>
#include <cstring>
#include <vector>

namespace sogen
{

    namespace
    {
        // gNlsProcessLocalCache's RVA silently drifts across different Windows servicing/OS-build
        // combinations (confirmed directly: it differs between the Windows 2022 and 2025 CI-baked
        // kernelbase.dll builds - one relocates the global entirely, out of where the other build's
        // RVA would land). Since real ntdll/kernelbase code eventually WRITES through whatever
        // TEB.NlsCache points at, resolving to a stale RVA that now falls on the wrong page/section is
        // a guaranteed guest access violation rather than a silent correctness gap - so validate
        // against the module's own section table (parsed from the real PE headers at map time, so it's
        // always accurate for THIS build) before trusting any resolved address.
        bool address_is_in_writable_section(const mapped_module& mod, const uint64_t address)
        {
            for (const auto& section : mod.sections)
            {
                const auto& region = section.region;
                if (address >= region.start && address - region.start < region.length)
                {
                    return (region.permissions & memory_permission::write) != memory_permission::none;
                }
            }

            return false;
        }

        // Locates kernelbase.dll's private (non-exported) gNlsProcessLocalCache global by scanning the
        // module's own code for the fixed TEB.NlsCache access sequence inside BaseNlsThreadCleanup:
        //   65 48 8B 04 25 30 00 00 00   mov rax, gs:[0x30]      (TEB self-pointer)
        //   48 8B 98 A0 17 00 00         mov rbx, [rax+0x17A0]    (TEB.NlsCache)
        //   48 8D 05 xx xx xx xx         lea rax, [rip+disp32]    (&gNlsProcessLocalCache)
        // Confirmed via idasql against both the Windows 2022 and 2025 CI-baked kernelbase.dll builds:
        // this exact 19-byte prefix is byte-identical across both (the TEB access is an ABI-level,
        // fixed-offset structure read that the compiler always emits the same way), and only the
        // trailing RIP-relative displacement - the actual per-build variable - differs. This is what
        // makes the scan build-independent, unlike a hardcoded RVA (see address_is_in_writable_section's
        // doc comment above for why a stale RVA is dangerous, not just wrong).
        std::optional<uint64_t> scan_kernelbase_nls_cache_reference(const memory_manager& memory, const mapped_module& kernelbase)
        {
            static constexpr std::array<uint8_t, 19> pattern{
                0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00, // mov rax, gs:[0x30]
                0x48, 0x8B, 0x98, 0xA0, 0x17, 0x00, 0x00,             // mov rbx, [rax+0x17A0]
                0x48, 0x8D, 0x05,                                     // lea rax, [rip+disp32]
            };

            for (const auto& section : kernelbase.sections)
            {
                const auto& region = section.region;
                if (!is_executable(region.permissions) || region.length < pattern.size() + sizeof(int32_t))
                {
                    continue;
                }

                std::vector<uint8_t> data(region.length);
                if (!memory.try_read_memory(region.start, data.data(), data.size()))
                {
                    continue;
                }

                const auto search_end = data.size() - pattern.size() - sizeof(int32_t);
                for (size_t i = 0; i <= search_end; ++i)
                {
                    if (std::memcmp(data.data() + i, pattern.data(), pattern.size()) != 0)
                    {
                        continue;
                    }

                    int32_t displacement{};
                    std::memcpy(&displacement, data.data() + i + pattern.size(), sizeof(displacement));

                    const auto instruction_end = region.start + i + pattern.size() + sizeof(displacement);
                    return static_cast<uint64_t>(static_cast<int64_t>(instruction_end) + displacement);
                }
            }

            return std::nullopt;
        }

        // Locates ntdll32's own cached copy of TEB32.WOW32Reserved (fs:[0xC0], the undocumented but
        // well-established "Wow64Transition" field). Every 32-bit syscall stub reaches the wow64
        // dispatcher via a shared two-instruction indirection: the stub does
        // `mov eax,<syscall#>; mov edx,<thunk>; call edx` where <thunk> is a small, module-internal
        // trampoline that itself does `jmp dword ptr [<cache>]` - <cache> being ntdll32's OWN cached
        // copy of fs:[0xC0], read once during LdrpInitializeProcess rather than re-read from the TEB
        // on every syscall. Writing fs:[0xC0] itself (e.g. once wow64cpu.dll loads and its real thunk
        // address becomes known) is therefore too late: that caching already happened, with whatever
        // (null, since this backend never runs whatever real code would set fs:[0xC0]) fs:[0xC0] held
        // at process start. Scan for the syscall stub's own bytes instead, extract <thunk> from the
        // stub's second `mov` immediate, then dereference <thunk>'s own `jmp dword ptr [<cache>]` to
        // get <cache>'s real address - this needs no hardcoded RVA and survives ntdll32 build changes,
        // matching scan_kernelbase_nls_cache_reference's approach above for the same class of problem.
        std::optional<uint64_t> scan_wow64_transition_cache_address(const memory_manager& memory, const mapped_module& ntdll32)
        {
            for (const auto& section : ntdll32.sections)
            {
                const auto& region = section.region;
                constexpr size_t stub_size = 12; // B8 imm32 BA imm32 FF D2
                if (!is_executable(region.permissions) || region.length < stub_size)
                {
                    continue;
                }

                std::vector<uint8_t> data(region.length);
                if (!memory.try_read_memory(region.start, data.data(), data.size()))
                {
                    continue;
                }

                for (size_t i = 0; i + stub_size <= data.size(); ++i)
                {
                    if (data[i] != 0xB8 || data[i + 5] != 0xBA || data[i + 10] != 0xFF || data[i + 11] != 0xD2)
                    {
                        continue;
                    }

                    uint32_t thunk_address{};
                    std::memcpy(&thunk_address, &data[i + 6], sizeof(thunk_address));

                    std::array<uint8_t, 6> thunk_bytes{};
                    if (!memory.try_read_memory(thunk_address, thunk_bytes.data(), thunk_bytes.size()) || thunk_bytes[0] != 0xFF ||
                        thunk_bytes[1] != 0x25)
                    {
                        continue;
                    }

                    uint32_t cache_address{};
                    std::memcpy(&cache_address, &thunk_bytes[2], sizeof(cache_address));
                    return cache_address;
                }
            }

            return std::nullopt;
        }

        // wow64cpu.dll's real RunSimulatedCode entry has no export, so its caller previously used a
        // single hand-decoded RVA relative to TurboDispatchJumpAddressStart. That fixed offset drifts
        // across wow64cpu.dll builds - confirmed via idasql against a genuine Windows Server 2025
        // system DLL (the one CI's create-emulation-root job bakes): the hardcoded RVA landed 0x11
        // (17) bytes before the real prologue, inside the tail (a bare `retn`) of the unrelated,
        // preceding function. The registered gate crossing (built from that RVA up to
        // TurboDispatchJumpAddressStart) then accidentally swallowed that ret too, and FEXCore's JIT
        // refused to execute it (a genuine NoExec fault) every time real control flow legitimately
        // returned through it - surfacing as a deterministic, CI-only STATUS_FATAL_USER_CALLBACK_
        // EXCEPTION during the one-time win32k client-thread-setup callback. RunSimulatedCode's
        // prologue is a highly distinctive, fixed instruction sequence (9 specific register pushes in
        // a specific order, then a specific stack adjustment) any compiler emits identically
        // regardless of build - scan for it directly, matching scan_kernelbase_nls_cache_reference/
        // scan_wow64_transition_cache_address's approach for the same class of problem, instead of
        // trusting a single hand-decoded RVA. Returns the closest match preceding dispatch_start
        // (TurboDispatchJumpAddressStart), matching the known layout invariant that RunSimulatedCode
        // precedes it in the same dispatch code block.
        std::optional<uint64_t> scan_run_simulated_code_entry(const memory_manager& memory, const mapped_module& wow64cpu,
                                                              uint64_t dispatch_start)
        {
            static constexpr std::array<uint8_t, 16> pattern{
                0x41, 0x57,             // push r15
                0x41, 0x56,             // push r14
                0x41, 0x55,             // push r13
                0x41, 0x54,             // push r12
                0x53,                   // push rbx
                0x56,                   // push rsi
                0x57,                   // push rdi
                0x55,                   // push rbp
                0x48, 0x83, 0xEC, 0x68, // sub rsp, 0x68
            };

            std::optional<uint64_t> best;

            for (const auto& section : wow64cpu.sections)
            {
                const auto& region = section.region;
                if (!is_executable(region.permissions) || region.length < pattern.size())
                {
                    continue;
                }

                std::vector<uint8_t> data(region.length);
                if (!memory.try_read_memory(region.start, data.data(), data.size()))
                {
                    continue;
                }

                for (size_t i = 0; i + pattern.size() <= data.size(); ++i)
                {
                    if (std::memcmp(data.data() + i, pattern.data(), pattern.size()) != 0)
                    {
                        continue;
                    }

                    const auto candidate = region.start + i;
                    if (candidate < dispatch_start && (!best.has_value() || candidate > *best))
                    {
                        best = candidate;
                    }
                }
            }

            return best;
        }

        // Returns 0 (unresolved) if neither the build-independent code scan above nor the hardcoded-RVA
        // fallback land on writable data for THIS specific build - see address_is_in_writable_section's
        // doc comment. The scan is the primary path; the hardcoded RVA (derived once via static analysis
        // of the original macOS dev-build kernelbase.dll) is kept only as a last-resort fallback for the
        // case where BaseNlsThreadCleanup's code shape changes enough that the scan no longer matches.
        uint64_t resolve_kernelbase_nls_cache_address(const memory_manager& memory, const mapped_module& kernelbase)
        {
            if (const auto scanned = scan_kernelbase_nls_cache_reference(memory, kernelbase);
                scanned.has_value() && address_is_in_writable_section(kernelbase, *scanned))
            {
                return *scanned;
            }

            constexpr uint64_t kernelbase_gnls_process_local_cache_rva = 0x326c00;
            const auto address = kernelbase.image_base + kernelbase_gnls_process_local_cache_rva;
            return address_is_in_writable_section(kernelbase, address) ? address : 0;
        }

        uint64_t get_system_dll_init_block_size(const windows_version_manager& version)
        {
            if (version.is_build_after_or_equal(WINDOWS_VERSION::WINDOWS_11_24H2))
            {
                return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V3;
            }
            if (version.is_build_after_or_equal(WINDOWS_VERSION::WINDOWS_10_2004))
            {
                return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V3_2004;
            }
            if (version.is_build_after_or_equal(WINDOWS_VERSION::WINDOWS_10_1709))
            {
                return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V2;
            }
            if (version.is_build_after_or_equal(WINDOWS_VERSION::WINDOWS_10_1703))
            {
                return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V2_1703;
            }
            return PS_SYSTEM_DLL_INIT_BLOCK_SIZE_V1;
        }
    }

    namespace utils
    {
        static void serialize(buffer_serializer& buffer, const exported_symbol& sym)
        {
            buffer.write(sym.name);
            buffer.write(sym.ordinal);
            buffer.write(sym.rva);
            buffer.write(sym.address);
        }

        static void deserialize(buffer_deserializer& buffer, exported_symbol& sym)
        {
            buffer.read(sym.name);
            buffer.read(sym.ordinal);
            buffer.read(sym.rva);
            buffer.read(sym.address);
        }

        static void serialize(buffer_serializer& buffer, const basic_memory_region<>& region)
        {
            buffer.write(region.start);
            buffer.write<uint64_t>(region.length);
            buffer.write(region.permissions);
        }

        static void deserialize(buffer_deserializer& buffer, basic_memory_region<>& region)
        {
            buffer.read(region.start);
            region.length = static_cast<size_t>(buffer.read<uint64_t>());
            buffer.read(region.permissions);
        }

        static void serialize(buffer_serializer& buffer, const mapped_section& mod)
        {
            buffer.write_optional(mod.first_execute);
            buffer.write(mod.name);
            buffer.write(mod.region);
        }

        static void deserialize(buffer_deserializer& buffer, mapped_section& mod)
        {
            buffer.read_optional(mod.first_execute);
            buffer.read(mod.name);
            buffer.read(mod.region);
        }

        static void serialize(buffer_serializer& buffer, const mapped_module& mod)
        {
            buffer.write(mod.name);
            buffer.write(mod.path);
            buffer.write(mod.module_path);

            buffer.write(mod.image_base);
            buffer.write(mod.image_base_file);
            buffer.write(mod.size_of_image);
            buffer.write(mod.entry_point);

            buffer.write(mod.machine);
            buffer.write(mod.dll_characteristics);
            buffer.write(mod.size_of_stack_reserve);
            buffer.write(mod.size_of_stack_commit);
            buffer.write(mod.size_of_heap_reserve);
            buffer.write(mod.size_of_heap_commit);

            buffer.write_vector(mod.exports);
            buffer.write_map(mod.address_names);

            buffer.write_vector(mod.sections);

            buffer.write(mod.is_static);
        }

        static void deserialize(buffer_deserializer& buffer, mapped_module& mod)
        {
            buffer.read(mod.name);
            buffer.read(mod.path);
            buffer.read(mod.module_path);

            buffer.read(mod.image_base);
            buffer.read(mod.image_base_file);
            buffer.read(mod.size_of_image);
            buffer.read(mod.entry_point);

            buffer.read(mod.machine);
            buffer.read(mod.dll_characteristics);
            buffer.read(mod.size_of_stack_reserve);
            buffer.read(mod.size_of_stack_commit);
            buffer.read(mod.size_of_heap_reserve);
            buffer.read(mod.size_of_heap_commit);

            buffer.read_vector(mod.exports);
            buffer.read_map(mod.address_names);

            buffer.read_vector(mod.sections);

            buffer.read(mod.is_static);
        }
    }

    pe_detection_result pe_architecture_detector::detect_from_file(const std::filesystem::path& file)
    {
        auto variant_result = winpe::get_pe_arch(file);

        if (std::holds_alternative<std::error_code>(variant_result))
        {
            pe_detection_result result;
            const auto error_code = std::get<std::error_code>(variant_result);
            result.error_message = error_code.message();
            return result;
        }

        auto arch = std::get<winpe::pe_arch>(variant_result);
        pe_detection_result result;
        result.architecture = arch;
        result.suggested_mode = determine_execution_mode(arch);
        return result;
    }

    pe_detection_result pe_architecture_detector::detect_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size)
    {
        const auto reader = [&](uint64_t offset, void* dst, size_t n) -> bool {
            return memory.try_read_memory(base_address + offset, dst, n);
        };
        auto variant_result = winpe::get_pe_arch(image_size, reader);

        if (std::holds_alternative<std::error_code>(variant_result))
        {
            pe_detection_result result;
            result.error_message = "Failed to detect PE architecture from memory at 0x" + std::to_string(base_address);
            return result;
        }

        auto arch = std::get<winpe::pe_arch>(variant_result);
        pe_detection_result result;
        result.architecture = arch;
        result.suggested_mode = determine_execution_mode(arch);
        return result;
    }

    execution_mode pe_architecture_detector::determine_execution_mode(winpe::pe_arch executable_arch)
    {
        switch (executable_arch)
        {
        case winpe::pe_arch::pe32:
            return execution_mode::wow64_32bit;
        case winpe::pe_arch::pe64:
            return execution_mode::native_64bit;
        default:
            return execution_mode::unknown;
        }
    }

    // PE32 Mapping Strategy Implementation
    mapped_module pe32_mapping_strategy::map_from_file(memory_manager& memory, std::filesystem::path file, windows_path module_path,
                                                       const uint64_t relocation_base)
    {
        return map_module_from_file<std::uint32_t>(memory, std::move(file), std::move(module_path), relocation_base);
    }

    mapped_module pe32_mapping_strategy::map_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                         windows_path module_path)
    {
        return map_module_from_memory<std::uint32_t>(memory, base_address, image_size, std::move(module_path));
    }

    // PE64 Mapping Strategy Implementation
    mapped_module pe64_mapping_strategy::map_from_file(memory_manager& memory, std::filesystem::path file, windows_path module_path,
                                                       const uint64_t relocation_base)
    {
        return map_module_from_file<std::uint64_t>(memory, std::move(file), std::move(module_path), relocation_base);
    }

    mapped_module pe64_mapping_strategy::map_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                         windows_path module_path)
    {
        return map_module_from_memory<std::uint64_t>(memory, base_address, image_size, module_path);
    }

    mapping_strategy_factory::mapping_strategy_factory()
        : pe32_strategy_(std::make_unique<pe32_mapping_strategy>()),
          pe64_strategy_(std::make_unique<pe64_mapping_strategy>())
    {
    }

    module_mapping_strategy& mapping_strategy_factory::get_strategy(winpe::pe_arch arch)
    {
        switch (arch)
        {
        case winpe::pe_arch::pe32:
            return *pe32_strategy_;
        case winpe::pe_arch::pe64:
            return *pe64_strategy_;
        default:
            throw std::runtime_error("Unsupported PE architecture");
        }
    }

    module_manager::module_manager(memory_manager& memory, file_system& file_sys, callbacks& cb)
        : memory_(&memory),
          file_sys_(&file_sys),
          callbacks_(&cb)
    {
    }

    mapped_module* module_manager::map_module_core(const pe_detection_result& detection_result,
                                                   const std::function<mapped_module()>& mapper, const logger& logger, bool is_static)
    {
        if (!detection_result.is_valid())
        {
            logger.error("Cannot map module: %s\n", detection_result.error_message.c_str());
            return nullptr;
        }

        try
        {
            [[maybe_unused]] auto& strategy = strategy_factory_.get_strategy(detection_result.architecture);
            mapped_module mod = mapper();
            mod.is_static = is_static;

            if (!mod.path.empty())
            {
                this->modules_load_count[mod.path]++;
            }

            const auto image_base = mod.image_base;
            const auto entry = this->modules_.try_emplace(image_base, std::move(mod));
            this->last_module_cache_ = this->modules_.end();
            this->callbacks_->on_module_load(entry.first->second);
            return &entry.first->second;
        }
        catch (const std::exception& e)
        {
            logger.error("Failed to map module: %s\n", e.what());
            return nullptr;
        }
    }

    execution_mode module_manager::detect_execution_mode(const windows_path& executable_path, const logger& logger)
    {
        const auto local_file = this->file_sys_->translate(executable_path);
        auto detection_result = pe_architecture_detector::detect_from_file(local_file);

        if (!detection_result.is_valid())
        {
            logger.error("Failed to detect executable architecture of file %s: %s\n", local_file.string().c_str(),
                         detection_result.error_message.c_str());
            return execution_mode::unknown;
        }

        return detection_result.suggested_mode;
    }

    void module_manager::load_native_64bit_modules(const windows_path& executable_path, const windows_path& ntdll_path,
                                                   const windows_path& win32u_path, const logger& logger)
    {
        this->executable = this->map_module_or_throw(executable_path, logger, true);
        this->memory_->set_dep_enabled(this->executable->machine != static_cast<uint16_t>(PEMachineType::I386) ||
                                       (this->executable->dll_characteristics & IMAGE_DLLCHARACTERISTICS_NX_COMPAT) != 0);

        this->ntdll = this->map_module_or_throw(ntdll_path, logger, true);
        this->win32u = this->map_module_or_throw(win32u_path, logger, true);
    }

    void module_manager::load_wow64_modules(x86_64_emulator& emu, const windows_path& executable_path, const windows_path& ntdll_path,
                                            const windows_path& win32u_path, const windows_path& ntdll32_path,
                                            windows_version_manager& version, const logger& logger)
    {
        load_native_64bit_modules(executable_path, ntdll_path, win32u_path, logger);

        this->wow64_modules_.ntdll32 = this->map_module_or_throw(ntdll32_path, logger, true);

        const auto ntdll32_original_imagebase = this->wow64_modules_.ntdll32->get_image_base_file();
        const auto ntdll64_original_imagebase = this->ntdll->get_image_base_file();

        if (ntdll32_original_imagebase == 0 || ntdll64_original_imagebase == 0)
        {
            logger.error("Failed to get PE ImageBase values for WOW64 setup\n");
            return;
        }

        PS_SYSTEM_DLL_INIT_BLOCK init_block = {};
        const auto init_block_size = get_system_dll_init_block_size(version);

        init_block.Size = static_cast<ULONG>(init_block_size);

        // Calculate relocation values
        // SystemDllWowRelocation = mapped_base - original_imagebase for 32-bit ntdll
        init_block.SystemDllWowRelocation = this->wow64_modules_.ntdll32->image_base - ntdll32_original_imagebase;

        // SystemDllNativeRelocation = mapped_base - original_imagebase for 64-bit ntdll
        init_block.SystemDllNativeRelocation = this->ntdll->image_base - ntdll64_original_imagebase;

        // Fill Wow64SharedInformation array with 32-bit ntdll export addresses
        init_block.Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32LdrInitializeThunk)] =
            this->wow64_modules_.ntdll32->find_export("LdrInitializeThunk");
        init_block.Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32KiUserExceptionDispatcher)] =
            this->wow64_modules_.ntdll32->find_export("KiUserExceptionDispatcher");
        init_block.Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32KiUserApcDispatcher)] =
            this->wow64_modules_.ntdll32->find_export("KiUserApcDispatcher");
        init_block.Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32KiUserCallbackDispatcher)] =
            this->wow64_modules_.ntdll32->find_export("KiUserCallbackDispatcher");
        init_block.Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32RtlUserThreadStart)] =
            this->wow64_modules_.ntdll32->find_export("RtlUserThreadStart");
        init_block
            .Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32pQueryProcessDebugInformationRemote)] =
            this->wow64_modules_.ntdll32->find_export("RtlpQueryProcessDebugInformationRemote");
        init_block.Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32BaseAddress)] =
            this->wow64_modules_.ntdll32->image_base;
        init_block.Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32LdrSystemDllInitBlock)] =
            this->wow64_modules_.ntdll32->find_export("LdrSystemDllInitBlock");
        init_block.Wow64SharedInformation[static_cast<uint64_t>(WOW64_SHARED_INFORMATION_V5::SharedNtdll32RtlpFreezeTimeBias)] =
            this->wow64_modules_.ntdll32->find_export("RtlpFreezeTimeBias");

        // Set RngData to a random non-zero value for early randomization
        init_block.RngData = 0x11111111;

        // Set flags and mitigation options based on WinDbg data
        init_block.Flags = 0x22222022;
        init_block.MitigationOptionsMap.Map[0] = 0x20002000;
        init_block.MitigationOptionsMap.Map[1] = 0x00000002;
        init_block.MitigationOptionsMap.Map[2] = 0x00000000;

        // CFG and audit options (set to zero as per WinDbg data)
        init_block.CfgBitMap = 0;
        init_block.CfgBitMapSize = 0;
        init_block.Wow64CfgBitMap = 0;
        init_block.Wow64CfgBitMapSize = 0;
        init_block.MitigationAuditOptionsMap.Map[0] = 0;
        init_block.MitigationAuditOptionsMap.Map[1] = 0;
        init_block.MitigationAuditOptionsMap.Map[2] = 0;

        // Find LdrSystemDllInitBlock export address in 64-bit ntdll and write the structure
        const auto ldr_init_block_addr = this->ntdll->find_export("LdrSystemDllInitBlock");
        if (ldr_init_block_addr == 0)
        {
            logger.error("Failed to find LdrSystemDllInitBlock export in 64-bit ntdll\n");
            return;
        }

        this->memory_->write_memory(ldr_init_block_addr, &init_block, static_cast<size_t>(init_block_size));

        // Install the WOW64 Heaven's Gate trampoline used for compat-mode -> 64-bit transitions.
        this->install_wow64_heaven_gate(emu, logger);
    }

    void module_manager::install_wow64_heaven_gate(x86_64_emulator& emu, const logger& logger)
    {
        using wow64::heaven_gate::kCodeBase;
        using wow64::heaven_gate::kCodeSize;
        using wow64::heaven_gate::kStackBase;
        using wow64::heaven_gate::kStackSize;
        using wow64::heaven_gate::kTrampolineBytes;

        // Unlike a real module's preferred base, kCodeBase/kStackBase aren't addresses real
        // Windows/wow64cpu.dll require - they're sogen-internal implementation details (kTrampolineBytes
        // is itself fully position-independent code: its one "call" targets its own next instruction,
        // not an absolute address), so there's no compatibility reason they must sit exactly there. Try
        // the preferred address first (the common, fast path - almost always free), but fall back to
        // searching for any free spot below 4GB if it isn't. This matters on a backend sharing the guest
        // address space with the host process (FEX on Apple Silicon): a real regression showed the
        // preferred kCodeBase can already be claimed by a foreign host mapping - this process's own
        // Cocoa/Metal GPU subsystem reserves a large host VA range whose ASLR placement occasionally
        // lands exactly there. Confirmed to be a genuine, ongoing race (not just a one-time startup
        // ordering issue): even when the address reads as free at allocation time, Metal's own
        // background thread can still claim it in the narrow window before the trampoline bytes are
        // actually written, so the write itself must be retried at a new address on failure too, not
        // just the initial allocation. With none of this handled, real wow64 runs failed outright in
        // roughly 20-40% of runs.
        constexpr uint64_t heaven_gate_below_4gb_ceiling = 0xFFFFFFFFULL;
        constexpr int max_heaven_gate_retries = 8;

        auto allocate_or_validate = [&](uint64_t preferred_base, size_t size, memory_permission perms,
                                        const char* name) -> std::optional<uint64_t> {
            if (this->memory_->allocate_memory(preferred_base, size, perms))
            {
                return preferred_base;
            }

            const auto region = this->memory_->get_region_info(preferred_base);
            if (region.is_reserved && region.allocation_length >= size)
            {
                return preferred_base;
            }

            const uint64_t relocated_base =
                this->memory_->find_free_host_allocation_base(size, preferred_base, heaven_gate_below_4gb_ceiling);
            if (!relocated_base || !this->memory_->allocate_memory(relocated_base, size, perms))
            {
                logger.error("Failed to allocate %s (size 0x%zx)\n", name, size);
                return std::nullopt;
            }

            return relocated_base;
        };

        // Allocates (or relocates) preferred_base, then runs `commit` (the actual writes/protection
        // changes). If `commit` reports failure, releases the allocation and retries at a fresh
        // address, up to max_heaven_gate_retries times.
        //
        // Deliberately does NOT just retry allocate_or_validate(preferred_base, ...) unchanged: on this
        // backend, the fixed-address path's underlying host mmap uses MAP_FIXED, which unconditionally
        // succeeds by silently clobbering whatever was already there - it can't detect Metal's
        // background thread continuing to reclaim the exact same address after we do (a genuinely
        // adversarial, repeated race, confirmed empirically: identical retries at the same preferred
        // address kept reporting the same commit failure every time). So after the first failure this
        // switches straight to a real search (find_free_host_allocation_base, which confirms host
        // availability and rescans past foreign occupants).
        //
        // The relocation search always restarts from heaven_gate_search_floor (a low, fixed address),
        // never from just past the last failed range: kCodeBase/kStackBase sit near the TOP of the
        // below-4GB range, so "search upward from the failure" would only ever cover the last few MB
        // before hitting the 4GB ceiling, silently ignoring the (likely much larger) free space below
        // the preferred address entirely. Restarting from the floor each time lets the full below-4GB
        // range be considered; find_free_host_allocation_base's own rescan-and-retry already keeps a
        // repeat failure at the same spot from looping forever.
        constexpr uint64_t heaven_gate_search_floor = 0x10000ULL;

        auto commit_with_retry = [&](uint64_t preferred_base, size_t size, const char* name,
                                     const std::function<bool(uint64_t)>& commit) -> std::optional<uint64_t> {
            bool force_relocate = false;

            for (int attempt = 0; attempt <= max_heaven_gate_retries; ++attempt)
            {
                std::optional<uint64_t> base;
                if (!force_relocate)
                {
                    base = allocate_or_validate(preferred_base, size, memory_permission::read_write, name);
                }
                else
                {
                    const uint64_t relocated_base =
                        this->memory_->find_free_host_allocation_base(size, heaven_gate_search_floor, heaven_gate_below_4gb_ceiling);
                    if (relocated_base && this->memory_->allocate_memory(relocated_base, size, memory_permission::read_write))
                    {
                        base = relocated_base;
                    }
                }

                if (!base)
                {
                    logger.error("Failed to allocate %s (size 0x%zx)\n", name, size);
                    return std::nullopt;
                }

                if (commit(*base))
                {
                    return base;
                }

                logger.error("Failed to commit %s at 0x%" PRIx64 " (size 0x%zx) - retrying elsewhere\n", name, *base, size);
                this->memory_->release_memory(*base, size);
                force_relocate = true;
            }

            logger.error("Failed to commit %s after %d retries\n", name, max_heaven_gate_retries);
            return std::nullopt;
        };

        const auto code_base = commit_with_retry(kCodeBase, kCodeSize, "WOW64 heaven gate code", [&](uint64_t base) {
            if (!this->memory_->protect_memory(base, kCodeSize, nt_memory_permission(memory_permission::read_write)))
            {
                return false;
            }

            std::vector<uint8_t> buffer(kCodeSize, 0);
            if (!this->memory_->try_write_memory(base, buffer.data(), buffer.size()) ||
                !this->memory_->try_write_memory(base, kTrampolineBytes.data(), kTrampolineBytes.size()))
            {
                return false;
            }

            this->memory_->protect_memory(base, kCodeSize, nt_memory_permission(memory_permission::read | memory_permission::exec));
            return true;
        });

        if (code_base)
        {
            this->wow64_heaven_gate_code_base_ = *code_base;

            // KVM/Unicorn genuinely execute the trampoline's real machine code (a real CS-segment
            // mode switch via retf/iretq) - real|exec is correct and sufficient for them, so
            // register_gate_crossing is a no-op default there. A JIT-based backend (FEXCore)
            // cannot execute that at all (bitness fixed per compiled Context - see
            // notify_process_bitness's doc comment) and instead intercepts execution reaching this
            // page before any of it is ever JIT-compiled, then synthesizes the mode switch itself
            // (marshal register state into its other-bitness Context and flip which one runs) -
            // see register_gate_crossing / gate_crossing_kind::heaven_gate (arch_emulator.hpp).
            // The trampoline is driven with the confirmed convention RAX=RIP, RBX=RSP, RCX=CS,
            // RDX=SS (see exception_dispatch.cpp), which the FEX crossing handler decodes.
            emu.register_gate_crossing(*code_base, kCodeSize, x86_64_emulator::gate_crossing_kind::heaven_gate);

            if (this->modules_.contains(*code_base))
            {
                mapped_module module{};
                module.name = "wow64_heaven_gate";
                module.path = "<wow64-heaven-gate>";
                module.image_base = *code_base;
                module.image_base_file = *code_base;
                module.size_of_image = kCodeSize;
                module.entry_point = *code_base;
                constexpr uint16_t kMachineAmd64 = 0x8664;
                module.machine = kMachineAmd64;
                module.is_static = true;

                mapped_section section{};
                section.name = ".gate";
                section.region.start = *code_base;
                section.region.length = kCodeSize;
                section.region.permissions = memory_permission::read | memory_permission::exec;
                module.sections.emplace_back(std::move(section));

                this->modules_.emplace(module.image_base, std::move(module));
                this->last_module_cache_ = this->modules_.end();
            }
        }

        const auto stack_base = commit_with_retry(kStackBase, kStackSize, "WOW64 heaven gate stack", [&](uint64_t base) {
            std::vector<uint8_t> buffer(kStackSize, 0);
            return this->memory_->try_write_memory(base, buffer.data(), buffer.size());
        });
        if (stack_base)
        {
            this->wow64_heaven_gate_stack_top_ = *stack_base + kStackSize;
        }
    }

    void module_manager::ensure_kernelbase_nls_cache_hook(process_context& context)
    {
        if (!this->kernelbase_nls_cache_hook_registered_)
        {
            this->kernelbase_nls_cache_hook_registered_ = true;

            // Resolve kernelbase.dll's private (non-exported) gNlsProcessLocalCache global as soon as
            // it's mapped, for emulator_thread.cpp's TEB64.NlsCache setup - real Windows points every
            // thread's TEB.NlsCache at this shared, process-wide fallback structure until that thread
            // does its own locale/NLS API work, and kernelbase.dll's BaseNlsThreadCleanup
            // (DLL_THREAD_DETACH) relies on pointer equality with it to know whether to RtlFreeHeap the
            // TEB's NlsCache value.
            //
            // kernelbase.dll is never a parameter to process_context::setup() - unlike ntdll/win32u, it
            // is not mapped eagerly here; it loads later via the guest loader's own import resolution
            // (see load_native_64bit_modules/load_wow64_modules, which only map exe+ntdll+win32u
            // upfront) - so this must be resolved on_module_load, not at setup() time (a prior attempt
            // to resolve it there found kernelbase.dll never mapped yet and always fell back to the
            // placeholder, below, for every thread). By the time this fires, the only thread that
            // exists is the initial one, which is fine: it never reaches BaseNlsThreadCleanup via
            // DLL_THREAD_DETACH (that only fires for a thread exiting while others remain live - the
            // process's own final exit instead goes through DLL_PROCESS_DETACH, a separate code path
            // that never touches NlsCache at all) - every thread the guest's own code spawns afterwards
            // (i.e. any real DLL_THREAD_DETACH candidate) is created well after this callback has
            // already run, since kernelbase.dll loads during the loader's own import resolution,
            // strictly before the app's own code can call CreateThread. Its RVA is specific to the
            // exact kernelbase.dll build it was originally identified against (via static analysis) -
            // resolve_kernelbase_nls_cache_address validates it still lands on writable data before
            // trusting it, since that drifts across Windows builds. Registered only once per
            // module_manager instance - safe, since the callback captures `context` by reference and
            // every caller of this function passes the SAME process_context the module_manager was
            // constructed for.
            this->callbacks_->on_module_load.add([this, &context](mapped_module& mod) {
                if (mod.name != "kernelbase.dll")
                {
                    return;
                }

                context.kernelbase_nls_process_local_cache = resolve_kernelbase_nls_cache_address(*this->memory_, mod);
            });
        }

        // Unlike the registration above, this must run every time this function is called, not just
        // once: a windows_emulator's deserialize()/restore_snapshot() calls this AFTER resetting both
        // mod_manager's module list and process_context back to a snapshot's state, but
        // kernelbase_nls_process_local_cache is deliberately not part of process_context's own
        // serialized state (it is host-side bookkeeping meant to be re-derived, not guest state) - so
        // without this, a reset to a snapshot taken BEFORE kernelbase.dll loaded would leave
        // context.kernelbase_nls_process_local_cache holding a stale, no-longer-valid guest address
        // left over from whatever ran on this object before the reset, instead of correctly falling
        // back to 0 (which resolve_nls_cache, emulator_thread.cpp, treats as "not resolved yet, use the
        // placeholder"). Resolve from the CURRENT module list every time: if kernelbase.dll is mapped
        // right now, point at its cache global; if it isn't (e.g. right after a reset to a
        // pre-kernelbase.dll-load snapshot), reset to 0 so a thread created before the on_module_load
        // callback above fires again gets the placeholder instead of a stale/invalid address.
        const auto* kernelbase = this->find_by_name("kernelbase.dll");
        context.kernelbase_nls_process_local_cache = kernelbase ? resolve_kernelbase_nls_cache_address(*this->memory_, *kernelbase) : 0;
    }

    void module_manager::map_main_modules(x86_64_emulator& emu, const windows_path& executable_path, windows_version_manager& version,
                                          process_context& context, const logger& logger)
    {
        const auto& system_root = version.get_system_root();
        const auto system32_path = system_root / "System32";
        const auto syswow64_path = system_root / "SysWOW64";

        current_execution_mode_ = detect_execution_mode(executable_path, logger);
        context.is_wow64_process = (current_execution_mode_ == execution_mode::wow64_32bit);

        // Must happen before any module gets mapped below: a JIT-based backend (FEXCore) needs to
        // know the bitness before compiling its first block (see notify_process_bitness's doc
        // comment), and a 32-bit process's own image/ntdll32 map into the low 4GB - a range some
        // backends must steer real host allocations away from for a 64-bit process, but must NOT for
        // a 32-bit one (see reserve_host_memory_ranges's doc comment) - so the reservation has to be
        // recomputed now that the bitness is known, before either module is mapped.
        emu.notify_process_bitness(context.is_wow64_process);
        this->memory_->reset_host_memory_ranges();

        this->ensure_kernelbase_nls_cache_hook(context);

        switch (current_execution_mode_)
        {
        case execution_mode::native_64bit:
            load_native_64bit_modules(executable_path, system32_path / "ntdll.dll", system32_path / "win32u.dll", logger);
            break;

        case execution_mode::wow64_32bit:
            // Data-driven interception of the real WoW64 CPU-simulation dispatcher: unlike
            // ntdll32/win32u/native-ntdll (mapped directly by load_wow64_modules), wow64cpu.dll is
            // never mapped by sogen - real ntdll loads it dynamically through the guest loader (task
            // #27), which flows through map_module_core and fires on_module_load. When it appears,
            // register its turbo-thunk dispatcher (TurboDispatchJumpAddressStart..End - the address
            // ntdll32's Wow64Transition points at) as a gate crossing so a JIT backend intercepts the
            // 32<->64 mode switch there instead of mis-compiling that 64-bit dispatch code. A no-op on
            // native-execution backends (register_gate_crossing's default). See fex_x86_64_emulator's
            // perform_gate_crossing for the (not-yet-decodable) wow64cpu_dispatch convention.
            this->callbacks_->on_module_load.add([this, emu_ptr = &emu, &context](mapped_module& mod) {
                if (mod.name != "wow64cpu.dll")
                {
                    return;
                }
                const auto dispatch_start = mod.find_export("TurboDispatchJumpAddressStart");
                if (dispatch_start == 0)
                {
                    return;
                }
                // wow64cpu.dll layout (decoded by hand from the shipped binary, cross-checked by
                // objdump; all RVAs relative to the image base, which we recover from the
                // TurboDispatchJumpAddressStart export @ 0x17a6 so everything tracks ASLR):
                //   BTCpuSimulate (export, 0x11c0) = `L: call RunSimulatedCode; jmp L`;
                //   RunSimulatedCode (0x1650, NOT an export) = the 64->32 forward transition;
                //   TurboDispatchJumpAddressStart (export, 0x17a6) = the 64-bit turbo dispatcher tail;
                //   the WOW64SVC far-transition thunk (0x2010) = the 32->64 reverse gate.
                constexpr uint64_t turbo_dispatch_start_rva = 0x17a6;
                const auto image_base = dispatch_start - turbo_dispatch_start_rva;

                // TurboDispatchJumpAddressEnd's RVA relative to Start is NOT reliably 0x17af - 0x17a6
                // == 9 bytes across every wow64cpu.dll build (confirmed empirically: a real CI build's
                // bytes at image_base+0x17af decode as nonsense, not the documented dispatcher code,
                // and executing them corrupts guest memory). Resolve the real export address directly
                // instead of assuming a fixed offset, exactly like TurboDispatchJumpAddressStart above.
                const auto dispatch_end = mod.find_export("TurboDispatchJumpAddressEnd");
                if (dispatch_end != 0)
                {
                    emu_ptr->set_wow64_turbo_dispatch_end(dispatch_end);
                }

                // Register the forward (64->32) transition, RunSimulatedCode. Its body contains the
                // un-JIT-able `mov gs, cx`; registering the span makes a JIT backend intercept it at
                // its entry and marshal the 32-bit register block itself instead of compiling those
                // bytes. See the FEX backend's enter_wow64_32bit_from_run_simulated_code
                // (gate_crossing_kind::wow64_run_simulated_code).
                //
                // Prefer the build-independent prologue scan (see scan_run_simulated_code_entry's doc
                // comment for why the fixed RVA below drifts across wow64cpu.dll builds); fall back to
                // the hand-decoded RVA only if the scan finds no match, so an unrecognized future
                // build shape degrades to the previous behavior instead of failing outright.
                constexpr uint64_t run_simulated_code_rva = 0x1650;
                uint64_t run_simulated_code_address = image_base + run_simulated_code_rva;
                uint64_t run_simulated_code_size = turbo_dispatch_start_rva - run_simulated_code_rva;

                if (const auto scanned = scan_run_simulated_code_entry(*this->memory_, mod, dispatch_start); scanned.has_value())
                {
                    run_simulated_code_address = *scanned;
                    run_simulated_code_size = dispatch_start - *scanned;
                }

                emu_ptr->register_gate_crossing(run_simulated_code_address, static_cast<size_t>(run_simulated_code_size),
                                                x86_64_emulator::gate_crossing_kind::wow64_run_simulated_code);

                // NtContinue never returns through the normal syscall path - the real kernel restores
                // the guest's context directly, including a mode switch back to 32-bit compatibility
                // mode, without wow64.dll's usual "jmp RunSimulatedCode" return sequence ever running.
                // handle_NtContinueEx (syscalls/thread.cpp) needs a reverse-gate address to redirect a
                // WoW64 thread's 64-bit engine to when NtContinue fires mid-gate-crossing, so the
                // existing reverse-gate machinery performs the real engine flip instead of the syscall
                // handler corrupting the active (64-bit) engine's own control-flow state. Deliberately
                // NOT run_simulated_code_rva (0x1650) itself - enter_wow64_32bit_from_run_simulated_code
                // only spills the 8 nonvolatile registers and adjusts rsp by -0xA8 when src.rip equals
                // that exact address (its "true, freshly-called-from-BTCpuSimulate entry" case); a
                // NtContinue-triggered crossing is never that - it fires mid-execution of wow64.dll's
                // own real code, so re-running that one-time prologue spill scribbles over the frozen
                // 64-bit stack with whatever's currently in r12-r15/rbx/rsi/rdi/rbp and double-adjusts
                // rsp, corrupting state for every syscall dispatched afterward (confirmed locally: this
                // broke reproducibly 3/3 runs when 0x1650 was used, while the equivalent syscall-return
                // re-entry point stayed clean 3/3). 0x167f is that clean re-entry point - it falls
                // within this same registered gate range (0x1650 already extends the whole way to
                // turbo_dispatch_start_rva) but is documented as "already runs with rsp prologue-
                // adjusted and must not be double-counted", exactly matching a mid-syscall return.
                constexpr uint64_t syscall_reentry_rva = 0x167f;
                context.wow64_syscall_reentry_addr = image_base + syscall_reentry_rva;

                // Register the reverse (32->64) transition. The 32-bit ntdll syscall stub reaches the
                // WOW64SVC thunk @ 0x2010 via `call fs:[0xC0]` (Wow64Transition); the thunk does a far
                // `ljmp 0x33:...` into 64-bit mode, which the fixed-bitness 32-bit Context cannot do.
                // Registering the 32-bit portion of the thunk [0x2010, 0x2024) makes the JIT intercept
                // it, marshal the 32-bit state, and hand control to the real 64-bit TurboDispatch so
                // wow64.dll services the syscall (see enter_wow64_64bit_from_wow64svc_thunk,
                // gate_crossing_kind::wow64cpu_dispatch). NOTE: TurboDispatchJumpAddressStart itself is
                // deliberately NOT registered - that 64-bit dispatch code MUST execute to translate the
                // 32-bit service number and issue the real 64-bit syscall sogen hooks.
                constexpr uint64_t wow64svc_thunk_rva = 0x2010;
                constexpr uint64_t wow64svc_thunk_size = 0x14;
                emu_ptr->register_gate_crossing(image_base + wow64svc_thunk_rva, wow64svc_thunk_size,
                                                x86_64_emulator::gate_crossing_kind::wow64cpu_dispatch);

                // BTCpuProcessInit enables "turbo thunks" by default (it sets the flag at RVA 0x4590),
                // so BTCpuGetBopCode (RVA 0x11e0) returns the W64SVC *turbo* bop (RVA 0x6000) instead of
                // the WOW64SVC thunk (0x2010). The 32-bit syscall stub's `call fs:[0xC0]` (Wow64Transition)
                // therefore targets 0x6000, whose `ljmp 0x33:...; jmp [r15+0xf8]` is the same un-executable
                // 32-bit bitness switch. Register it as a reverse gate too, routed to the SAME handler:
                // enter_wow64_64bit_from_wow64svc_thunk is bop-agnostic - it marshals the live 32-bit
                // register file into the CONTEXT block and resumes the 64-bit TurboDispatch, never executing
                // the thunk bytes - so the reverse transition works whichever bop code BTCpuGetBopCode
                // hands the guest.
                constexpr uint64_t w64svc_turbo_bop_rva = 0x6000;
                constexpr uint64_t w64svc_turbo_bop_size = 0x10;
                emu_ptr->register_gate_crossing(image_base + w64svc_turbo_bop_rva, w64svc_turbo_bop_size,
                                                x86_64_emulator::gate_crossing_kind::wow64cpu_dispatch);

                // Real wow64.dll process init writes this same address into TEB32.WOW32Reserved (the
                // Wow64Transition field at fs:[0xC0], undocumented name but a well-established
                // reverse-engineered fact). sogen never runs whatever real guest code would normally
                // populate fs:[0xC0] (this backend synthesizes the gate crossings above instead of
                // executing wow64cpu.dll's real dispatch code), so the field stays null. Setting
                // TEB32.WOW32Reserved alone is not enough though - confirmed empirically (the syscall
                // stub's own baked-in behavior was unchanged by it): every 32-bit syscall stub reaches
                // the dispatcher via a shared indirection stub that itself does
                // `jmp dword ptr [<ntdll32's OWN cached copy of fs:[0xC0]>]`, and that copy is read
                // once during LdrpInitializeProcess, long before wow64cpu.dll loads and this callback
                // ever runs - so by the time TEB32.WOW32Reserved gets set here, the (null) value is
                // already cached and nothing re-reads the TEB. Find and write ntdll32's actual cache
                // address directly instead (see scan_wow64_transition_cache_address's doc comment) -
                // the same "write it directly rather than rely on guest code" approach
                // LdrSystemDllInitBlock below already uses. Still set TEB32.WOW32Reserved on every
                // existing thread too, in case anything else reads it directly rather than through the
                // cache - wow64cpu.dll (and this callback) only ever loads once per process, but by the
                // time it does, worker-factory threads may already exist with their own TEB32.
                const auto turbo_bop_address = static_cast<std::uint32_t>(image_base + w64svc_turbo_bop_rva);
                if (const auto cache_address = scan_wow64_transition_cache_address(*this->memory_, *this->wow64_modules_.ntdll32);
                    cache_address.has_value())
                {
                    this->memory_->write_memory(*cache_address, &turbo_bop_address, sizeof(turbo_bop_address));
                }

                for (auto& t : context.threads | std::views::values)
                {
                    if (t.teb32.has_value())
                    {
                        t.teb32->access([&](TEB32& teb32_obj) { teb32_obj.WOW32Reserved = turbo_bop_address; });
                    }
                }

                // BTCpuProcessInit also writes a standalone compatibility-mode-to-long-mode probe into
                // its own freshly-r-x'd page: a bare `jmp far 0x33:<target>` (opcode 0xEA), which real
                // hardware executes to verify the CPU can actually switch into 64-bit mode - a
                // one-time sanity check unrelated to the two dispatch mechanisms above, but exactly as
                // un-JIT-able (0xEA is undefined in 64-bit long mode). See perform_gate_crossing's
                // decode of it (gate_crossing_kind::far_jmp_bitness_switch).
                //
                // Confirmed empirically (not by hand like the RVAs above): the probe faults at
                // image_base+0x6d41. This is NOT the same as (mapped base)+0x7000 - image_base here
                // (dispatch_start - turbo_dispatch_start_rva) sits a fixed 0x2bf bytes above wow64cpu.dll's
                // real mapped base, so RVAs measured directly against the PE mapping (as first done
                // for this one) land 0x2bf too high once added to image_base instead. The other three
                // gates above don't have this problem because their RVAs were decoded by hand directly
                // against image_base's own frame of reference, not the PE mapping.
                constexpr uint64_t cpu_mode_probe_rva = 0x6d41;
                // Exactly the far jmp's own encoding length (1-byte opcode + 4-byte offset + 2-byte
                // selector) - NOT rounded up like the other gates above. The probe's target sits just
                // 9 bytes past its own start (jmp far 0x33:<9 bytes ahead>), so a padded size (0x10, as
                // first tried here) would cover the landing address too: execution would re-enter this
                // same "non-executable" gate immediately after the crossing, decode the identical bytes,
                // and cross again forever - an infinite loop rather than the crash this gate exists to
                // fix. Confirmed via a hung CI run before narrowing this to the true instruction length.
                constexpr uint64_t cpu_mode_probe_size = 0x7;
                emu_ptr->register_gate_crossing(image_base + cpu_mode_probe_rva, cpu_mode_probe_size,
                                                x86_64_emulator::gate_crossing_kind::far_jmp_bitness_switch);
            });

            load_wow64_modules(emu, executable_path, system32_path / "ntdll.dll", system32_path / "win32u.dll", syswow64_path / "ntdll.dll",
                               version, logger);
            break;

        case execution_mode::unknown:
        default:
            throw std::runtime_error("Unknown or unsupported execution mode detected");
        }
    }

    std::optional<uint64_t> module_manager::get_module_load_count_by_path(const windows_path& path)
    {
        auto local_file = std::filesystem::weakly_canonical(std::filesystem::absolute(this->file_sys_->translate(path)));

        if (auto load_count_entry = modules_load_count.find(local_file); load_count_entry != modules_load_count.end())
        {
            return load_count_entry->second;
        }

        return {};
    }

    mapped_module* module_manager::map_module(windows_path file, const logger& logger, const bool is_static, bool allow_duplicate,
                                              const uint64_t relocation_base)
    {
        auto local_file = this->file_sys_->translate(file);

        if (local_file.filename() == "win32u.dll")
        {
            return this->map_local_module(local_file, std::move(file), logger, is_static, false, relocation_base);
        }

        return this->map_local_module(local_file, std::move(file), logger, is_static, allow_duplicate, relocation_base);
    }

    mapped_module* module_manager::map_module_or_throw(const windows_path& file, const logger& logger, const bool is_static,
                                                       bool allow_duplicate)
    {
        auto* mapped_module = this->map_module(file, logger, is_static, allow_duplicate);
        if (mapped_module == nullptr)
        {
            throw std::runtime_error{"Cannot map " + file.string()};
        }

        return mapped_module;
    }

    mapped_module* module_manager::map_local_module(const std::filesystem::path& file, windows_path module_path, const logger& logger,
                                                    const bool is_static, bool allow_duplicate, const uint64_t relocation_base)
    {
        auto local_file = weakly_canonical(absolute(file));

        if (!allow_duplicate)
        {
            for (auto& mod : this->modules_ | std::views::values)
            {
                if (mod.path == local_file)
                {
                    return &mod;
                }
            }
        }

        auto detection_result = pe_architecture_detector::detect_from_file(local_file);

        return map_module_core(
            detection_result,
            [&]() {
                auto& strategy = strategy_factory_.get_strategy(detection_result.architecture);
                return strategy.map_from_file(*this->memory_, std::move(local_file), std::move(module_path), relocation_base);
            },
            logger, is_static);
    }

    mapped_module* module_manager::map_memory_module(uint64_t base_address, uint64_t image_size, windows_path module_path,
                                                     const logger& logger, bool is_static, bool allow_duplicate)
    {
        if (!allow_duplicate)
        {
            for (auto& mod : this->modules_ | std::views::values)
            {
                if (mod.image_base == base_address)
                {
                    return &mod;
                }
            }
        }

        auto detection_result = pe_architecture_detector::detect_from_memory(*this->memory_, base_address, image_size);

        return map_module_core(
            detection_result,
            [&]() {
                auto& strategy = strategy_factory_.get_strategy(detection_result.architecture);
                return strategy.map_from_memory(*this->memory_, base_address, image_size, std::move(module_path));
            },
            logger, is_static);
    }

    void module_manager::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_map(this->modules_);
        buffer.write_map(this->modules_load_count);

        buffer.write(this->executable ? this->executable->image_base : 0);
        buffer.write(this->ntdll ? this->ntdll->image_base : 0);
        buffer.write(this->win32u ? this->win32u->image_base : 0);

        // Serialize execution mode
        buffer.write(static_cast<uint32_t>(this->current_execution_mode_));

        // Serialize WOW64 module pointers
        buffer.write(this->wow64_modules_.ntdll32 ? this->wow64_modules_.ntdll32->image_base : 0);
        buffer.write(this->wow64_modules_.wow64_dll ? this->wow64_modules_.wow64_dll->image_base : 0);
        buffer.write(this->wow64_modules_.wow64win_dll ? this->wow64_modules_.wow64win_dll->image_base : 0);
    }

    void module_manager::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read_map(this->modules_);
        buffer.read_map(this->modules_load_count);
        this->last_module_cache_ = this->modules_.end();

        const auto executable_base = buffer.read<uint64_t>();
        const auto ntdll_base = buffer.read<uint64_t>();
        const auto win32u_base = buffer.read<uint64_t>();

        this->executable = executable_base ? this->find_by_address(executable_base) : nullptr;
        this->ntdll = ntdll_base ? this->find_by_address(ntdll_base) : nullptr;
        this->win32u = win32u_base ? this->find_by_address(win32u_base) : nullptr;

        // Deserialize execution mode
        this->current_execution_mode_ = static_cast<execution_mode>(buffer.read<uint32_t>());

        // Deserialize WOW64 module pointers
        const auto ntdll32_base = buffer.read<uint64_t>();
        const auto wow64_dll_base = buffer.read<uint64_t>();
        const auto wow64win_dll_base = buffer.read<uint64_t>();

        this->wow64_modules_.ntdll32 = ntdll32_base ? this->find_by_address(ntdll32_base) : nullptr;
        this->wow64_modules_.wow64_dll = wow64_dll_base ? this->find_by_address(wow64_dll_base) : nullptr;
        this->wow64_modules_.wow64win_dll = wow64win_dll_base ? this->find_by_address(wow64win_dll_base) : nullptr;
    }

    bool module_manager::unmap(const uint64_t address)
    {
        const auto mod = this->modules_.find(address);
        if (mod == this->modules_.end())
        {
            return false;
        }

        if (mod->second.is_static)
        {
            return true;
        }

        this->callbacks_->on_module_unload(mod->second);
        unmap_module(*this->memory_, mod->second);

        auto module_load_count = this->modules_load_count[mod->second.path] - 1;
        if (module_load_count == 0)
        {
            this->modules_load_count.erase(mod->second.path);
        }
        else
        {
            this->modules_load_count[mod->second.path] = module_load_count;
        }

        this->modules_.erase(mod);
        this->last_module_cache_ = this->modules_.end();

        return true;
    }

} // namespace sogen

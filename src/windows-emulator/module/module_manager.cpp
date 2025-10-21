#include "../std_include.hpp"
#include "module_manager.hpp"
#include "module_mapping.hpp"
#include "platform/win_pefile.hpp"
#include "windows-emulator/logger.hpp"
#include "../wow64_heaven_gate.hpp"

#include <serialization_helper.hpp>
#include <cinttypes>
#include <random>
#include <algorithm>
#include <vector>

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
        buffer.write(mod.name);
        buffer.write(mod.region);
    }

    static void deserialize(buffer_deserializer& buffer, mapped_section& mod)
    {
        buffer.read(mod.name);
        buffer.read(mod.region);
    }

    static void serialize(buffer_serializer& buffer, const mapped_module& mod)
    {
        buffer.write(mod.name);
        buffer.write(mod.path);

        buffer.write(mod.image_base);
        buffer.write(mod.image_base_file);
        buffer.write(mod.size_of_image);
        buffer.write(mod.entry_point);

        buffer.write_vector(mod.exports);
        buffer.write_map(mod.address_names);

        buffer.write_vector(mod.sections);

        buffer.write(mod.is_static);
    }

    static void deserialize(buffer_deserializer& buffer, mapped_module& mod)
    {
        buffer.read(mod.name);
        buffer.read(mod.path);

        buffer.read(mod.image_base);
        buffer.read(mod.image_base_file);
        buffer.read(mod.size_of_image);
        buffer.read(mod.entry_point);

        buffer.read_vector(mod.exports);
        buffer.read_map(mod.address_names);

        buffer.read_vector(mod.sections);

        buffer.read(mod.is_static);
    }
}

// PE Architecture Detector Implementation
pe_detection_result pe_architecture_detector::detect_from_file(const std::filesystem::path& file)
{
    auto variant_result = winpe::get_pe_arch(file);

    if (std::holds_alternative<std::error_code>(variant_result))
    {
        pe_detection_result result;
        result.error_message = "Failed to detect PE architecture from file: " + file.string();
        return result;
    }

    auto arch = std::get<winpe::pe_arch>(variant_result);
    pe_detection_result result;
    result.architecture = arch;
    result.suggested_mode = determine_execution_mode(arch);
    return result;
}

pe_detection_result pe_architecture_detector::detect_from_memory(uint64_t base_address, uint64_t image_size)
{
    auto variant_result = winpe::get_pe_arch(base_address, image_size);

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
mapped_module pe32_mapping_strategy::map_from_file(memory_manager& memory, std::filesystem::path file)
{
    return map_module_from_file<std::uint32_t>(memory, std::move(file));
}

mapped_module pe32_mapping_strategy::map_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                     const std::string& module_name)
{
    return map_module_from_memory<std::uint32_t>(memory, base_address, image_size, module_name);
}

// PE64 Mapping Strategy Implementation
mapped_module pe64_mapping_strategy::map_from_file(memory_manager& memory, std::filesystem::path file)
{
    return map_module_from_file<std::uint64_t>(memory, std::move(file));
}

mapped_module pe64_mapping_strategy::map_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                     const std::string& module_name)
{
    return map_module_from_memory<std::uint64_t>(memory, base_address, image_size, module_name);
}

// Mapping Strategy Factory Implementation
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

// Core mapping logic to eliminate code duplication
mapped_module* module_manager::map_module_core(const pe_detection_result& detection_result, const std::function<mapped_module()>& mapper,
                                               const logger& logger, bool is_static)
{
    if (!detection_result.is_valid())
    {
        logger.error("%s", detection_result.error_message.c_str());
        return nullptr;
    }

    try
    {
        [[maybe_unused]] auto& strategy = strategy_factory_.get_strategy(detection_result.architecture);
        mapped_module mod = mapper();
        mod.is_static = is_static;

        const auto image_base = mod.image_base;
        const auto entry = this->modules_.try_emplace(image_base, std::move(mod));

        // TODO: Patch shell32.dll entry point to prevent TLS storage issues
        // The shell32.dll module in SysWOW64 has TLS storage that fails, causing crashes
        // This is a temporary workaround until the root cause is investigated and fixed
        this->patch_shell32_entry_point_if_needed(entry.first->second);

        this->callbacks_->on_module_load(entry.first->second);
        return &entry.first->second;
    }
    catch (const std::exception& e)
    {
        logger.error("Failed to map module: %s", e.what());
        return nullptr;
    }
}

// Execution mode detection
execution_mode module_manager::detect_execution_mode(const windows_path& executable_path, const logger& logger)
{
    auto detection_result = pe_architecture_detector::detect_from_file(this->file_sys_->translate(executable_path));

    if (!detection_result.is_valid())
    {
        logger.error("Failed to detect executable architecture: %s", detection_result.error_message.c_str());
        return execution_mode::unknown;
    }

    return detection_result.suggested_mode;
}

// Native 64-bit module loading
void module_manager::load_native_64bit_modules(const windows_path& executable_path, const windows_path& ntdll_path,
                                               const windows_path& win32u_path, const logger& logger)
{
    logger.info("Loading native 64-bit modules\n");

    this->executable = this->map_module(executable_path, logger, true);
    this->ntdll = this->map_module(ntdll_path, logger, true);
    this->win32u = this->map_module(win32u_path, logger, true);
}

// WOW64 module loading (with TODO placeholders for 32-bit details)
void module_manager::load_wow64_modules(const windows_path& executable_path, const windows_path& ntdll_path,
                                        const windows_path& win32u_path, const windows_path& ntdll32_path, const logger& logger)
{
    logger.info("Loading WOW64 modules for 32-bit application\n");

    // Load 32-bit main executable
    this->executable = this->map_module(executable_path, logger, true);

    // Load 64-bit system modules for WOW64 subsystem
    this->ntdll = this->map_module(ntdll_path, logger, true);   // 64-bit ntdll
    this->win32u = this->map_module(win32u_path, logger, true); // 64-bit win32u

    // Load 32-bit ntdll module for WOW64 subsystem
    this->wow64_modules_.ntdll32 = this->map_module(ntdll32_path, logger, true); // 32-bit ntdll

    // Get original ImageBase values from PE files
    const auto ntdll32_original_imagebase = this->wow64_modules_.ntdll32->get_image_base_file();
    const auto ntdll64_original_imagebase = this->ntdll->get_image_base_file();

    if (ntdll32_original_imagebase == 0 || ntdll64_original_imagebase == 0)
    {
        logger.error("Failed to get PE ImageBase values for WOW64 setup\n");
        return;
    }

    // Set up LdrSystemDllInitBlock structure
    PS_SYSTEM_DLL_INIT_BLOCK init_block = {};
    constexpr uint64_t symtem_dll_init_block_fix_size = 0xF0; // Wine or WIN10

    // Basic structure initialization
    init_block.Size = symtem_dll_init_block_fix_size;

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
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(1, UINT32_MAX);
    init_block.RngData = dis(gen);

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

    // Write the initialized structure to the export address
    this->memory_->write_memory(ldr_init_block_addr, &init_block, symtem_dll_init_block_fix_size);

    logger.info("Successfully initialized LdrSystemDllInitBlock at 0x%" PRIx64 "\n", ldr_init_block_addr);

    // Install the WOW64 Heaven's Gate trampoline used for compat-mode -> 64-bit transitions.
    this->install_wow64_heaven_gate(logger);
}

void module_manager::install_wow64_heaven_gate(const logger& logger)
{
    using wow64::heaven_gate::kCodeBase;
    using wow64::heaven_gate::kCodeSize;
    using wow64::heaven_gate::kStackBase;
    using wow64::heaven_gate::kStackSize;
    using wow64::heaven_gate::kTrampolineBytes;

    auto allocate_or_validate = [&](uint64_t base, size_t size, memory_permission perms, const char* name) {
        if (!this->memory_->allocate_memory(base, size, perms))
        {
            const auto region = this->memory_->get_region_info(base);
            if (!region.is_reserved || region.allocation_length < size)
            {
                logger.error("Failed to allocate %s at 0x%" PRIx64 " (size 0x%zx)\n", name, base, size);
                return false;
            }
        }
        return true;
    };

    bool code_initialized = false;
    if (allocate_or_validate(kCodeBase, kCodeSize, memory_permission::read_write, "WOW64 heaven gate code"))
    {
        if (!this->memory_->protect_memory(kCodeBase, kCodeSize, nt_memory_permission(memory_permission::read_write)))
        {
            logger.error("Failed to change protection for WOW64 heaven gate code at 0x%" PRIx64 "\n", kCodeBase);
        }
        else
        {
            std::vector<uint8_t> buffer(kCodeSize, 0);
            this->memory_->write_memory(kCodeBase, buffer.data(), buffer.size());
            this->memory_->write_memory(kCodeBase, kTrampolineBytes.data(), kTrampolineBytes.size());
            this->memory_->protect_memory(kCodeBase, kCodeSize, nt_memory_permission(memory_permission::read | memory_permission::exec));
            code_initialized = true;
        }

        if (code_initialized && this->modules_.find(kCodeBase) == this->modules_.end())
        {
            mapped_module module{};
            module.name = "wow64_heaven_gate";
            module.path = "<wow64-heaven-gate>";
            module.image_base = kCodeBase;
            module.image_base_file = kCodeBase;
            module.size_of_image = kCodeSize;
            module.entry_point = kCodeBase;
            constexpr uint16_t kMachineAmd64 = 0x8664;
            module.machine = kMachineAmd64;
            module.is_static = true;

            mapped_section section{};
            section.name = ".gate";
            section.region.start = kCodeBase;
            section.region.length = kCodeSize;
            section.region.permissions = memory_permission::read | memory_permission::exec;
            module.sections.emplace_back(std::move(section));

            this->modules_.emplace(module.image_base, std::move(module));
        }
    }

    if (allocate_or_validate(kStackBase, kStackSize, memory_permission::read_write, "WOW64 heaven gate stack"))
    {
        std::vector<uint8_t> buffer(kStackSize, 0);
        this->memory_->write_memory(kStackBase, buffer.data(), buffer.size());
    }
}

// Refactored map_main_modules with execution mode detection
void module_manager::map_main_modules(const windows_path& executable_path, const windows_path& system32_path,
                                      const windows_path& syswow64_path, const logger& logger)
{
    // Detect execution mode based on executable architecture
    current_execution_mode_ = detect_execution_mode(executable_path, logger);

    // Load modules based on detected execution mode
    switch (current_execution_mode_)
    {
    case execution_mode::native_64bit:
        load_native_64bit_modules(executable_path, system32_path / "ntdll.dll", system32_path / "win32u.dll", logger);
        break;

    case execution_mode::wow64_32bit:
        load_wow64_modules(executable_path, system32_path / "ntdll.dll", system32_path / "win32u.dll", syswow64_path / "ntdll.dll", logger);
        break;

    case execution_mode::unknown:
    default:
        throw std::runtime_error("Unknown or unsupported execution mode detected");
    }
}

mapped_module* module_manager::map_module(const windows_path& file, const logger& logger, const bool is_static)
{
    return this->map_local_module(this->file_sys_->translate(file), logger, is_static);
}

// Refactored map_local_module using the new architecture
mapped_module* module_manager::map_local_module(const std::filesystem::path& file, const logger& logger, const bool is_static)
{
    auto local_file = weakly_canonical(absolute(file));

    // Check if module is already loaded
    for (auto& mod : this->modules_ | std::views::values)
    {
        if (mod.path == local_file)
        {
            return &mod;
        }
    }

    // Detect PE architecture
    auto detection_result = pe_architecture_detector::detect_from_file(local_file);

    // Use core mapping logic to eliminate code duplication
    return map_module_core(
        detection_result,
        [&]() {
            auto& strategy = strategy_factory_.get_strategy(detection_result.architecture);
            return strategy.map_from_file(*this->memory_, std::move(local_file));
        },
        logger, is_static);
}

// Refactored map_memory_module using the new architecture
mapped_module* module_manager::map_memory_module(uint64_t base_address, uint64_t image_size, const std::string& module_name,
                                                 const logger& logger, bool is_static)
{
    // Check if module is already loaded at this address
    for (auto& mod : this->modules_ | std::views::values)
    {
        if (mod.image_base == base_address)
        {
            return &mod;
        }
    }

    // Detect PE architecture from memory
    auto detection_result = pe_architecture_detector::detect_from_memory(base_address, image_size);

    // Use core mapping logic to eliminate code duplication
    return map_module_core(
        detection_result,
        [&]() {
            auto& strategy = strategy_factory_.get_strategy(detection_result.architecture);
            return strategy.map_from_memory(*this->memory_, base_address, image_size, module_name);
        },
        logger, is_static);
}

void module_manager::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write_map(this->modules_);

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
    this->modules_.erase(mod);

    return true;
}

void module_manager::patch_shell32_entry_point_if_needed(mapped_module& mod)
{
    // Only patch shell32.dll in SysWOW64 directory (32-bit)
    // Convert module name to lowercase for case-insensitive comparison
    std::string module_name_lower = mod.name;
    std::transform(module_name_lower.begin(), module_name_lower.end(), module_name_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (module_name_lower != "shell32.dll")
    {
        return;
    }

    // Check if this is the SysWOW64 version by examining if it's a 32-bit module
    // Convert path to lowercase for case-insensitive comparison
    std::string path_str = mod.path.string();
    std::transform(path_str.begin(), path_str.end(), path_str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (path_str.find("syswow64") == std::string::npos)
    {
        return;
    }

    if (mod.entry_point == 0)
    {
        return;
    }

    // Get the page containing the entry point
    const auto entry_page_start = mod.entry_point & ~0xFFFULL;
    const auto page_size = 0x1000;

    // Temporarily change memory protection to writable
    nt_memory_permission mem_permisson(memory_permission::none);
    if (!this->memory_->protect_memory(entry_page_start, page_size, memory_permission::all, &mem_permisson))
    {
        return; // Failed to change protection
    }

    // Write the ret 0Ch instruction at the entry point (0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x0C, 0x00)
    // This makes DllMain return immediately without executing CRT startup
    constexpr std::array<uint8_t, 8> patch_bytes = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x0C, 0x00}; // mov eax, 1 && ret 0Ch
    this->memory_->write_memory(mod.entry_point, patch_bytes.data(), patch_bytes.size());

    // Restore the original memory protection
    this->memory_->protect_memory(entry_page_start, page_size, mem_permisson, nullptr);
}

#include "../std_include.hpp"
#include "module_manager.hpp"
#include "module_mapping.hpp"
#include "platform/win_pefile.hpp"
#include "windows-emulator/logger.hpp"
#include "../wow64_heaven_gate.hpp"
#include "../version/windows_version_manager.hpp"
#include "../process_context.hpp"

#include <serialization_helper.hpp>
#include <cinttypes>
#include <vector>

namespace
{
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

        buffer.write(mod.machine);
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

        buffer.read(mod.image_base);
        buffer.read(mod.image_base_file);
        buffer.read(mod.size_of_image);
        buffer.read(mod.entry_point);

        buffer.read(mod.machine);
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
mapped_module pe32_mapping_strategy::map_from_file(memory_manager& memory, std::filesystem::path file, windows_path module_path)
{
    return map_module_from_file<std::uint32_t>(memory, std::move(file), std::move(module_path));
}

mapped_module pe32_mapping_strategy::map_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                     windows_path module_path)
{
    return map_module_from_memory<std::uint32_t>(memory, base_address, image_size, std::move(module_path));
}

// PE64 Mapping Strategy Implementation
mapped_module pe64_mapping_strategy::map_from_file(memory_manager& memory, std::filesystem::path file, windows_path module_path)
{
    return map_module_from_file<std::uint64_t>(memory, std::move(file), std::move(module_path));
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
        logger.error("Failed to map module: %s", e.what());
        return nullptr;
    }
}

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

void module_manager::load_native_64bit_modules(const windows_path& executable_path, const windows_path& ntdll_path,
                                               const windows_path& win32u_path, const logger& logger)
{
    this->executable = this->map_module(executable_path, logger, true);
    this->ntdll = this->map_module(ntdll_path, logger, true);
    this->win32u = this->map_module(win32u_path, logger, true);
}

void module_manager::load_wow64_modules(const windows_path& executable_path, const windows_path& ntdll_path,
                                        const windows_path& win32u_path, const windows_path& ntdll32_path, windows_version_manager& version,
                                        const logger& logger)
{
    logger.info("Loading WOW64 modules for 32-bit application\n");

    this->executable = this->map_module(executable_path, logger, true);
    this->ntdll = this->map_module(ntdll_path, logger, true);
    this->win32u = this->map_module(win32u_path, logger, true);

    this->wow64_modules_.ntdll32 = this->map_module(ntdll32_path, logger, true);

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
            this->last_module_cache_ = this->modules_.end();
        }
    }

    if (allocate_or_validate(kStackBase, kStackSize, memory_permission::read_write, "WOW64 heaven gate stack"))
    {
        std::vector<uint8_t> buffer(kStackSize, 0);
        this->memory_->write_memory(kStackBase, buffer.data(), buffer.size());
    }
}

void module_manager::map_main_modules(const windows_path& executable_path, windows_version_manager& version, process_context& context,
                                      const logger& logger)
{
    const auto& system_root = version.get_system_root();
    const auto system32_path = system_root / "System32";
    const auto syswow64_path = system_root / "SysWOW64";

    current_execution_mode_ = detect_execution_mode(executable_path, logger);
    context.is_wow64_process = (current_execution_mode_ == execution_mode::wow64_32bit);

    switch (current_execution_mode_)
    {
    case execution_mode::native_64bit:
        load_native_64bit_modules(executable_path, system32_path / "ntdll.dll", system32_path / "win32u.dll", logger);
        break;

    case execution_mode::wow64_32bit:
        load_wow64_modules(executable_path, system32_path / "ntdll.dll", system32_path / "win32u.dll", syswow64_path / "ntdll.dll", version,
                           logger);
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

mapped_module* module_manager::map_module(windows_path file, const logger& logger, const bool is_static, bool allow_duplicate)
{
    auto local_file = this->file_sys_->translate(file);

    if (local_file.filename() == "win32u.dll")
    {
        return this->map_local_module(local_file, std::move(file), logger, is_static, false);
    }

    return this->map_local_module(local_file, std::move(file), logger, is_static, allow_duplicate);
}

mapped_module* module_manager::map_local_module(const std::filesystem::path& file, windows_path module_path, const logger& logger,
                                                const bool is_static, bool allow_duplicate)
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
            return strategy.map_from_file(*this->memory_, std::move(local_file), std::move(module_path));
        },
        logger, is_static);
}

mapped_module* module_manager::map_memory_module(uint64_t base_address, uint64_t image_size, windows_path module_path, const logger& logger,
                                                 bool is_static, bool allow_duplicate)
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

    auto detection_result = pe_architecture_detector::detect_from_memory(base_address, image_size);

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

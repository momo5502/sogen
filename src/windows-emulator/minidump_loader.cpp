#include "std_include.hpp"
#include "minidump_loader.hpp"
#include "windows_emulator.hpp"
#include "windows_objects.hpp"
#include "emulator_thread.hpp"
#include "common/platform/unicode.hpp"
#include "common/platform/kernel_mapped.hpp"
#include "memory_utils.hpp"

#include <minidump/minidump.hpp>

namespace minidump_loader
{
    struct dump_statistics
    {
        size_t thread_count = 0;
        size_t module_count = 0;
        size_t memory_region_count = 0;
        size_t memory_segment_count = 0;
        size_t handle_count = 0;
        uint64_t total_memory_size = 0;
        bool has_exception = false;
        bool has_system_info = false;
    };

    std::string get_architecture_string(const minidump::minidump_file* dump_file)
    {
        if (!dump_file)
        {
            return "Unknown";
        }

        const auto* sys_info = dump_file->get_system_info();
        if (!sys_info)
        {
            return "Unknown";
        }

        const auto arch = static_cast<minidump::processor_architecture>(sys_info->processor_architecture);
        switch (arch)
        {
        case minidump::processor_architecture::amd64:
            return "x64 (AMD64)";
        case minidump::processor_architecture::intel:
            return "x86 (Intel)";
        case minidump::processor_architecture::arm64:
            return "ARM64";
        default:
            return "Unknown (" + std::to_string(static_cast<int>(arch)) + ")";
        }
    }

    bool parse_minidump_file(windows_emulator& win_emu, const std::filesystem::path& minidump_path,
                             std::unique_ptr<minidump::minidump_file>& dump_file, std::unique_ptr<minidump::minidump_reader>& dump_reader)
    {
        win_emu.log.info("Parsing minidump file\n");

        if (!std::filesystem::exists(minidump_path))
        {
            win_emu.log.error("Minidump file does not exist: %s\n", minidump_path.string().c_str());
            return false;
        }

        const auto file_size = std::filesystem::file_size(minidump_path);
        win_emu.log.info("File size: %ju bytes\n", file_size);

        auto parsed_file = minidump::minidump_file::parse(minidump_path.string());
        if (!parsed_file)
        {
            win_emu.log.error("Failed to parse minidump file\n");
            return false;
        }

        win_emu.log.info("Minidump header parsed successfully\n");

        auto reader = parsed_file->get_reader();
        if (!reader)
        {
            win_emu.log.error("Failed to create minidump reader\n");
            return false;
        }

        dump_file = std::move(parsed_file);
        dump_reader = std::move(reader);

        win_emu.log.info("Minidump reader created successfully\n");
        return true;
    }

    bool validate_dump_compatibility(windows_emulator& win_emu, const minidump::minidump_file* dump_file)
    {
        win_emu.log.info("Validating dump compatibility\n");

        if (!dump_file)
        {
            win_emu.log.error("Dump file not loaded\n");
            return false;
        }

        const auto& header = dump_file->header();

        if (!header.is_valid())
        {
            win_emu.log.error("Invalid minidump signature or header\n");
            return false;
        }

        win_emu.log.info("Minidump signature: 0x%08X (valid)\n", header.signature);
        win_emu.log.info("Version: %u.%u\n", header.version, header.implementation_version);
        win_emu.log.info("Number of streams: %u\n", header.number_of_streams);
        win_emu.log.info("Flags: 0x%016" PRIx64 "\n", header.flags);

        const auto* sys_info = dump_file->get_system_info();
        if (sys_info)
        {
            const auto arch = static_cast<minidump::processor_architecture>(sys_info->processor_architecture);
            const bool is_x64 = (arch == minidump::processor_architecture::amd64);

            win_emu.log.info("Processor architecture: %s\n", get_architecture_string(dump_file).c_str());

            if (!is_x64)
            {
                win_emu.log.error("Only x64 minidumps are currently supported\n");
                return false;
            }

            win_emu.log.info("Architecture compatibility: OK (x64)\n");
        }
        else
        {
            win_emu.log.warn("No system info stream found - proceeding with caution\n");
        }

        return true;
    }

    void log_dump_summary(windows_emulator& win_emu, const minidump::minidump_file* dump_file, dump_statistics& stats)
    {
        win_emu.log.info("Generating dump summary\n");

        stats = {};

        if (!dump_file)
        {
            win_emu.log.error("Dump file not loaded\n");
            return;
        }

        stats.thread_count = dump_file->threads().size();
        stats.module_count = dump_file->modules().size();
        stats.memory_region_count = dump_file->memory_regions().size();
        stats.memory_segment_count = dump_file->memory_segments().size();
        stats.handle_count = dump_file->handles().size();
        stats.has_exception = (dump_file->get_exception_info() != nullptr);
        stats.has_system_info = (dump_file->get_system_info() != nullptr);

        for (const auto& segment : dump_file->memory_segments())
        {
            stats.total_memory_size += segment.size;
        }

        win_emu.log.info("Summary: %s, %zu threads, %zu modules, %zu regions, %zu segments, %zu handles, %" PRIu64 " bytes memory\n",
                         get_architecture_string(dump_file).c_str(), stats.thread_count, stats.module_count, stats.memory_region_count,
                         stats.memory_segment_count, stats.handle_count, stats.total_memory_size);
    }

    void process_streams(windows_emulator& win_emu, const minidump::minidump_file* dump_file)
    {
        if (!dump_file)
        {
            return;
        }

        // Process system info
        const auto* sys_info = dump_file->get_system_info();
        if (sys_info)
        {
            win_emu.log.info("System: OS %u.%u.%u, %u processors, type %u, platform %u\n", sys_info->major_version, sys_info->minor_version,
                             sys_info->build_number, sys_info->number_of_processors, sys_info->product_type, sys_info->platform_id);
        }

        // Process memory info
        const auto& memory_regions = dump_file->memory_regions();
        uint64_t total_reserved = 0;
        uint64_t total_committed = 0;
        size_t guard_pages = 0;
        for (const auto& region : memory_regions)
        {
            total_reserved += region.region_size;
            if (region.state & MEM_COMMIT)
            {
                total_committed += region.region_size;
            }
            if (region.protect & PAGE_GUARD)
            {
                guard_pages++;
            }
        }
        win_emu.log.info("Memory: %zu regions, %" PRIu64 " bytes reserved, %" PRIu64 " bytes committed, %zu guard pages\n",
                         memory_regions.size(), total_reserved, total_committed, guard_pages);

        // Process memory content
        const auto& memory_segments = dump_file->memory_segments();
        uint64_t min_addr = UINT64_MAX;
        uint64_t max_addr = 0;
        for (const auto& segment : memory_segments)
        {
            min_addr = std::min(min_addr, segment.start_virtual_address);
            max_addr = std::max(max_addr, segment.end_virtual_address());
        }
        if (!memory_segments.empty())
        {
            win_emu.log.info("Content: %zu segments, range 0x%" PRIx64 "-0x%" PRIx64 " (%" PRIu64 " bytes span)\n", memory_segments.size(),
                             min_addr, max_addr, max_addr - min_addr);
        }

        // Process modules
        const auto& modules = dump_file->modules();
        for (const auto& mod : modules)
        {
            win_emu.log.info("Module: %s at 0x%" PRIx64 " (%u bytes)\n", mod.module_name.c_str(), mod.base_of_image, mod.size_of_image);
        }

        // Process threads
        const auto& threads = dump_file->threads();
        for (const auto& thread : threads)
        {
            win_emu.log.info("Thread %u: TEB 0x%" PRIx64 ", stack 0x%" PRIx64 " (%u bytes), context %u bytes\n", thread.thread_id,
                             thread.teb, thread.stack_start_of_memory_range, thread.stack_data_size, thread.context_data_size);
        }

        // Process handles
        const auto& handles = dump_file->handles();
        if (!handles.empty())
        {
            std::map<std::string, size_t> handle_type_counts;
            for (const auto& handle : handles)
            {
                handle_type_counts[handle.type_name]++;
            }
            win_emu.log.info("Handles: %zu total\n", handles.size());
            for (const auto& [type, count] : handle_type_counts)
            {
                win_emu.log.info("  %s: %zu\n", type.c_str(), count);
            }
        }

        // Process exception info
        const auto* exception = dump_file->get_exception_info();
        if (exception)
        {
            win_emu.log.info("Exception: thread %u, code 0x%08X at 0x%" PRIx64 "\n", exception->thread_id,
                             exception->exception_record.exception_code, exception->exception_record.exception_address);
        }
    }

    void reconstruct_memory_state(windows_emulator& win_emu, const minidump::minidump_file* dump_file,
                                  minidump::minidump_reader* dump_reader)
    {
        if (!dump_file || !dump_reader)
        {
            win_emu.log.error("Dump file or reader not loaded\n");
            return;
        }

        const auto& memory_regions = dump_file->memory_regions();
        const auto& memory_segments = dump_file->memory_segments();

        win_emu.log.info("Reconstructing memory: %zu regions, %zu data segments\n", memory_regions.size(), memory_segments.size());
        size_t reserved_count = 0;
        size_t committed_count = 0;
        size_t failed_count = 0;

        for (const auto& region : memory_regions)
        {
            // Log the memory region details
            win_emu.log.info("Region: 0x%" PRIx64 ", size=%" PRIu64 ", state=0x%08X, protect=0x%08X\n", region.base_address,
                             region.region_size, region.state, region.protect);

            const bool is_reserved = (region.state & MEM_RESERVE) != 0;
            const bool is_committed = (region.state & MEM_COMMIT) != 0;
            const bool is_free = (region.state & MEM_FREE) != 0;

            if (is_free)
            {
                continue;
            }

            auto protect_value = region.protect;
            if (protect_value == 0)
            {
                protect_value = PAGE_READONLY;
                win_emu.log.warn("  Region 0x%" PRIx64 " has zero protection, using PAGE_READONLY\n", region.base_address);
            }

            memory_permission perms = map_nt_to_emulator_protection(protect_value);

            try
            {
                if (is_committed)
                {
                    if (win_emu.memory.allocate_memory(region.base_address, static_cast<size_t>(region.region_size), perms, false))
                    {
                        committed_count++;
                        win_emu.log.info("  Allocated committed 0x%" PRIx64 ": size=%" PRIu64 ", state=0x%08X, protect=0x%08X\n",
                                         region.base_address, region.region_size, region.state, region.protect);
                    }
                    else
                    {
                        failed_count++;
                        win_emu.log.warn("  Failed to allocate committed 0x%" PRIx64 ": size=%" PRIu64 "\n", region.base_address,
                                         region.region_size);
                    }
                }
                else if (is_reserved)
                {
                    if (win_emu.memory.allocate_memory(region.base_address, static_cast<size_t>(region.region_size), perms, true))
                    {
                        reserved_count++;
                        win_emu.log.info("  Reserved 0x%" PRIx64 ": size=%" PRIu64 ", state=0x%08X, protect=0x%08X\n", region.base_address,
                                         region.region_size, region.state, region.protect);
                    }
                    else
                    {
                        failed_count++;
                        win_emu.log.warn("  Failed to reserve 0x%" PRIx64 ": size=%" PRIu64 "\n", region.base_address, region.region_size);
                    }
                }
            }
            catch (const std::exception& e)
            {
                failed_count++;
                win_emu.log.error("  Exception allocating 0x%" PRIx64 ": %s\n", region.base_address, e.what());
            }
        }

        win_emu.log.info("Regions: %zu reserved, %zu committed, %zu failed\n", reserved_count, committed_count, failed_count);
        size_t written_count = 0;
        size_t write_failed_count = 0;
        uint64_t total_bytes_written = 0;

        for (const auto& segment : memory_segments)
        {
            try
            {
                auto memory_data = dump_reader->read_memory(segment.start_virtual_address, static_cast<size_t>(segment.size));
                win_emu.memory.write_memory(segment.start_virtual_address, memory_data.data(), static_cast<size_t>(memory_data.size()));
                written_count++;
                total_bytes_written += memory_data.size();
                win_emu.log.info("  Written segment 0x%" PRIx64 ": %zu bytes\n", segment.start_virtual_address, memory_data.size());
            }
            catch (const std::exception& e)
            {
                write_failed_count++;
                win_emu.log.error("  Failed to write segment 0x%" PRIx64 ": %s\n", segment.start_virtual_address, e.what());
            }
        }

        win_emu.log.info("Content: %zu segments written (%" PRIu64 " bytes), %zu failed\n", written_count, total_bytes_written,
                         write_failed_count);
    }

    bool is_main_executable(const minidump::module_info& mod)
    {
        const auto name = mod.module_name;
        return name.find(".exe") != std::string::npos;
    }

    bool is_ntdll(const minidump::module_info& mod)
    {
        const auto name = mod.module_name;
        return name == "ntdll.dll" || name == "NTDLL.DLL";
    }

    bool is_win32u(const minidump::module_info& mod)
    {
        const auto name = mod.module_name;
        return name == "win32u.dll" || name == "WIN32U.DLL";
    }

    void reconstruct_module_state(windows_emulator& win_emu, const minidump::minidump_file* dump_file)
    {
        if (!dump_file)
        {
            win_emu.log.error("Dump file not loaded\n");
            return;
        }

        const auto& modules = dump_file->modules();
        win_emu.log.info("Reconstructing module state: %zu modules\n", modules.size());

        size_t mapped_count = 0;
        size_t failed_count = 0;
        size_t identified_count = 0;

        for (const auto& mod : modules)
        {
            try
            {
                auto* mapped_module =
                    win_emu.mod_manager.map_memory_module(mod.base_of_image, mod.size_of_image, mod.module_name, win_emu.log);

                if (mapped_module)
                {
                    mapped_count++;
                    win_emu.log.info("  Mapped %s at 0x%" PRIx64 " (%u bytes, %zu sections, %zu exports)\n", mod.module_name.c_str(),
                                     mod.base_of_image, mod.size_of_image, mapped_module->sections.size(), mapped_module->exports.size());

                    if (is_main_executable(mod))
                    {
                        win_emu.mod_manager.executable = mapped_module;
                        identified_count++;
                        win_emu.log.info("    Identified as main executable\n");
                    }
                    else if (is_ntdll(mod))
                    {
                        win_emu.mod_manager.ntdll = mapped_module;
                        identified_count++;
                        win_emu.log.info("    Identified as ntdll\n");
                    }
                    else if (is_win32u(mod))
                    {
                        win_emu.mod_manager.win32u = mapped_module;
                        identified_count++;
                        win_emu.log.info("    Identified as win32u\n");
                    }
                }
                else
                {
                    failed_count++;
                    win_emu.log.warn("  Failed to map %s at 0x%" PRIx64 "\n", mod.module_name.c_str(), mod.base_of_image);
                }
            }
            catch (const std::exception& e)
            {
                failed_count++;
                win_emu.log.error("  Exception mapping %s: %s\n", mod.module_name.c_str(), e.what());
            }
        }

        win_emu.log.info("Module reconstruction: %zu mapped, %zu failed, %zu system modules identified\n", mapped_count, failed_count,
                         identified_count);
    }

    void setup_kusd_from_dump(windows_emulator& win_emu, const minidump::minidump_file* dump_file)
    {
        const auto* sys_info = dump_file->get_system_info();
        if (!sys_info)
        {
            win_emu.log.warn("No system info available - using default KUSD\n");
            return;
        }

        win_emu.log.info("Setting up KUSER_SHARED_DATA from dump system info\n");

        auto& kusd = win_emu.process.kusd.get();
        kusd.NtMajorVersion = sys_info->major_version;
        kusd.NtMinorVersion = sys_info->minor_version;
        kusd.NtBuildNumber = sys_info->build_number;
        kusd.NativeProcessorArchitecture = sys_info->processor_architecture;
        kusd.ActiveProcessorCount = sys_info->number_of_processors;
        kusd.UnparkedProcessorCount = sys_info->number_of_processors;
        kusd.NtProductType = static_cast<NT_PRODUCT_TYPE>(sys_info->product_type);
        kusd.ProductTypeIsValid = 1;

        win_emu.log.info("KUSD updated: Windows %u.%u.%u, %u processors, product type %u\n", sys_info->major_version,
                         sys_info->minor_version, sys_info->build_number, sys_info->number_of_processors, sys_info->product_type);
    }

    bool load_thread_context(const std::filesystem::path& minidump_path, const minidump::thread_info& thread_info,
                             std::vector<std::byte>& context_buffer)
    {
        if (thread_info.context_data_size == 0)
        {
            return false;
        }

        std::ifstream context_file(minidump_path, std::ios::binary);
        if (!context_file.is_open())
        {
            return false;
        }

        context_file.seekg(thread_info.context_rva);
        context_buffer.resize(thread_info.context_data_size);
        context_file.read(reinterpret_cast<char*>(context_buffer.data()), thread_info.context_data_size);

        return context_file.good();
    }

    void reconstruct_threads(windows_emulator& win_emu, const minidump::minidump_file* dump_file,
                             const std::filesystem::path& minidump_path)
    {
        const auto& threads = dump_file->threads();
        if (threads.empty())
        {
            win_emu.log.warn("No threads found in minidump\n");
            return;
        }

        win_emu.log.info("Reconstructing threads: %zu threads\n", threads.size());

        size_t success_count = 0;
        size_t context_loaded_count = 0;

        for (const auto& thread_info : threads)
        {
            try
            {
                emulator_thread thread(win_emu.memory);
                thread.id = thread_info.thread_id;
                thread.stack_base = thread_info.stack_start_of_memory_range;
                thread.stack_size = thread_info.stack_data_size;

                // Load CPU context if available
                const bool context_loaded = load_thread_context(minidump_path, thread_info, thread.last_registers);
                if (context_loaded)
                {
                    context_loaded_count++;
                }

                // Set TEB address if valid
                if (thread_info.teb != 0)
                {
                    thread.teb = emulator_object<TEB64>(win_emu.memory);
                    thread.teb->set_address(thread_info.teb);
                }

                win_emu.log.info("  Thread %u: TEB=0x%" PRIx64 ", stack=0x%" PRIx64 " (%u bytes), context=%s\n", thread_info.thread_id,
                                 thread_info.teb, thread.stack_base, thread_info.stack_data_size,
                                 context_loaded ? "loaded" : "unavailable");

                win_emu.process.threads.store(std::move(thread));
                success_count++;
            }
            catch (const std::exception& e)
            {
                win_emu.log.error("  Failed to reconstruct thread %u: %s\n", thread_info.thread_id, e.what());
            }
        }

        // Set active thread to first available thread
        if (success_count > 0)
        {
            auto& first_thread = win_emu.process.threads.begin()->second;
            win_emu.process.active_thread = &first_thread;
        }

        win_emu.log.info("Thread reconstruction: %zu/%zu threads created, %zu with context\n", success_count, threads.size(),
                         context_loaded_count);
    }

    void setup_peb_from_teb(windows_emulator& win_emu, const minidump::minidump_file* dump_file)
    {
        const auto& threads = dump_file->threads();
        if (threads.empty())
        {
            win_emu.log.warn("No threads available for PEB setup\n");
            return;
        }

        const auto& first_thread = threads[0];
        if (first_thread.teb == 0)
        {
            win_emu.log.warn("Thread %u has null TEB address\n", first_thread.thread_id);
            return;
        }

        try
        {
            constexpr uint64_t teb_peb_offset = offsetof(TEB64, ProcessEnvironmentBlock);
            uint64_t peb_address = 0;

            win_emu.memory.read_memory(first_thread.teb + teb_peb_offset, &peb_address, sizeof(peb_address));

            if (peb_address == 0)
            {
                win_emu.log.warn("PEB address is null in TEB at 0x%" PRIx64 "\n", first_thread.teb);
                return;
            }

            win_emu.process.peb.set_address(peb_address);
            win_emu.log.info("PEB address: 0x%" PRIx64 " (from TEB 0x%" PRIx64 ")\n", peb_address, first_thread.teb);
        }
        catch (const std::exception& e)
        {
            win_emu.log.error("Failed to read PEB from TEB: %s\n", e.what());
        }
    }

    void reconstruct_handle_table(windows_emulator& win_emu, const minidump::minidump_file* dump_file)
    {
        const auto& handles = dump_file->handles();
        if (handles.empty())
        {
            return;
        }

        win_emu.log.info("Reconstructing handle table: %zu handles\n", handles.size());

        std::map<std::string, size_t> handle_type_counts;
        size_t created_count = 0;

        for (const auto& handle_info : handles)
        {
            handle_type_counts[handle_info.type_name]++;

            try
            {
                if (handle_info.type_name == "Event")
                {
                    event evt{};
                    evt.name = u8_to_u16(handle_info.object_name);
                    win_emu.process.events.store(std::move(evt));
                    created_count++;
                }
                else if (handle_info.type_name == "File")
                {
                    file f{};
                    f.name = u8_to_u16(handle_info.object_name);
                    win_emu.process.files.store(std::move(f));
                    created_count++;
                }
                else if (handle_info.type_name == "Mutant")
                {
                    mutant m{};
                    m.name = u8_to_u16(handle_info.object_name);
                    win_emu.process.mutants.store(std::move(m));
                    created_count++;
                }
                // Other handle types can be added here as needed
            }
            catch (const std::exception& e)
            {
                win_emu.log.error("  Failed to create %s handle '%s': %s\n", handle_info.type_name.c_str(), handle_info.object_name.c_str(),
                                  e.what());
            }
        }

        // Log summary by type
        for (const auto& [type, count] : handle_type_counts)
        {
            win_emu.log.info("  %s: %zu handles\n", type.c_str(), count);
        }

        win_emu.log.info("Handle table: %zu/%zu handles reconstructed\n", created_count, handles.size());
    }

    void setup_exception_context(windows_emulator& win_emu, const minidump::minidump_file* dump_file)
    {
        const auto* exception_info = dump_file->get_exception_info();
        if (!exception_info)
        {
            return;
        }

        win_emu.log.info("Exception context: address=0x%" PRIx64 ", code=0x%08X, thread=%u\n",
                         exception_info->exception_record.exception_address, exception_info->exception_record.exception_code,
                         exception_info->thread_id);
    }

    void load_minidump_into_emulator(windows_emulator& win_emu, const std::filesystem::path& minidump_path)
    {
        win_emu.log.info("Starting minidump loading process\n");
        win_emu.log.info("Minidump file: %s\n", minidump_path.string().c_str());

        try
        {
            std::unique_ptr<minidump::minidump_file> dump_file;
            std::unique_ptr<minidump::minidump_reader> dump_reader;

            if (!parse_minidump_file(win_emu, minidump_path, dump_file, dump_reader))
            {
                throw std::runtime_error("Failed to parse minidump file");
            }

            if (!validate_dump_compatibility(win_emu, dump_file.get()))
            {
                throw std::runtime_error("Minidump compatibility validation failed");
            }

            setup_kusd_from_dump(win_emu, dump_file.get());

            dump_statistics stats;
            log_dump_summary(win_emu, dump_file.get(), stats);
            process_streams(win_emu, dump_file.get());

            // Existing phases
            reconstruct_memory_state(win_emu, dump_file.get(), dump_reader.get());
            reconstruct_module_state(win_emu, dump_file.get());

            // Process state reconstruction phases
            setup_peb_from_teb(win_emu, dump_file.get());
            reconstruct_threads(win_emu, dump_file.get(), minidump_path);
            reconstruct_handle_table(win_emu, dump_file.get());
            setup_exception_context(win_emu, dump_file.get());

            win_emu.log.info("Process state reconstruction completed\n");
        }
        catch (const std::exception& e)
        {
            win_emu.log.error("Minidump loading failed: %s\n", e.what());
            throw;
        }
    }
} // namespace minidump_loader

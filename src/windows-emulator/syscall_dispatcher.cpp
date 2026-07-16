#include "std_include.hpp"
#include "syscall_dispatcher.hpp"
#include "syscall_utils.hpp"

#include <utils/string.hpp>

namespace sogen
{
    namespace
    {
        // Real ntdll's RtlpInitCodePageTables (called once, early, from LdrpInitializeNlsInfo) is
        // supposed to always leave this internal global (real ntdll's own "GlobalRtlNlsState +
        // 0x90"-offset field, consumed directly by RtlConsoleMultiByteToUnicodeN/
        // RtlAnsiCharToUnicodeChar/RtlIsTextUnicode/etc. as a lead-byte-info table pointer) pointing
        // at a valid table - either a real DBCS lead-byte table, or (for the common single-byte-
        // codepage case, matching sogen's shipped 1252/437 codepages) ntdll's own static, empty
        // NlsEmptyLeadByteInfoTable global. Confirmed via extensive idasql/Hex-Rays tracing this
        // session: for a wow64-flagged process, this field is still exactly null by the time guest
        // execution reaches its first syscalls, even though the sibling codepage-table fields
        // (GlobalRtlNlsState/word_180172710) are correctly populated with the real 1252/437 codepage
        // data at that same point - i.e. RtlpInitCodePageTables's own success path is reached but this
        // one specific field ends up unset for wow64 specifically (root mechanism not pinned down
        // despite deep tracing; a real hardware debugger or a re-armable write-trap would be needed to
        // go further - both established elsewhere in this project as currently unavailable/blocked).
        // Left null, any guest read of this table (`*(WORD*)(table + 2*byte)`, no null-check in most
        // consumers) computes a near-null address and crashes - the long-investigated
        // "Null-pointer call to 0x50"/(null)-substitution signature. Populating it here with a
        // harmless, always-empty (all-zero) lead-byte table is semantically identical to what real
        // ntdll provides for the single-byte-codepage case (its own NlsEmptyLeadByteInfoTable is
        // exactly an empty/no-lead-bytes table), so this is a safe, real fix for this specific
        // internal ntdll field regardless of the root mechanism.
        constexpr uint64_t nls_lead_byte_info_table_offset = 0x172760;

        void ensure_nls_lead_byte_info_table(windows_emulator& win_emu)
        {
            const auto* ntdll_mod = win_emu.mod_manager.ntdll;
            if (ntdll_mod == nullptr)
            {
                return;
            }

            const auto field_address = ntdll_mod->image_base + nls_lead_byte_info_table_offset;

            uint64_t current_value = 0;
            if (!win_emu.emu().try_read_memory(field_address, &current_value, sizeof(current_value)) || current_value != 0)
            {
                return;
            }

            uint16_t global_rtl_nls_state = 0;
            if (!win_emu.emu().try_read_memory(ntdll_mod->image_base + 0x1726d0, &global_rtl_nls_state, sizeof(global_rtl_nls_state)) ||
                global_rtl_nls_state == 0xFDE9)
            {
                // Real ntdll's own codepage init hasn't run (or genuinely failed) yet - nothing to
                // fix up yet; a legitimate call site further along in NLS init may still populate
                // this field correctly on its own, so don't touch it prematurely.
                return;
            }

            constexpr size_t lead_byte_table_size = 0x200; // 256 WORD entries, one per possible byte value
            const auto aligned_size = static_cast<size_t>(page_align_up(lead_byte_table_size));
            const auto table_address = win_emu.memory.allocate_memory(aligned_size, memory_permission::read);
            const std::vector<std::byte> zeroed_table(lead_byte_table_size, std::byte{0});
            win_emu.emu().write_memory(table_address, zeroed_table.data(), zeroed_table.size());
            win_emu.emu().write_memory(field_address, &table_address, sizeof(table_address));
        }

    } // namespace

    static void serialize(utils::buffer_serializer& buffer, const syscall_handler_entry& obj)
    {
        buffer.write(obj.name);
    }

    static void deserialize(utils::buffer_deserializer& buffer, syscall_handler_entry& obj)
    {
        buffer.read(obj.name);
        obj.handler = nullptr;
    }

    void syscall_dispatcher::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_map(this->handlers_);
    }

    void syscall_dispatcher::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read_map(this->handlers_);
        this->add_handlers();
        this->add_callbacks();
    }

    void syscall_dispatcher::setup(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                                   const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data)
    {
        this->handlers_ = {};

        const auto ntdll_syscalls = find_syscalls(ntdll_exports, ntdll_data);
        const auto win32u_syscalls = find_syscalls(win32u_exports, win32u_data);

        map_syscalls(this->handlers_, ntdll_syscalls);
        map_syscalls(this->handlers_, win32u_syscalls);

        this->add_handlers();
        this->add_callbacks();
    }

    void syscall_dispatcher::add_handlers()
    {
        std::map<std::string, syscall_handler> handler_mapping{};
        syscall_dispatcher::add_handlers(handler_mapping);

        for (auto& entry : this->handlers_ | std::views::values)
        {
            const auto handler = handler_mapping.find(entry.name);
            if (handler == handler_mapping.end())
            {
                continue;
            }

            entry.handler = handler->second;

#ifndef NDEBUG
            handler_mapping.erase(handler);
#endif
        }
    }

    void syscall_dispatcher::dispatch(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        auto& emu = vcpu.cpu;
        auto& context = win_emu.process;

        ensure_nls_lead_byte_info_table(win_emu);

        const auto address = emu.read_instruction_pointer();
        const auto raw_syscall_id = emu.reg<uint32_t>(x86_register::eax);
        const auto syscall_id = raw_syscall_id & 0x3FFF; // Only take low bits for WOW64 compatibility, match windoows wraparound

        const auto entry = this->handlers_.find(syscall_id);
        const auto* syscall_name = (entry != this->handlers_.end()) ? entry->second.name.c_str() : "<unknown>";

        const syscall_context c{
            .win_emu = win_emu,
            .emu = emu,
            .vcpu = vcpu,
            .proc = context,
            .write_status = true,
        };

        try
        {
            if (entry == this->handlers_.end())
            {
                win_emu.log.error("Unknown syscall: 0x%X (raw: 0x%X)\n", syscall_id, raw_syscall_id);
                win_emu.record_stop(stop_reason::unknown_syscall, "0x" + utils::string::to_hex_number(syscall_id));
                c.emu.reg<uint64_t>(x86_register::rax, STATUS_NOT_SUPPORTED);
                c.emu.stop();
                return;
            }

            const auto res = win_emu.callbacks.on_syscall(syscall_id, entry->second.name);
            if (res == instruction_hook_continuation::skip_instruction)
            {
                return;
            }

            if (!entry->second.handler)
            {
                win_emu.log.error("Unimplemented syscall: %s - 0x%X (raw: 0x%X)\n", entry->second.name.c_str(), syscall_id, raw_syscall_id);
                win_emu.record_stop(stop_reason::unimplemented_syscall, entry->second.name);
                c.emu.reg<uint64_t>(x86_register::rax, STATUS_NOT_SUPPORTED);
                c.emu.stop();
                return;
            }

            entry->second.handler(c);

            dispatch_callback(win_emu, entry->second.name);
        }
        catch (std::exception& e)
        {
            win_emu.log.error("Syscall %s threw an exception: 0x%X (raw: 0x%X) (0x%" PRIx64 ") - %s\n", syscall_name, syscall_id,
                              raw_syscall_id, address, e.what());
            win_emu.record_stop(stop_reason::syscall_exception, std::string(syscall_name) + ": " + e.what());
            emu.reg<uint64_t>(x86_register::rax, STATUS_UNSUCCESSFUL);
            win_emu.stop();
        }
        catch (...)
        {
            win_emu.log.error("Syscall %s threw an unknown exception: 0x%X (raw: 0x%X) (0x%" PRIx64 ")\n", syscall_name, syscall_id,
                              raw_syscall_id, address);
            win_emu.record_stop(stop_reason::syscall_exception, std::string(syscall_name) + ": <unknown exception>");
            emu.reg<uint64_t>(x86_register::rax, STATUS_UNSUCCESSFUL);
            win_emu.stop();
        }
    }

    void syscall_dispatcher::dispatch_callback(windows_emulator& win_emu, std::string& syscall_name)
    {
        // active_cpu(), not emu(): this runs under the syscall's scoped_dispatch, and with more than one
        // vCPU the instrumentation-callback redirect must rewrite the acting vCPU's RIP/r10, not vCPU 0's.
        auto& emu = win_emu.active_cpu();
        auto& context = win_emu.process;

        if (context.instrumentation_callback != 0 && syscall_name != "NtContinue")
        {
            auto rip_old = emu.reg<uint64_t>(x86_register::rip);

            // The increase in RIP caused by executing the syscall here has not yet occurred.
            // If RIP is set directly, it will lead to an incorrect address, so the length of
            // the syscall instruction needs to be subtracted.
            emu.reg<uint64_t>(x86_register::rip, context.instrumentation_callback - 2);

            emu.reg<uint64_t>(x86_register::r10, rip_old);
        }
    }

    dispatch_result syscall_dispatcher::dispatch_completion(windows_emulator& win_emu, vcpu_context& vcpu, callback_id callback_id,
                                                            completion_state* completion_state, uint64_t callback_result)
    {
        auto& emu = vcpu.cpu;

        const syscall_context c{.win_emu = win_emu,
                                .emu = emu,
                                .vcpu = vcpu,
                                .proc = win_emu.process,
                                .write_status = true,
                                .is_callback_completion = true,
                                .current_completion_state = completion_state,
                                .previous_callback_result = callback_result};

        const auto entry = this->completion_handlers_.find(callback_id);

        if (entry == this->completion_handlers_.end())
        {
            win_emu.log.error("Unknown callback: 0x%X\n", static_cast<uint32_t>(callback_id));
            c.emu.stop();
            return dispatch_result::error;
        }

        try
        {
            entry->second(c);
            return c.run_callback ? dispatch_result::new_callback : dispatch_result::completed;
        }
        catch (std::exception& e)
        {
            win_emu.log.error("Completion for callback 0x%X threw an exception - %s\n", static_cast<int>(callback_id), e.what());
            emu.stop();
            return dispatch_result::error;
        }
        catch (...)
        {
            win_emu.log.error("Completion for callback 0x%X threw an unknown exception\n", static_cast<int>(callback_id));
            emu.stop();
            return dispatch_result::error;
        }
    }

    syscall_dispatcher::syscall_dispatcher(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                                           const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data)
    {
        this->setup(ntdll_exports, ntdll_data, win32u_exports, win32u_data);
    }

    std::map<callback_id, std::function<std::unique_ptr<completion_state>()>> syscall_dispatcher::completion_state_factories_{};

} // namespace sogen

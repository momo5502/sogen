#include "std_include.hpp"
#include "windows_emulator.hpp"
#include "host_crypt_protect.hpp"

#include <address_utils.hpp>
#include <utils/string.hpp>

#include <memory>
#include <span>
#include <unordered_set>
#include <vector>

#ifdef OS_WINDOWS
#include <wincrypt.h>
#endif

namespace sogen
{
    namespace
    {
        constexpr uint32_t k_cryptprotect_ui_forbidden = 0x1;
        constexpr uint32_t k_error_invalid_parameter = 87;
        [[maybe_unused]] constexpr uint32_t k_error_not_supported = 50;
        constexpr size_t k_max_protect_blob = 16u * 1024u * 1024u;
        constexpr uint64_t k_fastcall_arg6 = 0x30;
        constexpr uint64_t k_fastcall_arg7 = 0x38;

        struct guest_data_blob
        {
            uint32_t cb_data{};
            uint32_t padding{};
            uint64_t pb_data{};
        };

        static_assert(sizeof(guest_data_blob) == 16);

        struct host_crypt_protect_state
        {
            std::unordered_set<uint64_t> hooked_addresses{};
            std::unordered_set<uint64_t> guest_blobs{};
        };

        bool module_name_is(const mapped_module& mod, const std::string_view name)
        {
            const auto filename = std::filesystem::path(mod.name).filename().string();
            return utils::string::equals_ignore_case(std::string_view{filename}, name);
        }

        void set_last_error(windows_emulator& win_emu, const uint32_t last_error)
        {
            auto* thread = win_emu.vcpu(0).active_thread;
            if (!thread || !thread->teb64)
            {
                return;
            }

            thread->teb64->access([&](TEB64& teb) { teb.LastErrorValue = last_error; });
        }

        void return_from_fastcall(x86_64_cpu& cpu, const uint64_t rax)
        {
            const auto rsp = cpu.reg<uint64_t>(x86_register::rsp);
            uint64_t return_address{};
            if (!cpu.try_read_memory(rsp, &return_address, sizeof(return_address)))
            {
                return;
            }

            cpu.reg(x86_register::rax, rax);
            cpu.reg(x86_register::rsp, rsp + sizeof(uint64_t));
            cpu.reg(x86_register::rip, return_address);
        }

        bool read_guest_blob(windows_emulator& win_emu, const uint64_t blob_ptr, std::vector<uint8_t>& out)
        {
            out.clear();
            if (!blob_ptr)
            {
                return false;
            }

            guest_data_blob blob{};
            if (!win_emu.memory.try_read_memory(blob_ptr, &blob, sizeof(blob)))
            {
                return false;
            }

            if (blob.cb_data == 0)
            {
                return true;
            }

            if (blob.pb_data == 0 || blob.cb_data > k_max_protect_blob)
            {
                return false;
            }

            out.resize(blob.cb_data);
            return win_emu.memory.try_read_memory(blob.pb_data, out.data(), out.size());
        }

        bool write_guest_blob(windows_emulator& win_emu, host_crypt_protect_state& state, const uint64_t blob_ptr,
                               const std::span<const uint8_t> data)
        {
            if (!blob_ptr)
            {
                return false;
            }

            guest_data_blob blob{};
            if (data.empty())
            {
                return win_emu.memory.try_write_memory(blob_ptr, &blob, sizeof(blob));
            }

            const auto bytes = static_cast<size_t>(page_align_up(data.size()));
            const auto guest_ptr = win_emu.memory.allocate_memory(bytes, memory_permission::read_write);
            if (!guest_ptr)
            {
                return false;
            }

            if (!win_emu.memory.try_write_memory(guest_ptr, data.data(), data.size()))
            {
                win_emu.memory.release_memory(guest_ptr, 0);
                return false;
            }

            blob.cb_data = static_cast<uint32_t>(data.size());
            blob.pb_data = guest_ptr;
            if (!win_emu.memory.try_write_memory(blob_ptr, &blob, sizeof(blob)))
            {
                win_emu.memory.release_memory(guest_ptr, 0);
                return false;
            }

            state.guest_blobs.insert(guest_ptr);
            return true;
        }

#ifdef OS_WINDOWS
        struct host_protect_result
        {
            bool ok{};
            uint32_t last_error{};
            std::vector<uint8_t> data{};
        };

        host_protect_result host_crypt_protect_or_unprotect(const bool unprotect, const std::span<const uint8_t> data,
                                                            const std::span<const uint8_t> entropy, const uint32_t flags)
        {
            DATA_BLOB input{};
            input.cbData = static_cast<DWORD>(data.size());
            input.pbData = const_cast<BYTE*>(data.data());

            DATA_BLOB entropy_blob{};
            DATA_BLOB* entropy_ptr = nullptr;
            if (!entropy.empty())
            {
                entropy_blob.cbData = static_cast<DWORD>(entropy.size());
                entropy_blob.pbData = const_cast<BYTE*>(entropy.data());
                entropy_ptr = &entropy_blob;
            }

            DATA_BLOB output{};
            const DWORD host_flags = flags | k_cryptprotect_ui_forbidden;
            const BOOL ok = unprotect ? CryptUnprotectData(&input, nullptr, entropy_ptr, nullptr, nullptr, host_flags, &output)
                                        : CryptProtectData(&input, nullptr, entropy_ptr, nullptr, nullptr, host_flags, &output);

            host_protect_result result{};
            result.ok = ok == TRUE;
            result.last_error = GetLastError();
            if (ok == TRUE && output.pbData && output.cbData)
            {
                result.data.assign(output.pbData, output.pbData + output.cbData);
            }

            if (output.pbData)
            {
                LocalFree(output.pbData);
            }

            return result;
        }
#endif

        void handle_crypt_protect(windows_emulator& win_emu, host_crypt_protect_state& state, const bool unprotect)
        {
            auto& cpu = win_emu.emu();
            const auto p_data_in = cpu.reg<uint64_t>(x86_register::rcx);
            const auto p_psz_or_descr = cpu.reg<uint64_t>(x86_register::rdx);
            const auto p_entropy = cpu.reg<uint64_t>(x86_register::r8);
            const auto rsp = cpu.reg<uint64_t>(x86_register::rsp);

            uint32_t flags = 0;
            uint64_t p_data_out = 0;
            cpu.try_read_memory(rsp + k_fastcall_arg6, &flags, sizeof(flags));
            cpu.try_read_memory(rsp + k_fastcall_arg7, &p_data_out, sizeof(p_data_out));

            std::vector<uint8_t> input{};
            std::vector<uint8_t> entropy{};
            if (!read_guest_blob(win_emu, p_data_in, input) || (p_entropy && !read_guest_blob(win_emu, p_entropy, entropy)))
            {
                set_last_error(win_emu, k_error_invalid_parameter);
                return_from_fastcall(cpu, 0);
                return;
            }

#ifndef OS_WINDOWS
            (void)state;
            (void)p_psz_or_descr;
            (void)p_data_out;
            (void)unprotect;
            set_last_error(win_emu, k_error_not_supported);
            return_from_fastcall(cpu, 0);
#else
            const auto result = host_crypt_protect_or_unprotect(unprotect, input, entropy, flags);
            if (!result.ok || !write_guest_blob(win_emu, state, p_data_out, result.data))
            {
                set_last_error(win_emu, result.ok ? k_error_invalid_parameter : result.last_error);
                return_from_fastcall(cpu, 0);
                return;
            }

            if (unprotect && p_psz_or_descr)
            {
                const uint64_t none = 0;
                win_emu.memory.try_write_memory(p_psz_or_descr, &none, sizeof(none));
            }

            set_last_error(win_emu, 0);
            return_from_fastcall(cpu, 1);
#endif
        }

        void handle_local_free(windows_emulator& win_emu, host_crypt_protect_state& state)
        {
            auto& cpu = win_emu.emu();
            const auto pointer = cpu.reg<uint64_t>(x86_register::rcx);
            if (!pointer || !state.guest_blobs.contains(pointer))
            {
                return;
            }

            win_emu.memory.release_memory(pointer, 0);
            state.guest_blobs.erase(pointer);
            set_last_error(win_emu, 0);
            return_from_fastcall(cpu, 0);
        }

        void hook_export_once(windows_emulator& win_emu, host_crypt_protect_state& state, const mapped_module& mod,
                               const std::string_view export_name, memory_execution_hook_callback callback)
        {
            const auto address = mod.find_export(export_name);
            if (!address || !state.hooked_addresses.insert(address).second)
            {
                return;
            }

            win_emu.emu().hook_memory_execution(address, std::move(callback));
        }

        void install_module_hooks(windows_emulator& win_emu, const mapped_module& mod,
                                   const std::shared_ptr<host_crypt_protect_state>& state)
        {
            if (win_emu.process.is_wow64_process)
            {
                return;
            }

            if (module_name_is(mod, "crypt32.dll") || module_name_is(mod, "dpapi.dll"))
            {
                hook_export_once(win_emu, *state, mod, "CryptUnprotectData", [&win_emu, state](cpu_interface&, uint64_t) {
                    handle_crypt_protect(win_emu, *state, true);
                });
                hook_export_once(win_emu, *state, mod, "CryptProtectData", [&win_emu, state](cpu_interface&, uint64_t) {
                    handle_crypt_protect(win_emu, *state, false);
                });
            }

            if (module_name_is(mod, "kernel32.dll") || module_name_is(mod, "kernelbase.dll"))
            {
                hook_export_once(win_emu, *state, mod, "LocalFree", [&win_emu, state](cpu_interface&, uint64_t) {
                    handle_local_free(win_emu, *state);
                });
            }
        }
    }

    // CryptProtectData / CryptUnprotectData talk to lsass over ICryptProtect
    // (\RPC Control\protected_storage). That RPC interface is not implemented;
    // intercept the crypt32/dpapi exports and run the host DPAPI APIs instead.
    void setup_host_crypt_protect_hooks(windows_emulator& win_emu)
    {
        auto state = std::make_shared<host_crypt_protect_state>();
        win_emu.callbacks.on_module_load.add([&win_emu, state](mapped_module& mod) { install_module_hooks(win_emu, mod, state); });
    }

} // namespace sogen

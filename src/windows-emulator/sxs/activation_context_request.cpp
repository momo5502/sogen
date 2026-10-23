#include "../std_include.hpp"
#include "activation_context_request.hpp"
#include "activation_context.hpp"
#include <address_utils.hpp>
#include <utils/finally.hpp>
#include "../windows_emulator.hpp"

namespace sogen::sxs
{
    namespace
    {
        using unicode_string = UNICODE_STRING<EmulatorTraits<Emu64>>;

        struct sxs_stream
        {
            uint32_t flags;
            uint32_t reserved;
            unicode_string path;
            uint64_t file_handle;
            uint64_t section_handle;
            uint64_t address;
            uint64_t size;
        };

        struct create_activation_context_request
        {
            uint32_t flags;
            uint32_t processor_architecture;
            unicode_string culture_fallbacks;
            sxs_stream manifest;
            sxs_stream policy;
            unicode_string assembly_directory;
            unicode_string assembly_identity;
            uint64_t file_last_write_time;
            uint64_t resource_id;
            uint64_t output_address;
            uint64_t reserved;
            uint32_t reserved2;
            uint32_t result_flags;
        };

        // BaseSrv's SxS request follows the 0x18-byte CSR header, not PORT_MESSAGE.
        constexpr uint64_t csr_header_size = 0x18;
        static_assert(sizeof(sxs_stream) == 0x38);
        static_assert(offsetof(create_activation_context_request, manifest) == 0x18);
        static_assert(offsetof(create_activation_context_request, assembly_directory) == 0x88);
        static_assert(offsetof(create_activation_context_request, output_address) == 0xb8);
        static_assert(offsetof(create_activation_context_request, result_flags) == 0xcc);

        bool read_stream(windows_emulator& win_emu, const sxs_stream& stream, std::vector<std::byte>& contents)
        {
            constexpr uint64_t maximum_manifest_size = 16 * 1024 * 1024;
            if (stream.size == 0 || stream.size > maximum_manifest_size)
            {
                return false;
            }
            contents.resize(static_cast<size_t>(stream.size));
            if (stream.address != 0)
            {
                return win_emu.memory.try_read_memory(stream.address, contents.data(), contents.size());
            }
            auto* file = win_emu.process.files.get(stream.file_handle);
            if (!file || !file->handle)
            {
                return false;
            }
            const auto position = file->handle.tell();
            const auto restore = utils::finally([&] { file->handle.seek_to(position); });
            return file->handle.seek_to(0) && file->handle.read(contents) == contents.size();
        }

        bool read_string(const memory_manager& memory, const unicode_string& string, std::u16string& value)
        {
            if (string.Length > string.MaximumLength || string.Length % sizeof(char16_t) != 0)
            {
                return false;
            }
            value.resize(string.Length / sizeof(char16_t));
            return value.empty() || memory.try_read_memory(string.Buffer, value.data(), string.Length);
        }
    }

    lpc_request_result handle_create_activation_context(windows_emulator& win_emu, const lpc_request_context& request)
    {
        create_activation_context_request data{};
        const auto request_address = request.send_buffer + csr_header_size;
        if (request.send_buffer_length < csr_header_size + sizeof(data) ||
            !win_emu.memory.try_read_memory(request_address, &data, sizeof(data)) || data.output_address == 0)
        {
            return {STATUS_INVALID_PARAMETER, lpc_request_result::reply_in_place};
        }

        uint16_t machine{};
        switch (data.processor_architecture)
        {
        case PROCESSOR_ARCHITECTURE_AMD64:
            machine = IMAGE_FILE_MACHINE_AMD64;
            break;
        case PROCESSOR_ARCHITECTURE_INTEL:
        case PROCESSOR_ARCHITECTURE_IA32_ON_WIN64:
            machine = IMAGE_FILE_MACHINE_I386;
            break;
        case PROCESSOR_ARCHITECTURE_ARM64:
            machine = IMAGE_FILE_MACHINE_ARM64;
            break;
        default:
            win_emu.log.warn("Unsupported activation context architecture: %u\n", data.processor_architecture);
            return {STATUS_NOT_SUPPORTED, lpc_request_result::reply_in_place};
        }

        std::vector<std::byte> manifest_contents{};
        std::vector<std::byte> policy_contents{};
        if (!read_stream(win_emu, data.manifest, manifest_contents) ||
            (data.policy.size != 0 && !read_stream(win_emu, data.policy, policy_contents)))
        {
            return {STATUS_INVALID_PARAMETER, lpc_request_result::reply_in_place};
        }

        std::u16string manifest_name{};
        std::u16string assembly_name{};
        std::u16string policy_name{};
        std::u16string language{};
        if (!read_string(win_emu.memory, data.manifest.path, manifest_name) ||
            !read_string(win_emu.memory, data.assembly_directory, assembly_name) ||
            !read_string(win_emu.memory, data.policy.path, policy_name) || !read_string(win_emu.memory, data.culture_fallbacks, language))
        {
            return {STATUS_INVALID_PARAMETER, lpc_request_result::reply_in_place};
        }
        const windows_path manifest_path(manifest_name);
        const windows_path assembly_directory(assembly_name);
        if (!manifest_path.is_absolute() || !assembly_directory.is_absolute())
        {
            return {STATUS_INVALID_PARAMETER, lpc_request_result::reply_in_place};
        }
        mapped_module executable{};
        executable.module_path = assembly_directory / windows_path(manifest_path.leaf());
        executable.path = win_emu.file_sys.translate(executable.module_path);
        executable.machine = static_cast<uint16_t>(machine);

        const activation_context_source manifest{.contents = manifest_contents, .path = manifest_path};
        std::optional<activation_context_source> config{};
        if (!policy_contents.empty())
        {
            config = activation_context_source{.contents = policy_contents, .path = windows_path(policy_name)};
        }
        if (const auto separator = language.find(u'\0'); separator != std::u16string::npos)
        {
            language.resize(separator);
        }
        std::vector<std::byte> blob{};
        try
        {
            blob = build_activation_context(win_emu.file_sys, executable, manifest, config, windows_path(win_emu.version.get_system_root()),
                                            u16_to_u8(language));
        }
        catch (const std::runtime_error& error)
        {
            win_emu.log.warn("Unable to create activation context: %s\n", error.what());
            return {STATUS_SXS_CANT_GEN_ACTCTX, lpc_request_result::reply_in_place};
        }
        catch (const std::bad_alloc&)
        {
            return {STATUS_NO_MEMORY, lpc_request_result::reply_in_place};
        }
        const auto address = win_emu.memory.allocate_memory(static_cast<size_t>(page_align_up(blob.size())), memory_permission::read, false,
                                                            0, memory_region_kind::file_section_view);
        if (address == 0)
        {
            return {STATUS_NO_MEMORY, lpc_request_result::reply_in_place};
        }
        if (!win_emu.memory.try_write_memory(address, blob.data(), blob.size()) ||
            !win_emu.memory.try_write_memory(data.output_address, &address, sizeof(address)))
        {
            win_emu.memory.release_memory(address, 0);
            return {STATUS_ACCESS_VIOLATION, lpc_request_result::reply_in_place};
        }
        data.result_flags = 1;
        win_emu.memory.write_memory(request_address, &data, sizeof(data));
        return {STATUS_SUCCESS, lpc_request_result::reply_in_place};
    }
}

#include "../std_include.hpp"
#include "service_control.hpp"
#include "ndr_cursor.hpp"

#include "binary_writer.hpp"
#include "../registry/registry_utils.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{

    namespace
    {
        // The \RPC Control\ntsvcs ALPC port hosts several RPC interfaces. The one that matters here is svcctl
        // (the Service Control Manager), {367ABB81-9844-35F1-AD32-98F038001003}: while creating a render audio
        // client, mmdevapi opens the "AudioSrv" service through it (OpenSCManagerW -> OpenServiceW ->
        // SubscribeServiceChangeNotifications) so it can react to the audio service restarting, and wintrust waits
        // for CryptSvc the same way (QueryServiceConfigW -> WaitServiceState) before it uses the catalog database.
        // The same port also carries the PnP / Plug-and-Play interface (CM_* config-manager notifications); those
        // callers tolerate an empty success reply, which is the fallback below.
        constexpr std::array<uint8_t, 16> k_iface_svcctl = {0x81, 0xbb, 0x7a, 0x36, 0x44, 0x98, 0xf1, 0x35,
                                                            0xad, 0x32, 0x98, 0xf0, 0x38, 0x00, 0x10, 0x03};

        // svcctl opnums. NOTE: this Windows build's svcctl client uses opnum numbers shifted from the classic
        // MS-SCMR table - observed on the wire: ROpenSCManagerW arrives as opnum 64 (its [in] is the
        // "ServicesActive" database string + access, with no input handle). Follow-on opnums are discovered the
        // same way; the classic numbers are kept as fallbacks.
        constexpr uint32_t k_svcctl_close_handle = 0;          // RCloseServiceHandle ([in,out] handle)
        constexpr uint32_t k_svcctl_query_status = 6;          // RQueryServiceStatus (service handle -> SERVICE_STATUS)
        constexpr uint32_t k_svcctl_open_service_w = 16;       // ROpenServiceW (SCM handle + name + access)
        constexpr uint32_t k_svcctl_query_config_w = 17;       // RQueryServiceConfigW (service handle + buffer size)
        constexpr uint32_t k_svcctl_query_service_config = 55; // RI_ScQueryServiceConfig (handle + info level)
        constexpr uint32_t k_svcctl_open_sc_manager_w = 64;    // ROpenSCManagerW (database string + access)

        constexpr uint32_t k_error_success = 0;
        constexpr uint32_t k_error_file_not_found = 2;
        constexpr uint32_t k_error_access_denied = 5;
        constexpr uint32_t k_error_invalid_handle = 6;
        constexpr uint32_t k_error_insufficient_buffer = 122;
        constexpr uint32_t k_error_service_does_not_exist = 1060;

        // RI_ScQueryServiceConfig levels 2-4 return a WNF state name; level 3 is the service's status-change name.
        constexpr uint32_t k_service_status_notification_level = 3;

        // services.exe reports sizeof(QUERY_SERVICE_CONFIGW) as the needed size when it cannot read the service key.
        constexpr uint32_t k_query_service_config_size = 64;

        // services.exe publishes 1 << (dwCurrentState - 1) into a service's status-change name, and creates that
        // name (publishing the current state) the first time a client asks for it.
        constexpr uint32_t k_service_running_notification = 1u << (4 - 1);

        // Fabricated SC_RPC_HANDLEs. The bytes only need to round-trip: the client stores the handle from Open* and
        // hands it back on the follow-on Open/Close/Notify calls. Service handles carry a per-connection counter
        // after the prefix so each one names the service it was opened for.
        using context_handle = std::array<uint8_t, 16>;
        constexpr context_handle k_scm_handle_uuid = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x53, 0x43, 0x4d,
                                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
        constexpr std::array<uint8_t, 8> k_service_handle_prefix = {0x53, 0x6f, 0x67, 0x65, 0x6e, 0x53, 0x76, 0x63};
        constexpr context_handle k_null_handle_uuid = {};

        struct service_config
        {
            uint32_t service_type{};
            uint32_t start_type{};
            uint32_t error_control{};
            uint32_t tag_id{};
            std::u16string binary_path_name{};
            std::u16string load_order_group{};
            std::vector<std::u16string> dependencies{};
            std::u16string service_start_name{};
            std::u16string display_name{};
        };

        // An NDR [out] context handle: a 4-byte attributes field followed by the 16-byte handle UUID.
        void write_context_handle(utils::aligned_binary_writer& writer, const context_handle& uuid)
        {
            writer.write<uint32_t>(0); // context handle attributes
            writer.write(uuid.data(), uuid.size(), 1);
        }

        void write_return(utils::aligned_binary_writer& writer, const uint32_t win32_error)
        {
            writer.align_to(sizeof(uint32_t));
            writer.write<uint32_t>(win32_error);
        }

        std::optional<context_handle> read_context_handle(ndr_cursor& cursor)
        {
            uint32_t attributes{};
            std::vector<uint8_t> uuid{};
            if (!cursor.read_u32(attributes) || !cursor.read_bytes(sizeof(context_handle), uuid))
            {
                return std::nullopt;
            }

            context_handle handle{};
            std::ranges::copy(uuid, handle.begin());
            return handle;
        }

        std::filesystem::path service_key_path(const std::u16string_view service_name)
        {
            return std::filesystem::path{R"(\Registry\Machine\System\CurrentControlSet\Services)"} / service_name;
        }

        uint32_t read_dword(registry_manager& registry, const registry_key& key, const std::string_view name)
        {
            const auto value = registry.get_value(key, name);
            return value ? value->as_dword().value_or(0) : 0;
        }

        std::vector<std::u16string> read_multi_string(registry_manager& registry, const registry_key& key, const std::string_view name)
        {
            std::vector<std::u16string> strings{};
            const auto value = registry.get_value(key, name);
            if (!value || value->type != REG_MULTI_SZ)
            {
                return strings;
            }

            const std::u16string_view data{reinterpret_cast<const char16_t*>(value->data.data()), value->data.size() / sizeof(char16_t)};
            for (size_t start = 0; start < data.size();)
            {
                const auto end = std::min(data.find(u'\0', start), data.size());
                if (end == start)
                {
                    break;
                }

                strings.emplace_back(data.substr(start, end - start));
                start = end + 1;
            }

            return strings;
        }

        // Mirrors services.exe's ScReadConfigFromReg/ScReadDependencies: group dependencies follow the service ones,
        // each prefixed with SC_GROUP_IDENTIFIER, and a service without a display name reports its key name.
        service_config read_service_config(registry_manager& registry, const registry_key& key, const std::u16string& service_name)
        {
            service_config config{
                .service_type = read_dword(registry, key, "Type"),
                .start_type = read_dword(registry, key, "Start"),
                .error_control = read_dword(registry, key, "ErrorControl"),
                .tag_id = read_dword(registry, key, "Tag"),
                .binary_path_name = registry_utils::read_registry_string(registry, key, "ImagePath").value_or(u""),
                .load_order_group = registry_utils::read_registry_string(registry, key, "Group").value_or(u""),
                .dependencies = read_multi_string(registry, key, "DependOnService"),
                .service_start_name = registry_utils::read_registry_string(registry, key, "ObjectName").value_or(u""),
                .display_name = registry_utils::read_registry_string(registry, key, "DisplayName").value_or(service_name),
            };

            for (const auto& group : read_multi_string(registry, key, "DependOnGroup"))
            {
                config.dependencies.push_back(u'+' + group);
            }

            return config;
        }

        // The size services.exe's RQueryServiceConfigW reports in pcbBytesNeeded (and compares cbBufSize against).
        uint32_t service_config_size(const service_config& config)
        {
            const auto bytes = [](const std::u16string_view value) { return static_cast<uint32_t>(value.size() * sizeof(char16_t)); };

            uint32_t dependencies_size = 4;
            if (!config.dependencies.empty())
            {
                dependencies_size = sizeof(char16_t);
                for (const auto& dependency : config.dependencies)
                {
                    dependencies_size += bytes(dependency) + sizeof(char16_t);
                }
            }

            const auto size = 89 + bytes(config.binary_path_name) + bytes(config.display_name) + bytes(config.load_order_group) +
                              dependencies_size + 16 + bytes(config.service_start_name);
            return (size + 3) & ~1u;
        }

        // services.exe hands the dependency MULTI_SZ over the wire with every separator replaced by '/' (an empty
        // list becomes "/"); sechost's QueryServiceConfigW turns the slashes back into NULs.
        std::u16string encode_dependencies(const std::vector<std::u16string>& dependencies)
        {
            if (dependencies.empty())
            {
                return u"/";
            }

            std::u16string encoded{};
            for (const auto& dependency : dependencies)
            {
                encoded += dependency;
                encoded += u'/';
            }

            return encoded;
        }

        void write_service_config(utils::aligned_binary_writer& writer, const service_config& config, const bool include_strings)
        {
            writer.write<uint32_t>(config.service_type);
            writer.write<uint32_t>(config.start_type);
            writer.write<uint32_t>(config.error_control);
            writer.write_ndr_pointer(include_strings); // lpBinaryPathName
            writer.write_ndr_pointer(include_strings); // lpLoadOrderGroup
            writer.write<uint32_t>(config.tag_id);
            writer.write_ndr_pointer(include_strings); // lpDependencies
            writer.write_ndr_pointer(include_strings); // lpServiceStartName
            writer.write_ndr_pointer(include_strings); // lpDisplayName

            if (include_strings)
            {
                writer.write_ndr_u16string(config.binary_path_name);
                writer.write_ndr_u16string(config.load_order_group);
                writer.write_ndr_u16string(encode_dependencies(config.dependencies));
                writer.write_ndr_u16string(config.service_start_name);
                writer.write_ndr_u16string(config.display_name);
            }
        }

        uint64_t get_service_status_state_name(process_context& process, const std::u16string& service_name)
        {
            const auto existing = process.service_status_state_names.find(service_name);
            if (existing != process.service_status_state_names.end())
            {
                return existing->second;
            }

            const auto state_name = make_temporary_system_wnf_state_name(process.service_status_state_names.size() + 1);
            process.service_status_state_names.emplace(service_name, state_name);

            std::vector<std::byte> data(sizeof(k_service_running_notification));
            std::memcpy(data.data(), &k_service_running_notification, data.size());
            process.wnf_states.insert_or_assign(state_name, wnf_state{.change_stamp = 1, .data = std::move(data)});

            return state_name;
        }

        struct service_control_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& win_emu, const uint32_t procedure_id, const lpc_request_context& c,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>& /*reply_handles*/) override
            {
                if (this->bound_interface() != k_iface_svcctl)
                {
                    // PnP / other ntsvcs interfaces (e.g. CM_Register_Notification): an empty success reply is
                    // enough for the current callers to continue.
                    return STATUS_SUCCESS;
                }

                switch (procedure_id)
                {
                case k_svcctl_open_sc_manager_w:
                    write_context_handle(writer, k_scm_handle_uuid);
                    write_return(writer, k_error_success);
                    return STATUS_SUCCESS;

                case k_svcctl_open_service_w:
                    return this->open_service(win_emu, c, writer);

                case k_svcctl_query_config_w:
                    return this->query_service_config(win_emu, c, writer);

                case k_svcctl_query_service_config:
                    return this->query_service_notification_name(win_emu, c, writer);

                case k_svcctl_query_status:
                    // RQueryServiceStatus([in] service handle, [out] SERVICE_STATUS). The [out] struct is seven
                    // inline DWORDs. mmdevapi queries the AudioSrv service state while bringing up a render
                    // client; report it as a running shared-process service so the caller proceeds.
                    writer.write<uint32_t>(0x20); // dwServiceType = SERVICE_WIN32_SHARE_PROCESS
                    writer.write<uint32_t>(4);    // dwCurrentState = SERVICE_RUNNING
                    writer.write<uint32_t>(0x5);  // dwControlsAccepted = STOP | SHUTDOWN
                    writer.write<uint32_t>(0);    // dwWin32ExitCode
                    writer.write<uint32_t>(0);    // dwServiceSpecificExitCode
                    writer.write<uint32_t>(0);    // dwCheckPoint
                    writer.write<uint32_t>(0);    // dwWaitHint
                    write_return(writer, k_error_success);
                    return STATUS_SUCCESS;

                case k_svcctl_close_handle:
                    return this->close_handle(win_emu, c, writer);

                default:
                    return STATUS_NOT_SUPPORTED;
                }
            }

            void serialize_object(utils::buffer_serializer& buffer) const override
            {
                rpc_port::serialize_object(buffer);
                buffer.write_map(this->services_);
                buffer.write(this->next_service_handle_);
            }

            void deserialize_object(utils::buffer_deserializer& buffer) override
            {
                rpc_port::deserialize_object(buffer);
                buffer.read_map(this->services_);
                buffer.read(this->next_service_handle_);
            }

          private:
            std::map<context_handle, std::u16string> services_{};
            uint64_t next_service_handle_{1};

            context_handle allocate_service_handle()
            {
                context_handle handle{};
                std::ranges::copy(k_service_handle_prefix, handle.begin());
                const auto id = this->next_service_handle_++;
                std::memcpy(handle.data() + k_service_handle_prefix.size(), &id, sizeof(id));
                return handle;
            }

            NTSTATUS open_service(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer)
            {
                const auto request = read_rpc_request(win_emu.memory, c);
                ndr_cursor cursor{.bytes = request, .pointer_size = writer.pointer_size()};
                std::u16string service_name{};
                if (!read_context_handle(cursor) || !cursor.read_ndr_string(service_name))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!win_emu.registry.get_key(service_key_path(service_name)))
                {
                    write_context_handle(writer, k_null_handle_uuid);
                    write_return(writer, k_error_service_does_not_exist);
                    return STATUS_SUCCESS;
                }

                const auto handle = this->allocate_service_handle();
                this->services_.emplace(handle, std::move(service_name));
                write_context_handle(writer, handle);
                write_return(writer, k_error_success);
                return STATUS_SUCCESS;
            }

            NTSTATUS query_service_config(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer)
            {
                const auto request = read_rpc_request(win_emu.memory, c);
                ndr_cursor cursor{.bytes = request, .pointer_size = writer.pointer_size()};
                const auto handle = read_context_handle(cursor);
                uint32_t buffer_size{};
                if (!handle || !cursor.read_u32(buffer_size))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto service = this->services_.find(*handle);
                if (service == this->services_.end())
                {
                    write_service_config(writer, {}, false);
                    writer.write<uint32_t>(0);
                    write_return(writer, k_error_invalid_handle);
                    return STATUS_SUCCESS;
                }

                const auto key = win_emu.registry.get_key(service_key_path(service->second));
                if (!key)
                {
                    write_service_config(writer, {}, false);
                    writer.write<uint32_t>(k_query_service_config_size);
                    write_return(writer, k_error_file_not_found);
                    return STATUS_SUCCESS;
                }

                const auto config = read_service_config(win_emu.registry, *key, service->second);
                const auto needed = service_config_size(config);
                const auto fits = needed <= buffer_size;
                write_service_config(writer, config, fits);
                writer.write<uint32_t>(needed);
                write_return(writer, fits ? k_error_success : k_error_insufficient_buffer);
                return STATUS_SUCCESS;
            }

            NTSTATUS query_service_notification_name(windows_emulator& win_emu, const lpc_request_context& c,
                                                     utils::aligned_binary_writer& writer)
            {
                const auto request = read_rpc_request(win_emu.memory, c);
                ndr_cursor cursor{.bytes = request, .pointer_size = writer.pointer_size()};
                const auto handle = read_context_handle(cursor);
                uint32_t info_level{};
                if (!handle || !cursor.read_u32(info_level))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                // The [out] is a non-encapsulated union discriminated by the info level whose arm is a unique pointer
                // to an 8-byte WNF_STATE_NAME. Service-set and property change names are not modelled; their callers
                // (SubscribeServiceChangeNotifications) pass ERROR_ACCESS_DENIED straight back.
                const auto service = this->services_.find(*handle);
                auto status = k_error_success;
                if (info_level != k_service_status_notification_level)
                {
                    status = k_error_access_denied;
                }
                else if (service == this->services_.end())
                {
                    status = k_error_invalid_handle;
                }

                writer.write<uint32_t>(info_level);
                writer.write_ndr_pointer(status == k_error_success);
                if (status == k_error_success)
                {
                    writer.write<uint64_t>(get_service_status_state_name(win_emu.process, service->second));
                }

                write_return(writer, status);
                return STATUS_SUCCESS;
            }

            NTSTATUS close_handle(windows_emulator& win_emu, const lpc_request_context& c, utils::aligned_binary_writer& writer)
            {
                const auto request = read_rpc_request(win_emu.memory, c);
                ndr_cursor cursor{.bytes = request, .pointer_size = writer.pointer_size()};
                if (const auto handle = read_context_handle(cursor))
                {
                    this->services_.erase(*handle);
                }

                // [in, out] SC_RPC_HANDLE: the client expects the handle nulled out on a successful close.
                write_context_handle(writer, k_null_handle_uuid);
                write_return(writer, k_error_success);
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<port> create_service_control_port()
    {
        return std::make_unique<service_control_port>();
    }

} // namespace sogen

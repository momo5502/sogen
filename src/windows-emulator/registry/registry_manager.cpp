#include "../std_include.hpp"
#include "registry_manager.hpp"
#include "registry_utils.hpp"

#include <serialization_helper.hpp>

#include "hive_parser.hpp"

namespace sogen
{

    namespace
    {
        bool is_subpath(const utils::path_key& root, const utils::path_key& p)
        {
            auto root_it = root.get().begin();
            auto p_it = p.get().begin();

            for (; root_it != root.get().end(); ++root_it, ++p_it)
            {
                if (p_it == p.get().end() || *root_it != *p_it)
                {
                    return false;
                }
            }

            return true;
        }

        void register_hive(registry_manager::hive_map& hives, const utils::path_key& key, const std::filesystem::path& file)
        {
            hives[key] = std::make_unique<hive_parser>(file);
        }

        void register_optional_hive(registry_manager::hive_map& hives, const utils::path_key& key, const std::filesystem::path& file)
        {
            if (!std::filesystem::is_regular_file(file))
            {
                return;
            }

            hives[key] = std::make_unique<hive_parser>(file);
        }

        std::pair<utils::path_key, bool> perform_path_substitution(const std::map<utils::path_key, utils::path_key>& path_mapping,
                                                                   utils::path_key path)
        {
            for (const auto& mapping : path_mapping)
            {
                if (path == mapping.first)
                {
                    return {mapping.second, true};
                }

                if (is_subpath(mapping.first.get(), path.get()))
                {
                    return {mapping.second.get() / path.get().lexically_relative(mapping.first.get()), true};
                }
            }

            return {std::move(path), false};
        }
    }

    registry_manager::registry_manager() = default;
    registry_manager::~registry_manager() = default;
    registry_manager::registry_manager(registry_manager&&) noexcept = default;
    registry_manager& registry_manager::operator=(registry_manager&&) noexcept = default;

    registry_manager::registry_manager(const std::filesystem::path& hive_path)
        : hive_path_(std::filesystem::absolute(hive_path))
    {
        this->setup();
    }

    void registry_manager::setup()
    {
        this->path_mapping_.clear();
        this->overlay_values_.clear();
        this->hives_.clear();

        const std::filesystem::path root = R"(\registry)";
        const std::filesystem::path machine = root / "machine";
        const std::filesystem::path user = root / "user";

        register_hive(this->hives_, machine / "system", this->hive_path_ / "SYSTEM");
        register_hive(this->hives_, machine / "security", this->hive_path_ / "SECURITY");
        register_hive(this->hives_, machine / "sam", this->hive_path_ / "SAM");
        register_hive(this->hives_, machine / "software", this->hive_path_ / "SOFTWARE");
        register_hive(this->hives_, machine / "system", this->hive_path_ / "SYSTEM");
        register_optional_hive(this->hives_, machine / "hardware", this->hive_path_ / "HARDWARE");

        register_hive(this->hives_, root / "user", this->hive_path_ / "NTUSER.DAT");

        this->add_path_mapping(user / registry_utils::get_user_sid_string(*this), user);
        this->add_path_mapping(user / ".Default", user);

        this->add_path_mapping(machine / "system" / "CurrentControlSet", machine / "system" / "ControlSet001");
        this->add_path_mapping(machine / "system" / "ControlSet001" / "Control" / "ComputerName" / "ActiveComputerName",
                               machine / "system" / "ControlSet001" / "Control" / "ComputerName" / "ComputerName");

        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Classes", machine / "SOFTWARE" / "Classes" / "Wow6432Node");
        this->add_path_mapping(machine / "SOFTWARE" / "Classes" / "Wow6432Node" / "AppID", machine / "SOFTWARE" / "Classes" / "AppID");
        this->add_path_mapping(machine / "SOFTWARE" / "Classes" / "Wow6432Node" / "PROTOCOLS",
                               machine / "SOFTWARE" / "Classes" / "PROTOCOLS");
        this->add_path_mapping(machine / "SOFTWARE" / "Classes" / "Wow6432Node" / "TypeLib", machine / "SOFTWARE" / "Classes" / "TypeLib");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Clients", machine / "SOFTWARE" / "Clients");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Cellular",
                               machine / "SOFTWARE" / "Microsoft" / "Cellular");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "COM3", machine / "SOFTWARE" / "Microsoft" / "COM3");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Cryptography" / "Calais" / "Current",
                               machine / "SOFTWARE" / "Microsoft" / "Cryptography" / "Calais" / "Current");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Cryptography" / "Calais" / "Readers",
                               machine / "SOFTWARE" / "Microsoft" / "Cryptography" / "Calais" / "Readers");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Cryptography" / "Services",
                               machine / "SOFTWARE" / "Microsoft" / "Cryptography" / "Services");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "CTF" / "SystemShared",
                               machine / "SOFTWARE" / "Microsoft" / "CTF" / "SystemShared");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "CTF" / "TIP",
                               machine / "SOFTWARE" / "Microsoft" / "CTF" / "TIP");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "DeviceReg",
                               machine / "SOFTWARE" / "Microsoft" / "DeviceReg");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "DFS", machine / "SOFTWARE" / "Microsoft" / "DFS");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Driver Signing",
                               machine / "SOFTWARE" / "Microsoft" / "Driver Signing");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "EnterpriseCertificates",
                               machine / "SOFTWARE" / "Microsoft" / "EnterpriseCertificates");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "EventSystem",
                               machine / "SOFTWARE" / "Microsoft" / "EventSystem");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "FingerKB",
                               machine / "SOFTWARE" / "Microsoft" / "FingerKB");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "FuzzyDS",
                               machine / "SOFTWARE" / "Microsoft" / "FuzzyDS");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Input", machine / "SOFTWARE" / "Microsoft" / "Input");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "LanguageOverlay",
                               machine / "SOFTWARE" / "Microsoft" / "LanguageOverlay");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Messaging",
                               machine / "SOFTWARE" / "Microsoft" / "Messaging");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "MSMQ", machine / "SOFTWARE" / "Microsoft" / "MSMQ");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "MTF", machine / "SOFTWARE" / "Microsoft" / "MTF");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "MTFFuzzyFactors",
                               machine / "SOFTWARE" / "Microsoft" / "MTFFuzzyFactors");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "MTFInputType",
                               machine / "SOFTWARE" / "Microsoft" / "MTFInputType");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "MTFKeyboardMappings",
                               machine / "SOFTWARE" / "Microsoft" / "MTFKeyboardMappings");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Non-Driver Signing",
                               machine / "SOFTWARE" / "Microsoft" / "Non-Driver Signing");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Notepad" / "DefaultFonts",
                               machine / "SOFTWARE" / "Microsoft" / "Notepad" / "DefaultFonts");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Ole", machine / "SOFTWARE" / "Microsoft" / "Ole");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Phone", machine / "SOFTWARE" / "Microsoft" / "Phone");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Pim", machine / "SOFTWARE" / "Microsoft" / "Pim");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Poom", machine / "SOFTWARE" / "Microsoft" / "Poom");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Ras", machine / "SOFTWARE" / "Microsoft" / "Ras");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Rpc", machine / "SOFTWARE" / "Microsoft" / "Rpc");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "SecurityManager",
                               machine / "SOFTWARE" / "Microsoft" / "SecurityManager");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Semgr", machine / "SOFTWARE" / "Microsoft" / "Semgr");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Shell", machine / "SOFTWARE" / "Microsoft" / "Shell");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Software" / "Microsoft" / "Shared Tools" / "Msinfo",
                               machine / "SOFTWARE" / "Microsoft" / "Software" / "Microsoft" / "Shared Tools" / "Msinfo");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "SystemCertificates",
                               machine / "SOFTWARE" / "Microsoft" / "SystemCertificates");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "TermServLicensing",
                               machine / "SOFTWARE" / "Microsoft" / "TermServLicensing");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Transaction Server",
                               machine / "SOFTWARE" / "Microsoft" / "Transaction Server");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Unified Store",
                               machine / "SOFTWARE" / "Microsoft" / "Unified Store");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "UserData",
                               machine / "SOFTWARE" / "Microsoft" / "UserData");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Console",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Console");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Containers",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Containers");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "FontDPI",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "FontDPI");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "FontLink",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "FontLink");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "FontMapper",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "FontMapper");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Fonts",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Fonts");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "FontSubstitutes",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "FontSubstitutes");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "GRE_Initialize",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "GRE_Initialize");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" /
                                   "Image File Execution Options",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Image File Execution Options");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "LanguagePack",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "LanguagePack");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "NetworkCards",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "NetworkCards");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Perflib",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Perflib");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Ports",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Ports");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Print",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Print");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "ProfileList",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "ProfileList");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Time Zones",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Time Zones");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Virtualization" /
                                   "HvSocket",
                               machine / "SOFTWARE" / "Microsoft" / "Windows NT" / "CurrentVersion" / "Virtualization" / "HvSocket");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows Search",
                               machine / "SOFTWARE" / "Microsoft" / "Windows Search");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "App Paths",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "App Paths");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Control Panel" /
                                   "Cursors" / "Schemes",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Control Panel" / "Cursors" / "Schemes");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Control Panel" /
                                   "Theme",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Control Panel" / "Theme");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Control Panel" /
                                   "ThemeVolatile",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Control Panel" / "ThemeVolatile");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Explorer" /
                                   "AutoplayHandlers",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Explorer" / "AutoplayHandlers");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Explorer" /
                                   "DriveIcons",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Explorer" / "DriveIcons");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Explorer" / "KindMap",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Explorer" / "KindMap");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Group Policy",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Group Policy");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Parental Controls",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Parental Controls");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Policies",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Policies");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "PreviewHandlers",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "PreviewHandlers");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Setup",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Setup");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "Windows" / "CurrentVersion" / "Telephony" /
                                   "Locations",
                               machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "Telephony" / "Locations");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Microsoft" / "XAML", machine / "SOFTWARE" / "Microsoft" / "XAML");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "Policies", machine / "SOFTWARE" / "Policies");
        this->add_path_mapping(machine / "SOFTWARE" / "Wow6432Node" / "RegisteredApplications",
                               machine / "SOFTWARE" / "RegisteredApplications");

        this->alias_remote_audio_endpoints(machine);
    }

    // On a headless/server host the local Render/Capture endpoint folders are empty, while an RDP session
    // populates RemoteRender/RemoteCapture with the redirected device. mmdevapi resolves the default endpoint
    // by opening MMDevices\Audio\Render\{id} (or Capture\{id}), so alias each remote endpoint into the local
    // folder it expects, making the RDP audio device usable as the default playback/recording endpoint.
    void registry_manager::alias_remote_audio_endpoints(const std::filesystem::path& machine)
    {
        const auto audio = machine / "SOFTWARE" / "Microsoft" / "Windows" / "CurrentVersion" / "MMDevices" / "Audio";

        const std::array<std::pair<const char*, const char*>, 2> folders = {{
            {"Render", "RemoteRender"},
            {"Capture", "RemoteCapture"},
        }};

        for (const auto& [local, remote] : folders)
        {
            const auto remote_key = this->get_key(utils::path_key{audio / remote});
            if (!remote_key)
            {
                continue;
            }

            // RDP remote endpoints carry no DeviceState value; mmdevapi/audioses treat a missing or zero
            // state as an invalidated device, so mark each aliased endpoint active.
            for (size_t i = 0;; ++i)
            {
                const auto name = this->get_sub_key_name(*remote_key, i);
                if (!name)
                {
                    break;
                }

                const auto endpoint_key = this->get_key(utils::path_key{audio / remote / std::string(*name)});
                if (!endpoint_key)
                {
                    continue;
                }

                if (!this->get_value(*endpoint_key, "DeviceState"))
                {
                    constexpr uint32_t device_state_active = 1; // DEVICE_STATE_ACTIVE
                    const auto* state_bytes = reinterpret_cast<const std::byte*>(&device_state_active);
                    this->set_value(*endpoint_key, "DeviceState", 4 /* REG_DWORD */,
                                    std::span<const std::byte>(state_bytes, sizeof(device_state_active)));
                }

                // mmdevapi's endpoint enumeration (IMMDeviceEnumerator::EnumAudioEndpoints) only exposes a
                // device whose "Protocol" DWORD is 0 (or absent); RDP-redirected endpoints set a non-zero
                // Protocol to flag themselves remote, which makes the enumeration - and therefore the legacy
                // DirectSound device discovery built on top of it - skip them. Force it to 0 so the redirected
                // endpoint is enumerated as a local device.
                constexpr uint32_t protocol_local = 0;
                const auto* protocol_bytes = reinterpret_cast<const std::byte*>(&protocol_local);
                this->set_value(*endpoint_key, "Protocol", 4 /* REG_DWORD */,
                                std::span<const std::byte>(protocol_bytes, sizeof(protocol_local)));
            }

            // Expose the remote endpoints under the local folder. mmdevapi/WASAPI resolves a device by
            // opening Render\{id}, but DirectSound enumerates devices through the legacy waveOut/waveIn path
            // (winmm/wdmaud), which ENUMERATES the subkeys of Render\ - a per-endpoint path mapping redirects
            // lookups but is not enumerable, so the wave layer would see zero devices (-> DSERR_NODRIVER). A
            // headless/RDP host has an empty local Render folder, so alias the whole folder (covers both
            // lookups and enumeration); only if the host has real local endpoints do we fall back to
            // per-endpoint lookup mappings so we don't shadow them.
            const auto local_key = this->get_key(utils::path_key{audio / local});
            const bool local_empty = !local_key || !this->get_sub_key_name(*local_key, 0).has_value();
            if (local_empty)
            {
                this->add_path_mapping(audio / local, audio / remote);
            }
            else
            {
                for (size_t i = 0;; ++i)
                {
                    const auto name = this->get_sub_key_name(*remote_key, i);
                    if (!name)
                    {
                        break;
                    }

                    this->add_path_mapping(audio / local / std::string(*name), audio / remote / std::string(*name));
                }
            }
        }
    }

    utils::path_key registry_manager::normalize_path(utils::path_key path) const
    {
        for (size_t i = 0; i < 10; ++i)
        {
            auto [new_path, changed] = perform_path_substitution(this->path_mapping_, std::move(path));
            path = std::move(new_path);

            if (!changed)
            {
                break;
            }
        }

        return path;
    }

    void registry_manager::add_path_mapping(const utils::path_key& key, const utils::path_key& value)
    {
        this->path_mapping_[key] = value;
    }

    utils::path_key registry_manager::get_full_key_path(const registry_key& key)
    {
        return utils::path_key{key.hive.get() / key.path.get()};
    }

    std::optional<registry_key> registry_manager::get_key(const utils::path_key& key)
    {
        const auto normal_key = this->normalize_path(key);

        if (is_subpath(normal_key, utils::path_key{"\\registry\\machine"}) || is_subpath(normal_key, utils::path_key{"\\registry\\user"}))
        {
            registry_key reg_key{};
            reg_key.hive = normal_key;
            return {std::move(reg_key)};
        }

        const auto iterator = this->find_hive(normal_key);
        if (iterator == this->hives_.end())
        {
            return {};
        }

        registry_key reg_key{};
        reg_key.hive = iterator->first.get();
        reg_key.path = normal_key.get().lexically_relative(reg_key.hive.get());

        if (reg_key.path.get().empty())
        {
            return {std::move(reg_key)};
        }

        auto path = reg_key.path.get();
        const auto* entry = iterator->second->get_sub_key(path);

        if (!entry)
        {
            return std::nullopt;
        }

        return {std::move(reg_key)};
    }

    std::optional<registry_value> registry_manager::get_value(const registry_key& key, const std::string_view name)
    {
        if (const auto overlay_entry = this->overlay_values_.find(registry_manager::get_full_key_path(key));
            overlay_entry != this->overlay_values_.end())
        {
            if (const auto value_entry = overlay_entry->second.values.find(std::string{name});
                value_entry != overlay_entry->second.values.end())
            {
                registry_value v{};
                v.type = value_entry->second.type;
                v.name = value_entry->first;
                v.data = value_entry->second.data;
                return v;
            }
        }

        const auto iterator = this->hives_.find(key.hive);
        if (iterator == this->hives_.end())
        {
            return std::nullopt;
        }

        const auto* entry = iterator->second->get_value(key.path.get(), name);
        if (!entry)
        {
            return std::nullopt;
        }

        registry_value v{};
        v.type = entry->type;
        v.name = entry->name;
        v.data = entry->data;

        return v;
    }

    std::optional<registry_value> registry_manager::get_value(const registry_key& key, const size_t index)
    {
        const auto iterator = this->hives_.find(key.hive);
        if (iterator == this->hives_.end())
        {
            return std::nullopt;
        }

        const auto* entry = iterator->second->get_value(key.path.get(), index);
        if (!entry)
        {
            return std::nullopt;
        }

        registry_value v{};
        v.type = entry->type;
        v.name = entry->name;
        v.data = entry->data;

        return v;
    }

    void registry_manager::set_value(const registry_key& key, std::string name, const uint32_t type, const std::span<const std::byte> data)
    {
        auto& bucket = this->overlay_values_[registry_manager::get_full_key_path(key)];
        auto& value = bucket.values[std::move(name)];
        value.type = type;
        value.data.assign(data.begin(), data.end());
    }

    void registry_manager::serialize_runtime_state(utils::buffer_serializer& buffer) const
    {
        buffer.write_map(this->overlay_values_);
    }

    void registry_manager::deserialize_runtime_state(utils::buffer_deserializer& buffer)
    {
        buffer.read_map(this->overlay_values_);
    }

    registry_manager::hive_map::iterator registry_manager::find_hive(const utils::path_key& key)
    {
        for (auto i = this->hives_.begin(); i != this->hives_.end(); ++i)
        {
            if (is_subpath(i->first, key))
            {
                return i;
            }
        }

        return this->hives_.end();
    }

    std::optional<exposed_hive_key> registry_manager::get_hive_key(const registry_key& key)
    {
        const auto iterator = this->hives_.find(key.hive);
        if (iterator == this->hives_.end())
        {
            return std::nullopt;
        }

        auto* hive_key = iterator->second->get_sub_key(key.path.get());
        if (!hive_key)
        {
            return std::nullopt;
        }

        return exposed_hive_key{
            .key = *hive_key,
            .file = iterator->second->get_file(),
        };
    }

    std::optional<std::string_view> registry_manager::get_sub_key_name(const registry_key& key, const size_t index)
    {
        const auto iterator = this->hives_.find(key.hive);
        if (iterator == this->hives_.end())
        {
            return std::nullopt;
        }

        const auto* name = iterator->second->get_sub_key_name(key.path.get(), index);
        if (!name)
        {
            return std::nullopt;
        }

        return *name;
    }

    std::optional<std::u16string> registry_manager::read_u16string(const registry_key& key, size_t index)
    {
        const auto value_opt = this->get_value(key, index);
        if (!value_opt)
        {
            return {};
        }

        return registry_utils::decode_registry_string(*value_opt);
    }

} // namespace sogen

#include "mount_spec.hpp"

namespace sogen
{

    namespace
    {
        bool is_drive_letter(const char chr)
        {
            return (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z');
        }

        std::optional<size_t> find_guest_path(const std::string_view spec)
        {
            for (size_t i = spec.size(); i > 3; --i)
            {
                const auto drive_separator = i - 1;
                if (spec[drive_separator] == ':' && is_drive_letter(spec[drive_separator - 1]) && spec[drive_separator - 2] == ':')
                {
                    return drive_separator - 1;
                }
            }

            return std::nullopt;
        }
    }

    mount_spec parse_mount_spec(const std::string_view spec)
    {
        mount_spec result{};
        auto rest = spec;

        // MODE is split off at the last ':', so a GUEST that names an NTFS alternate data stream is ambiguous:
        // "c:\file.txt:ro" reads as a read-only mount of c:\file.txt. Mounting a stream is not supported, and
        // other stream names are rejected below.
        if (const auto separator = rest.rfind(':'); separator != std::string_view::npos)
        {
            if (const auto mode = parse_filesystem_mode(rest.substr(separator + 1)))
            {
                result.mode = *mode;
                rest = rest.substr(0, separator);
            }
        }

        const auto guest_start = find_guest_path(rest);
        if (!guest_start)
        {
            throw std::runtime_error("Mount must have the form HOST:GUEST[:MODE] with a guest drive letter: " + std::string(spec));
        }

        const auto host = rest.substr(0, *guest_start - 1);
        const auto guest = rest.substr(*guest_start);

        if (guest.find(':', 2) != std::string_view::npos)
        {
            throw std::runtime_error("Mount GUEST cannot contain ':' after the drive, and MODE must be ro or rw: " + std::string(spec));
        }

        result.host_path = std::filesystem::path(std::u8string(host.begin(), host.end()));
        result.guest_path = windows_path{std::u8string(guest.begin(), guest.end())};
        return result;
    }

    std::string render_mount_spec(const mount_spec& spec)
    {
        const auto host = spec.host_path.u8string();
        return std::string(host.begin(), host.end()) + ":" + spec.guest_path.string() + ":" + std::string(to_string(spec.mode));
    }

} // namespace sogen

#include "macos_input.hpp"

#include "macos_bundle.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <string_view>

namespace sogen
{
    namespace
    {
        // UDIF ("koly") trailer: 4-byte magic, u32 version, u32 header size (512), at the very end of the file.
        // The file's leading bytes are compressed payload with no fixed magic, so the trailer is the only marker.
        constexpr size_t UDIF_TRAILER_SIZE = 512;
        constexpr std::string_view UDIF_MAGIC = "koly";
        constexpr std::string_view ENCRYPTED_MAGIC = "encrcdsa";

        bool read_at(std::ifstream& stream, const uint64_t offset, const std::span<char> buffer)
        {
            stream.seekg(static_cast<std::streamoff>(offset));
            if (!stream)
            {
                return false;
            }

            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            return stream.gcount() == static_cast<std::streamsize>(buffer.size());
        }

        bool has_dmg_extension(const std::filesystem::path& path)
        {
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(),
                                   [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return extension == ".dmg";
        }
    }

    macos_input_kind classify_macos_input(const std::filesystem::path& path)
    {
        std::error_code error{};
        const auto status = std::filesystem::status(path, error);
        if (error || status.type() == std::filesystem::file_type::not_found)
        {
            return macos_input_kind::missing;
        }

        if (std::filesystem::is_directory(status))
        {
            return is_app_bundle_path(path) ? macos_input_kind::app_bundle : macos_input_kind::directory;
        }

        if (!std::filesystem::is_regular_file(status))
        {
            return macos_input_kind::special_file;
        }

        const auto size = std::filesystem::file_size(path, error);
        if (error)
        {
            return macos_input_kind::unknown;
        }

        std::ifstream stream{path, std::ios::binary};
        if (!stream)
        {
            return macos_input_kind::unknown;
        }

        std::array<char, 8> head{};
        if (size >= head.size() && read_at(stream, 0, head))
        {
            if (std::string_view{head.data(), ENCRYPTED_MAGIC.size()} == ENCRYPTED_MAGIC)
            {
                return macos_input_kind::encrypted_disk_image;
            }

            uint32_t magic = 0;
            std::memcpy(&magic, head.data(), sizeof(magic));
            switch (magic)
            {
            case 0xFEEDFACFU:
            case 0xCFFAEDFEU:
            case 0xFEEDFACEU:
            case 0xCEFAEDFEU:
                return macos_input_kind::mach_o;
            case 0xCAFEBABEU:
            case 0xBEBAFECAU:
            case 0xCAFEBABFU:
            case 0xBFBAFECAU:
                return macos_input_kind::fat_mach_o;
            default:
                break;
            }
        }

        if (size >= UDIF_TRAILER_SIZE)
        {
            std::array<char, 4> trailer{};
            if (read_at(stream, size - UDIF_TRAILER_SIZE, trailer) && std::string_view{trailer.data(), trailer.size()} == UDIF_MAGIC)
            {
                return macos_input_kind::disk_image;
            }
        }

        if (has_dmg_extension(path))
        {
            return macos_input_kind::disk_image;
        }

        return macos_input_kind::unknown;
    }

    std::string describe_unsupported_input(const std::filesystem::path& path, const macos_input_kind kind)
    {
        const auto name = path.string();

        switch (kind)
        {
        case macos_input_kind::disk_image:
            return "'" + name +
                   "' is an Apple Disk Image, not a Mach-O executable or an .app bundle.\n"
                   "sogen does not mount disk images: mounting runs the host kernel's filesystem parsers on the\n"
                   "sample's own bytes, outside the emulator. Mount it yourself and point sogen at the bundle inside:\n"
                   "  hdiutil attach -readonly -nobrowse \"" +
                   name +
                   "\"\n"
                   "  analyzer --os=macos \"/Volumes/<Volume>/<App>.app\"\n"
                   "  hdiutil detach \"/Volumes/<Volume>\"\n"
                   "Or let the helper do all three: src/tools/run-macos-dmg.sh \"" +
                   name + "\"";

        case macos_input_kind::encrypted_disk_image:
            return "'" + name +
                   "' is an encrypted Apple Disk Image. sogen does not mount or decrypt disk images.\n"
                   "Mount it yourself - hdiutil will prompt for its password - and point sogen at the bundle inside:\n"
                   "  hdiutil attach -readonly -nobrowse \"" +
                   name + "\"";

        case macos_input_kind::directory:
            return "'" + name + "' is a directory but not an .app bundle. Pass the executable inside it, or the .app itself.";

        case macos_input_kind::special_file:
            return "'" + name + "' is not a regular file. sogen only runs regular files and .app bundles.";

        case macos_input_kind::missing:
            return "'" + name + "' does not exist.";

        case macos_input_kind::unknown:
            return "'" + name + "' is not a Mach-O executable, an .app bundle or a disk image.";

        case macos_input_kind::app_bundle:
        case macos_input_kind::mach_o:
        case macos_input_kind::fat_mach_o:
            break;
        }

        return {};
    }
}

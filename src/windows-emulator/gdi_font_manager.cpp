#include "std_include.hpp"
#include "gdi_font_manager.hpp"
#include "file_system.hpp"
#include "emulator_utils.hpp"
#include "registry/registry_utils.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_SYNTHESIS_H
#include FT_OUTLINE_H

#include <list>
#include <set>

namespace sogen
{
    namespace
    {
        constexpr size_t maximum_file_bytes = 64u << 20;
        constexpr size_t maximum_total_bytes = 256u << 20;
        constexpr size_t maximum_files = 128;
        constexpr size_t maximum_faces = 64;
        constexpr uint32_t maximum_pixels = 4096;
        constexpr size_t maximum_bitmap_bytes = 4u << 20;
        constexpr size_t maximum_cache_bytes = 16u << 20;
        constexpr size_t maximum_cache_entries = 2048;
        constexpr size_t maximum_selections = 65536;
        constexpr std::u16string_view substitutes_key =
            uR"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes)";
        constexpr std::u16string_view fonts_key = uR"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion\Fonts)";

        std::u16string normalized(std::u16string name)
        {
            for (auto& ch : name)
            {
                if (ch >= u'A' && ch <= u'Z')
                {
                    ch = static_cast<char16_t>(ch + (u'a' - u'A'));
                }
            }
            return name;
        }

        struct library_deleter
        {
            void operator()(FT_Library library) const
            {
                FT_Done_FreeType(library);
            }
        };

        struct face_deleter
        {
            void operator()(FT_Face face) const
            {
                FT_Done_Face(face);
            }
        };

        using library_pointer = std::unique_ptr<std::remove_pointer_t<FT_Library>, library_deleter>;
        using face_pointer = std::unique_ptr<std::remove_pointer_t<FT_Face>, face_deleter>;

        void require_ft(const FT_Error error)
        {
            if (error != 0)
            {
                throw std::runtime_error("FreeType error " + std::to_string(error));
            }
        }

        int32_t pixels(const FT_Pos value)
        {
            return static_cast<int32_t>((value >= 0 ? value + 32 : value - 32) / 64);
        }

        std::u16string sfnt_name(const FT_SfntName& name)
        {
            std::u16string result{};
            if (name.string_len > 1024)
            {
                return result;
            }
            if (name.platform_id == 0 ||
                (name.platform_id == 3 && (name.encoding_id == 0 || name.encoding_id == 1 || name.encoding_id == 10)))
            {
                if (name.string_len % 2 != 0)
                {
                    return result;
                }
                for (FT_UInt i = 0; i < name.string_len; i += 2)
                {
                    result.push_back(static_cast<char16_t>((static_cast<uint16_t>(name.string[i]) << 8) | name.string[i + 1]));
                }
            }
            else if (name.platform_id == 1 && name.encoding_id == 0)
            {
                for (FT_UInt i = 0; i < name.string_len; ++i)
                {
                    if (name.string[i] >= 128)
                    {
                        return {};
                    }
                    result.push_back(static_cast<char16_t>(name.string[i]));
                }
            }
            return result;
        }

        struct physical_face
        {
            face_pointer handle{};
            std::u16string family{};
            std::set<std::u16string> names{};
        };

        struct font_file
        {
            windows_path path{};
            std::vector<std::byte> bytes{};
            std::vector<physical_face> faces{};
        };

        std::unique_ptr<font_file> parse_file(FT_Library library, windows_path path, std::vector<std::byte> bytes)
        {
            if (bytes.empty() || bytes.size() > maximum_file_bytes)
            {
                throw std::invalid_argument("Invalid font file size");
            }
            auto result = std::make_unique<font_file>();
            result->path = std::move(path);
            result->bytes = std::move(bytes);
            FT_Face raw = nullptr;
            require_ft(FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(result->bytes.data()),
                                          static_cast<FT_Long>(result->bytes.size()), 0, &raw));
            face_pointer first{raw};
            if (!FT_IS_SFNT(raw) || !FT_IS_SCALABLE(raw) || raw->num_faces <= 0 || raw->num_faces > static_cast<FT_Long>(maximum_faces))
            {
                throw std::invalid_argument("Unsupported font collection");
            }
            const auto count = static_cast<uint32_t>(raw->num_faces);
            for (uint32_t index = 0; index < count; ++index)
            {
                physical_face face{};
                if (index == 0)
                {
                    face.handle = std::move(first);
                }
                else
                {
                    raw = nullptr;
                    require_ft(FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(result->bytes.data()),
                                                  static_cast<FT_Long>(result->bytes.size()), static_cast<FT_Long>(index), &raw));
                    face.handle.reset(raw);
                }
                raw = face.handle.get();
                if (!FT_IS_SFNT(raw) || !FT_IS_SCALABLE(raw) || raw->num_glyphs <= 0 || raw->num_glyphs > 65535 || raw->units_per_EM == 0)
                {
                    throw std::invalid_argument("Unsupported font face");
                }
                require_ft(FT_Select_Charmap(raw, FT_ENCODING_UNICODE));
                const auto names = FT_Get_Sfnt_Name_Count(raw);
                for (FT_UInt i = 0; i < std::min<FT_UInt>(names, 4096); ++i)
                {
                    FT_SfntName record{};
                    if (FT_Get_Sfnt_Name(raw, i, &record) == 0 && (record.name_id == 1 || record.name_id == 4 || record.name_id == 16))
                    {
                        auto name = sfnt_name(record);
                        if (!name.empty())
                        {
                            if (face.family.empty() && (record.name_id == 1 || record.name_id == 16))
                            {
                                face.family = name;
                            }
                            face.names.insert(normalized(std::move(name)));
                        }
                    }
                }
                if (face.family.empty() && raw->family_name != nullptr && std::char_traits<char>::length(raw->family_name) <= 1024)
                {
                    face.family = u8_to_u16(raw->family_name);
                    face.names.insert(normalized(face.family));
                }
                result->faces.push_back(std::move(face));
            }
            return result;
        }

        std::vector<std::byte> read_font(file_system& files, const windows_path& path)
        {
            auto input = files.open_read(path);
            if (!input.file)
            {
                return {};
            }
            const auto size = input.file->size();
            if (size <= 0 || static_cast<uint64_t>(size) > maximum_file_bytes)
            {
                return {};
            }
            std::vector<std::byte> bytes(static_cast<size_t>(size));
            size_t offset = 0;
            while (offset < bytes.size())
            {
                const auto count = input.file->read(std::span{bytes}.subspan(offset));
                if (count == 0 || count > bytes.size() - offset)
                {
                    return {};
                }
                offset += count;
            }
            return bytes;
        }

        std::u16string substitute(registry_manager& registry, std::u16string family)
        {
            const auto key = registry.get_key(utils::path_key{std::filesystem::path{substitutes_key}});
            if (!key)
            {
                return family;
            }
            std::set<std::u16string> visited{};
            for (size_t i = 0; i < 16 && visited.insert(normalized(family)).second; ++i)
            {
                const auto value = registry.get_value(*key, u16_to_u8(family));
                if (!value || value->data.size() > 514)
                {
                    return family;
                }
                const auto next = registry_utils::decode_registry_string(*value);
                if (!next || next->empty() || next->size() > 256)
                {
                    return family;
                }
                family = *next;
            }
            return family;
        }

        gdi_font_reference sized_reference(const uint32_t file, const uint32_t index, FT_Face face, const EMU_LOGFONTW& request)
        {
            const auto height = request.lfHeight == 0 ? int64_t{16} : std::abs(static_cast<int64_t>(request.lfHeight));
            const auto width = std::abs(static_cast<int64_t>(request.lfWidth));
            if (height > maximum_pixels || width > maximum_pixels)
            {
                throw std::invalid_argument("Font size exceeds limit");
            }
            auto em_height = height;
            if (request.lfHeight > 0)
            {
                const auto cell = static_cast<int64_t>(face->ascender) - face->descender;
                if (cell <= 0)
                {
                    throw std::invalid_argument("Invalid font height");
                }
                em_height = std::max<int64_t>(1, height * face->units_per_EM / cell);
            }
            int64_t em_width = 0;
            if (width != 0)
            {
                const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, ft_sfnt_os2));
                const auto average = os2 && os2->xAvgCharWidth > 0 ? os2->xAvgCharWidth : face->max_advance_width;
                if (average <= 0)
                {
                    throw std::invalid_argument("Invalid font width");
                }
                em_width = std::max<int64_t>(1, width * face->units_per_EM / average);
            }
            if (em_height > maximum_pixels || em_width > maximum_pixels)
            {
                throw std::invalid_argument("Scaled font size exceeds limit");
            }
            return {file,
                    index,
                    static_cast<uint32_t>(em_width),
                    static_cast<uint32_t>(em_height),
                    request.lfWeight >= FW_BOLD && (face->style_flags & FT_STYLE_FLAG_BOLD) == 0,
                    request.lfItalic != 0 && (face->style_flags & FT_STYLE_FLAG_ITALIC) == 0};
        }

        gdi_glyph_metrics slot_metrics(FT_GlyphSlot slot)
        {
            return {static_cast<int32_t>(slot->advance.x),
                    static_cast<int32_t>(slot->advance.y),
                    static_cast<int32_t>(slot->metrics.horiBearingX),
                    static_cast<int32_t>(slot->metrics.horiBearingY),
                    static_cast<int32_t>(slot->metrics.width),
                    static_cast<int32_t>(slot->metrics.height)};
        }

        void write_text(utils::buffer_serializer& buffer, const std::u16string_view text)
        {
            if (text.size() > 1024)
            {
                throw std::length_error("Saved font name exceeds limit");
            }
            buffer.write(static_cast<uint32_t>(text.size()));
            buffer.write(text.data(), text.size() * sizeof(char16_t));
        }

        std::u16string read_text(utils::buffer_deserializer& buffer)
        {
            const auto length = buffer.read<uint32_t>();
            if (length > 1024 || length > buffer.get_remaining_size() / sizeof(char16_t))
            {
                throw std::invalid_argument("Invalid saved font name");
            }
            std::u16string result(length, u'\0');
            buffer.read(result.data(), result.size() * sizeof(char16_t));
            return result;
        }

        void write_reference(utils::buffer_serializer& buffer, const gdi_font_reference& ref)
        {
            buffer.write(ref.file);
            buffer.write(ref.face);
            buffer.write(ref.pixel_width);
            buffer.write(ref.pixel_height);
            buffer.write(ref.embolden);
            buffer.write(ref.oblique);
        }

        gdi_font_reference read_reference(utils::buffer_deserializer& buffer)
        {
            gdi_font_reference result{};
            buffer.read(result.file);
            buffer.read(result.face);
            buffer.read(result.pixel_width);
            buffer.read(result.pixel_height);
            buffer.read(result.embolden);
            buffer.read(result.oblique);
            return result;
        }
    }

    struct gdi_font_manager::implementation
    {
        struct selection_key
        {
            std::u16string system_root{};
            std::u16string family{};
            int32_t height{};
            int32_t width{};
            int32_t weight{};
            bool italic{};
            uint8_t charset{};
            uint32_t character{};

            auto operator<=>(const selection_key&) const = default;

            void serialize(utils::buffer_serializer& buffer) const
            {
                write_text(buffer, this->system_root);
                write_text(buffer, this->family);
                buffer.write(this->height);
                buffer.write(this->width);
                buffer.write(this->weight);
                buffer.write(this->italic);
                buffer.write(this->charset);
                buffer.write(this->character);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                this->system_root = read_text(buffer);
                this->family = read_text(buffer);
                buffer.read(this->height);
                buffer.read(this->width);
                buffer.read(this->weight);
                buffer.read(this->italic);
                buffer.read(this->charset);
                buffer.read(this->character);
            }
        };

        using glyph_key = std::pair<gdi_font_reference, uint32_t>;

        struct cached_glyph
        {
            std::shared_ptr<const gdi_glyph_bitmap> bitmap{};
            std::list<glyph_key>::iterator position{};
        };

        library_pointer library{};
        std::map<uint32_t, std::unique_ptr<font_file>> files{};
        std::map<selection_key, gdi_font_reference> selections{};
        std::list<glyph_key> recent{};
        std::map<glyph_key, cached_glyph> glyphs{};
        size_t file_bytes{};
        size_t cache_bytes{};

        implementation()
        {
            FT_Library raw = nullptr;
            require_ft(FT_Init_FreeType(&raw));
            this->library.reset(raw);
        }

        FT_Face face(const gdi_font_reference& ref)
        {
            const auto file = this->files.find(ref.file);
            if (file == this->files.end() || ref.face >= file->second->faces.size() || ref.pixel_height == 0 ||
                ref.pixel_height > maximum_pixels || ref.pixel_width > maximum_pixels)
            {
                throw std::invalid_argument("Invalid font reference");
            }
            auto result = file->second->faces[ref.face].handle.get();
            require_ft(FT_Set_Pixel_Sizes(result, ref.pixel_width, ref.pixel_height));
            return result;
        }

        FT_GlyphSlot load(const gdi_font_reference& ref, const uint32_t index)
        {
            const auto current = this->face(ref);
            if (index >= static_cast<uint32_t>(current->num_glyphs))
            {
                throw std::invalid_argument("Invalid glyph index");
            }
            require_ft(FT_Load_Glyph(current, index, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL));
            if (current->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
            {
                throw std::runtime_error("Unsupported glyph outline format");
            }
            if (ref.embolden)
            {
                FT_GlyphSlot_Embolden(current->glyph);
            }
            if (ref.oblique)
            {
                FT_GlyphSlot_Oblique(current->glyph);
                FT_BBox box{};
                FT_Outline_Get_CBox(&current->glyph->outline, &box);
                current->glyph->metrics.horiBearingX = box.xMin;
                current->glyph->metrics.horiBearingY = box.yMax;
                current->glyph->metrics.width = box.xMax - box.xMin;
                current->glyph->metrics.height = box.yMax - box.yMin;
            }
            FT_BBox bounds{};
            FT_Outline_Get_CBox(&current->glyph->outline, &bounds);
            constexpr auto limit = static_cast<FT_Pos>(maximum_pixels) * 64;
            if (bounds.xMin < -limit || bounds.yMin < -limit || bounds.xMax > limit || bounds.yMax > limit ||
                bounds.xMax - bounds.xMin > limit || bounds.yMax - bounds.yMin > limit)
            {
                throw std::length_error("Glyph outline exceeds limit");
            }
            return current->glyph;
        }

        std::optional<gdi_font_reference> choose(font_file& file, const uint32_t id, const std::u16string& family,
                                                 const EMU_LOGFONTW& request, const char32_t character, const bool exact)
        {
            std::optional<gdi_font_reference> result{};
            unsigned best = std::numeric_limits<unsigned>::max();
            for (uint32_t i = 0; i < file.faces.size(); ++i)
            {
                const auto& face = file.faces[i];
                if ((exact && !face.names.contains(family)) ||
                    (character != 0 && FT_Get_Char_Index(face.handle.get(), static_cast<FT_ULong>(character)) == 0))
                {
                    continue;
                }
                const auto flags = face.handle->style_flags;
                const unsigned score = ((request.lfWeight >= FW_BOLD) != ((flags & FT_STYLE_FLAG_BOLD) != 0) ? 2u : 0u) +
                                       ((request.lfItalic != 0) != ((flags & FT_STYLE_FLAG_ITALIC) != 0) ? 1u : 0u);
                if (score < best)
                {
                    result = sized_reference(id, i, face.handle.get(), request);
                    best = score;
                }
            }
            return result;
        }

        std::optional<gdi_font_reference> candidate(file_system& source, const windows_path& path, const std::u16string& family,
                                                    const EMU_LOGFONTW& request, const char32_t character, const bool exact)
        {
            for (const auto& [id, file] : this->files)
            {
                if (file->path == path)
                {
                    return this->choose(*file, id, family, request, character, exact);
                }
            }
            auto bytes = read_font(source, path);
            if (bytes.empty())
            {
                return std::nullopt;
            }
            std::unique_ptr<font_file> file{};
            try
            {
                file = parse_file(this->library.get(), path, std::move(bytes));
            }
            catch (const std::runtime_error&)
            {
                return std::nullopt;
            }
            catch (const std::invalid_argument&)
            {
                return std::nullopt;
            }
            const auto id = static_cast<uint32_t>(this->files.size() + 1);
            const auto selected = this->choose(*file, id, family, request, character, exact);
            if (!selected)
            {
                return std::nullopt;
            }
            if (this->files.size() >= maximum_files || file->bytes.size() > maximum_total_bytes - this->file_bytes)
            {
                throw std::length_error("Font resource limit exceeded");
            }
            const auto size = file->bytes.size();
            this->files.emplace(id, std::move(file));
            this->file_bytes += size;
            return selected;
        }
    };

    gdi_font_manager::gdi_font_manager() = default;
    gdi_font_manager::~gdi_font_manager() = default;
    gdi_font_manager::gdi_font_manager(gdi_font_manager&&) noexcept = default;
    gdi_font_manager& gdi_font_manager::operator=(gdi_font_manager&&) noexcept = default;

    gdi_font_manager::implementation& gdi_font_manager::get()
    {
        if (!this->impl_)
        {
            this->impl_ = std::make_unique<implementation>();
        }
        return *this->impl_;
    }

    std::optional<gdi_font_reference> gdi_font_manager::resolve(file_system& source, registry_manager& registry,
                                                                const windows_path& system_root, const EMU_LOGFONTW& request,
                                                                const char32_t required_character)
    {
        if (!system_root.is_absolute() || system_root.u16string().size() > 1024 || required_character > 0x10FFFF ||
            (required_character >= 0xD800 && required_character <= 0xDFFF) ||
            std::abs(static_cast<int64_t>(request.lfHeight)) > maximum_pixels ||
            std::abs(static_cast<int64_t>(request.lfWidth)) > maximum_pixels)
        {
            throw std::invalid_argument("Invalid font request");
        }
        auto& state = this->get();
        const auto requested = std::u16string{utils::string::to_string_view(std::span<const char16_t>{request.lfFaceName})};
        const auto family = normalized(substitute(registry, requested));
        const implementation::selection_key key{system_root.u16string(), family,
                                                request.lfHeight,        request.lfWidth,
                                                request.lfWeight,        request.lfItalic != 0,
                                                request.lfCharSet,       static_cast<uint32_t>(required_character)};
        if (const auto selected = state.selections.find(key); selected != state.selections.end())
        {
            return selected->second;
        }
        const auto remember = [&](std::optional<gdi_font_reference> ref) {
            if (ref && state.selections.size() < maximum_selections)
            {
                state.selections.emplace(key, *ref);
            }
            return ref;
        };
        std::optional<gdi_font_reference> closest{};
        unsigned closest_score = std::numeric_limits<unsigned>::max();
        const auto consider = [&](const gdi_font_reference& ref) {
            const auto flags = state.files.at(ref.file)->faces.at(ref.face).handle->style_flags;
            const unsigned score = ((request.lfWeight >= FW_BOLD) != ((flags & FT_STYLE_FLAG_BOLD) != 0) ? 2u : 0u) +
                                   ((request.lfItalic != 0) != ((flags & FT_STYLE_FLAG_ITALIC) != 0) ? 1u : 0u);
            if (score < closest_score)
            {
                closest = ref;
                closest_score = score;
            }
            return score == 0;
        };
        for (const auto& [id, file] : state.files)
        {
            if (auto selected = state.choose(*file, id, family, request, required_character, true); selected && consider(*selected))
            {
                return remember(selected);
            }
        }

        const auto folder = system_root / "Fonts";
        std::vector<windows_path> candidates{};
        const auto add = [&](windows_path path) {
            if (std::ranges::find(candidates, path) == candidates.end())
            {
                candidates.push_back(std::move(path));
            }
        };
        if (const auto fonts = registry.get_key(utils::path_key{std::filesystem::path{fonts_key}}))
        {
            for (size_t i = 0; i < std::min<size_t>(registry.get_value_count(*fonts), 4096); ++i)
            {
                const auto value = registry.get_value(*fonts, i);
                if (!value || value->data.size() > 2050 || family.empty() ||
                    normalized(u8_to_u16(value->name)).find(family) == std::u16string::npos)
                {
                    continue;
                }
                auto path = registry_utils::decode_registry_string(*value);
                if (!path || path->empty() || path->size() > 1024)
                {
                    continue;
                }
                const auto lower = normalized(*path);
                for (const auto prefix : {u"%systemroot%"sv, u"%windir%"sv})
                {
                    if (lower.starts_with(prefix))
                    {
                        *path = system_root.u16string() + path->substr(prefix.size());
                        break;
                    }
                }
                const windows_path candidate{*path};
                add(candidate.is_absolute() ? candidate : folder / candidate);
            }
        }
        auto names = source.list_directory_names(folder);
        if (names.size() > 4096)
        {
            throw std::length_error("Font directory limit exceeded");
        }
        std::ranges::sort(names, {}, [](const auto& name) { return normalized(name); });
        for (const auto& name : names)
        {
            const auto lower = normalized(name);
            if (lower.ends_with(u".ttf") || lower.ends_with(u".ttc") || lower.ends_with(u".otf"))
            {
                add(folder / windows_path{name});
            }
        }
        if (!family.empty())
        {
            for (const auto& path : candidates)
            {
                if (auto selected = state.candidate(source, path, family, request, required_character, true);
                    selected && consider(*selected))
                {
                    return remember(selected);
                }
            }
        }
        if (closest)
        {
            return remember(closest);
        }
        const auto western = request.lfWeight >= FW_BOLD ? (request.lfItalic ? u"segoeuiz.ttf" : u"segoeuib.ttf")
                                                         : (request.lfItalic ? u"segoeuii.ttf" : u"segoeui.ttf");
        const auto cjk = request.lfWeight >= FW_BOLD ? u"msyhbd.ttc" : u"msyh.ttc";
        const auto preferred = required_character >= 0x2E80 || request.lfCharSet == GB2312_CHARSET
                                   ? std::vector<std::u16string>{cjk, u"msyh.ttc", u"simsun.ttc", u"simhei.ttf", western}
                                   : std::vector<std::u16string>{western, u"segoeui.ttf", cjk, u"msyh.ttc", u"simsun.ttc", u"simhei.ttf"};
        std::vector<windows_path> fallback{};
        for (const auto& name : preferred)
        {
            fallback.push_back(folder / windows_path{name});
        }
        for (const auto& [id, file] : state.files)
        {
            (void)id;
            fallback.push_back(file->path);
        }
        fallback.insert(fallback.end(), candidates.begin(), candidates.end());
        std::set<std::u16string> visited{};
        for (const auto& path : fallback)
        {
            if (visited.insert(path.u16string()).second)
            {
                if (auto selected = state.candidate(source, path, family, request, required_character, false))
                {
                    return remember(selected);
                }
            }
        }
        return std::nullopt;
    }

    uint32_t gdi_font_manager::glyph_index(const gdi_font_reference& font, const char32_t character)
    {
        if (character > 0x10FFFF || (character >= 0xD800 && character <= 0xDFFF))
        {
            return 0;
        }
        return FT_Get_Char_Index(this->get().face(font), static_cast<FT_ULong>(character));
    }

    gdi_font_metrics gdi_font_manager::metrics(const gdi_font_reference& font)
    {
        const auto face = this->get().face(font);
        const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, ft_sfnt_os2));
        const auto average = os2 && os2->xAvgCharWidth > 0 ? os2->xAvgCharWidth : face->max_advance_width;
        return {pixels(face->size->metrics.ascender),    -pixels(face->size->metrics.descender),
                pixels(face->size->metrics.height),      pixels(FT_MulFix(average, face->size->metrics.x_scale)),
                pixels(face->size->metrics.max_advance), FT_IS_FIXED_WIDTH(face) != 0};
    }

    gdi_glyph_metrics gdi_font_manager::glyph_metrics(const gdi_font_reference& font, const uint32_t index)
    {
        return slot_metrics(this->get().load(font, index));
    }

    std::shared_ptr<const gdi_glyph_bitmap> gdi_font_manager::rasterize(const gdi_font_reference& font, const uint32_t index)
    {
        auto& state = this->get();
        const implementation::glyph_key key{font, index};
        if (const auto cached = state.glyphs.find(key); cached != state.glyphs.end())
        {
            state.recent.splice(state.recent.end(), state.recent, cached->second.position);
            return cached->second.bitmap;
        }
        const auto slot = state.load(font, index);
        auto result = std::make_shared<gdi_glyph_bitmap>();
        result->metrics = slot_metrics(slot);
        FT_BBox bounds{};
        FT_Outline_Get_CBox(&slot->outline, &bounds);
        const auto bitmap_width = static_cast<uint64_t>((bounds.xMax - bounds.xMin + 63) / 64 + 2);
        const auto bitmap_height = static_cast<uint64_t>((bounds.yMax - bounds.yMin + 63) / 64 + 2);
        if (bitmap_width * bitmap_height > maximum_bitmap_bytes)
        {
            throw std::length_error("Glyph bitmap limit exceeded");
        }
        require_ft(FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL));
        const auto& bitmap = slot->bitmap;
        const auto size = static_cast<uint64_t>(bitmap.width) * bitmap.rows;
        if (size > maximum_bitmap_bytes || bitmap.width > maximum_pixels || bitmap.rows > maximum_pixels)
        {
            throw std::length_error("Glyph bitmap limit exceeded");
        }
        result->left = slot->bitmap_left;
        result->top = slot->bitmap_top;
        result->width = bitmap.width;
        result->height = bitmap.rows;
        result->coverage.resize(static_cast<size_t>(size));
        if (size != 0)
        {
            const auto stride = std::abs(static_cast<int64_t>(bitmap.pitch));
            const auto minimum_stride = bitmap.pixel_mode == FT_PIXEL_MODE_MONO ? (bitmap.width + 7u) / 8u : bitmap.width;
            if (!bitmap.buffer || stride < minimum_stride ||
                (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.pixel_mode != FT_PIXEL_MODE_MONO) ||
                (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY && bitmap.num_grays < 2))
            {
                throw std::runtime_error("Unsupported glyph bitmap");
            }
            for (uint32_t y = 0; y < bitmap.rows; ++y)
            {
                const auto* row = bitmap.buffer + static_cast<std::ptrdiff_t>(y) * bitmap.pitch;
                for (uint32_t x = 0; x < bitmap.width; ++x)
                {
                    result->coverage[static_cast<size_t>(y) * bitmap.width + x] =
                        bitmap.pixel_mode == FT_PIXEL_MODE_MONO
                            ? static_cast<uint8_t>((row[x / 8] & (0x80u >> (x % 8))) != 0 ? 255 : 0)
                            : static_cast<uint8_t>(static_cast<uint32_t>(row[x]) * 255 / (bitmap.num_grays - 1));
                }
            }
        }
        while (!state.glyphs.empty() &&
               (state.glyphs.size() >= maximum_cache_entries || result->coverage.size() > maximum_cache_bytes - state.cache_bytes))
        {
            const auto oldest = state.glyphs.find(state.recent.front());
            state.cache_bytes -= oldest->second.bitmap->coverage.size();
            state.glyphs.erase(oldest);
            state.recent.pop_front();
        }
        state.recent.push_back(key);
        try
        {
            state.glyphs.emplace(key, implementation::cached_glyph{result, std::prev(state.recent.end())});
        }
        catch (...)
        {
            state.recent.pop_back();
            throw;
        }
        state.cache_bytes += result->coverage.size();
        return result;
    }

    std::u16string gdi_font_manager::family_name(const gdi_font_reference& font)
    {
        (void)this->get().face(font);
        return this->impl_->files.at(font.file)->faces.at(font.face).family;
    }

    void gdi_font_manager::clear()
    {
        this->impl_.reset();
    }

    void gdi_font_manager::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(static_cast<uint32_t>(this->impl_ ? this->impl_->files.size() : 0));
        if (!this->impl_)
        {
            buffer.write(uint32_t{});
            return;
        }
        for (const auto& [id, file] : this->impl_->files)
        {
            buffer.write(id);
            write_text(buffer, file->path.u16string());
            buffer.write_vector(file->bytes);
        }
        buffer.write(static_cast<uint32_t>(this->impl_->selections.size()));
        for (const auto& [key, ref] : this->impl_->selections)
        {
            key.serialize(buffer);
            write_reference(buffer, ref);
        }
    }

    void gdi_font_manager::deserialize(utils::buffer_deserializer& buffer)
    {
        gdi_font_manager restored{};
        const auto count = buffer.read<uint32_t>();
        if (count > maximum_files)
        {
            throw std::length_error("Font resource limit exceeded");
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& state = restored.get();
            const auto id = buffer.read<uint32_t>();
            const windows_path path{read_text(buffer)};
            const auto length = buffer.read<uint64_t>();
            if (!path.is_absolute() || id != i + 1 || length == 0 || length > maximum_file_bytes ||
                length > maximum_total_bytes - state.file_bytes || length > buffer.get_remaining_size())
            {
                throw std::invalid_argument("Invalid saved font resource");
            }
            std::vector<std::byte> bytes(static_cast<size_t>(length));
            buffer.read(bytes.data(), bytes.size());
            auto file = parse_file(state.library.get(), path, std::move(bytes));
            state.file_bytes += file->bytes.size();
            state.files.emplace(id, std::move(file));
        }
        const auto selections = buffer.read<uint32_t>();
        if (selections > maximum_selections || (count == 0 && selections != 0))
        {
            throw std::invalid_argument("Invalid saved font selections");
        }
        for (uint32_t i = 0; i < selections; ++i)
        {
            implementation::selection_key key{};
            key.deserialize(buffer);
            const auto character = key.character;
            if (!windows_path{key.system_root}.is_absolute() || key.family.size() > 256 ||
                std::abs(static_cast<int64_t>(key.height)) > maximum_pixels || std::abs(static_cast<int64_t>(key.width)) > maximum_pixels ||
                character > 0x10FFFF || (character >= 0xD800 && character <= 0xDFFF))
            {
                throw std::invalid_argument("Invalid saved font request");
            }
            const auto ref = read_reference(buffer);
            auto& state = restored.get();
            const auto face = state.face(ref);
            EMU_LOGFONTW request{};
            request.lfHeight = key.height;
            request.lfWidth = key.width;
            request.lfWeight = key.weight;
            request.lfItalic = key.italic ? 1 : 0;
            if (sized_reference(ref.file, ref.face, face, request) != ref || (character != 0 && FT_Get_Char_Index(face, character) == 0))
            {
                throw std::invalid_argument("Inconsistent saved font selection");
            }
            if (!state.selections.emplace(std::move(key), ref).second)
            {
                throw std::invalid_argument("Duplicate saved font selection");
            }
        }
        *this = std::move(restored);
    }
}

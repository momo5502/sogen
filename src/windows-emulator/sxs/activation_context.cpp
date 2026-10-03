#include "../std_include.hpp"
#include "activation_context.hpp"

#include <utils/io.hpp>

#include <charconv>

namespace sogen::sxs
{
    namespace
    {
        constexpr uint32_t activation_context_magic = 0x78746341;
        constexpr uint32_t string_section_magic = 0x64487353;
        constexpr uint32_t hash_string_algorithm_x65599 = 1;
        constexpr uint32_t activation_context_section_assembly_information = 1;
        constexpr uint32_t activation_context_section_dll_redirection = 2;
        constexpr uint32_t activation_context_section_format_string_table = 1;
        constexpr uint32_t string_section_case_insensitive = 1;
        constexpr uint32_t string_section_entries_in_pseudokey_order = 2;
        constexpr uint32_t roster_entry_invalid = 1;
        constexpr uint32_t roster_entry_root = 2;
        constexpr uint32_t assembly_information_root = 1;
        constexpr uint32_t assembly_information_private = 0x10;
        constexpr uint32_t dll_redirection_path_omits_assembly_root = 2;
        constexpr uint32_t activation_context_path_type_win32_file = 2;
        constexpr uint32_t rt_manifest = 24;

        struct activation_context_data
        {
            uint32_t magic;
            uint32_t header_size;
            uint32_t format_version;
            uint32_t total_size;
            uint32_t default_toc_offset;
            uint32_t extended_toc_offset;
            uint32_t assembly_roster_offset;
            uint32_t flags;
        };

        struct activation_context_toc_header
        {
            uint32_t header_size;
            uint32_t entry_count;
            uint32_t first_entry_offset;
            uint32_t flags;
        };

        struct activation_context_toc_entry
        {
            uint32_t id;
            uint32_t offset;
            uint32_t length;
            uint32_t format;
        };

        struct assembly_roster_header
        {
            uint32_t header_size;
            uint32_t hash_algorithm;
            uint32_t entry_count;
            uint32_t first_entry_offset;
            uint32_t assembly_information_section_offset;
        };

        struct assembly_roster_entry
        {
            uint32_t flags;
            uint32_t pseudokey;
            uint32_t assembly_name_offset;
            uint32_t assembly_name_length;
            uint32_t assembly_information_offset;
            uint32_t assembly_information_length;
        };

        struct string_section_header
        {
            uint32_t magic;
            uint32_t header_size;
            uint32_t format_version;
            uint32_t data_format_version;
            uint32_t flags;
            uint32_t element_count;
            uint32_t element_list_offset;
            uint32_t hash_algorithm;
            uint32_t search_structure_offset;
            uint32_t user_data_offset;
            uint32_t user_data_size;
        };

        struct string_section_entry
        {
            uint32_t pseudokey;
            uint32_t key_offset;
            uint32_t key_length;
            uint32_t offset;
            uint32_t length;
            uint32_t assembly_roster_index;
        };

        struct string_section_hash_table
        {
            uint32_t bucket_table_entry_count;
            uint32_t bucket_table_offset;
        };

        struct string_section_hash_bucket
        {
            uint32_t chain_count;
            uint32_t chain_offset;
        };

        struct assembly_global_information
        {
            uint32_t size;
            uint32_t flags;
            std::array<std::byte, 16> policy_coherency_guid;
            std::array<std::byte, 16> policy_override_guid;
            uint32_t application_directory_path_type;
            uint32_t application_directory_length;
            uint32_t application_directory_offset;
            uint32_t resource_name;
        };

        struct assembly_information
        {
            uint32_t size;
            uint32_t flags;
            uint32_t encoded_identity_length;
            uint32_t encoded_identity_offset;
            uint32_t manifest_path_type;
            uint32_t manifest_path_length;
            uint32_t manifest_path_offset;
            uint32_t manifest_last_write_time_low;
            int32_t manifest_last_write_time_high;
            uint32_t policy_path_type;
            uint32_t policy_path_length;
            uint32_t policy_path_offset;
            uint32_t policy_last_write_time_low;
            int32_t policy_last_write_time_high;
            uint32_t metadata_satellite_roster_index;
            uint32_t unused;
            uint32_t manifest_version_major;
            uint32_t manifest_version_minor;
            uint32_t policy_version_major;
            uint32_t policy_version_minor;
            uint32_t assembly_directory_name_length;
            uint32_t assembly_directory_name_offset;
            uint32_t file_count;
            uint32_t language_length;
            uint32_t language_offset;
            uint32_t run_level;
            uint32_t ui_access;
        };

        struct dll_redirection
        {
            uint32_t size;
            uint32_t flags;
            uint32_t total_path_length;
            uint32_t path_segment_count;
            uint32_t path_segment_offset;
        };

        static_assert(sizeof(activation_context_data) == 32);
        static_assert(sizeof(activation_context_toc_header) == 16);
        static_assert(sizeof(activation_context_toc_entry) == 16);
        static_assert(sizeof(assembly_roster_header) == 20);
        static_assert(sizeof(assembly_roster_entry) == 24);
        static_assert(sizeof(string_section_header) == 44);
        static_assert(sizeof(string_section_entry) == 24);
        static_assert(sizeof(assembly_global_information) == 56);
        static_assert(sizeof(assembly_information) == 108);
        static_assert(sizeof(dll_redirection) == 20);

        struct version
        {
            std::array<uint16_t, 4> parts{};

            bool operator==(const version& other) const
            {
                return parts == other.parts;
            }

            bool operator<(const version& other) const
            {
                return std::ranges::lexicographical_compare(parts, other.parts);
            }

            bool operator>(const version& other) const
            {
                return other < *this;
            }

            bool operator<=(const version& other) const
            {
                return !(other < *this);
            }

            bool operator>=(const version& other) const
            {
                return !(*this < other);
            }
        };

        struct assembly_identity
        {
            std::string name{};
            std::string processor_architecture{};
            std::string public_key_token{};
            std::string type{};
            std::string language{};
            version assembly_version{};
            std::string version_text{};
        };

        struct assembly_dependency
        {
            assembly_identity identity{};
            bool optional{};
        };

        struct manifest_document
        {
            std::optional<assembly_identity> identity{};
            std::vector<assembly_dependency> dependencies{};
            std::vector<std::u16string> files{};
        };

        struct xml_element
        {
            std::string name{};
            std::unordered_map<std::string, std::string> attributes{};
            std::vector<size_t> children{};
        };

        struct xml_document
        {
            std::vector<xml_element> elements{};
            std::vector<size_t> roots{};
        };

        struct source_document
        {
            std::string contents{};
            std::u16string path{};
        };

        struct resolved_assembly
        {
            assembly_identity identity{};
            std::u16string manifest_path{};
            std::u16string policy_path{};
            std::u16string directory_name{};
            std::vector<std::u16string> files{};
            std::vector<assembly_dependency> dependencies{};
            uint32_t flags{};
            uint32_t pseudokey{};
        };

        class byte_buffer
        {
          public:
            template <typename T>
            size_t append(const T& value = {})
            {
                const auto offset = this->data_.size();
                this->data_.resize(offset + sizeof(T));
                std::memcpy(this->data_.data() + offset, &value, sizeof(value));
                return offset;
            }

            template <typename T>
            size_t append_array(const size_t count)
            {
                const auto offset = this->data_.size();
                this->data_.resize(offset + sizeof(T) * count);
                return offset;
            }

            template <typename T>
            void write(const size_t offset, const T& value)
            {
                if (offset + sizeof(value) > this->data_.size())
                {
                    throw std::runtime_error("Invalid activation context buffer offset");
                }
                std::memcpy(this->data_.data() + offset, &value, sizeof(value));
            }

            size_t append_string(const std::u16string_view value)
            {
                const auto offset = this->data_.size();
                const auto byte_length = value.size() * sizeof(char16_t);
                this->data_.resize(offset + byte_length + sizeof(char16_t));
                std::memcpy(this->data_.data() + offset, value.data(), byte_length);
                return offset;
            }

            void align(const size_t alignment)
            {
                this->data_.resize((this->data_.size() + alignment - 1) & ~(alignment - 1));
            }

            size_t size() const
            {
                return this->data_.size();
            }

            void append(const std::vector<std::byte>& data)
            {
                this->data_.insert(this->data_.end(), data.begin(), data.end());
            }

            std::vector<std::byte> release()
            {
                return std::move(this->data_);
            }

          private:
            std::vector<std::byte> data_{};
        };

        std::string to_lower(std::string value)
        {
            std::ranges::transform(value, value.begin(),
                                   [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return value;
        }

        bool equal_insensitive(const std::string_view left, const std::string_view right)
        {
            return std::ranges::equal(left, right, [](const unsigned char left_character, const unsigned char right_character) {
                return std::tolower(left_character) == std::tolower(right_character);
            });
        }

        std::string decode_xml_entity(std::string value)
        {
            const std::array<std::pair<std::string_view, std::string_view>, 5> entities{{
                {"&quot;", "\""},
                {"&apos;", "'"},
                {"&lt;", "<"},
                {"&gt;", ">"},
                {"&amp;", "&"},
            }};
            for (const auto& [encoded, decoded] : entities)
            {
                for (size_t offset = 0; (offset = value.find(encoded, offset)) != std::string::npos; offset += decoded.size())
                {
                    value.replace(offset, encoded.size(), decoded);
                }
            }
            return value;
        }

        bool is_xml_name_character(const char character)
        {
            return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '.' || character == '-' ||
                   character == ':';
        }

        void skip_xml_whitespace(const std::string_view text, size_t& offset)
        {
            while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) != 0)
            {
                ++offset;
            }
        }

        std::string local_xml_name(const std::string_view name)
        {
            const auto separator = name.rfind(':');
            return to_lower(std::string(name.substr(separator == std::string_view::npos ? 0 : separator + 1)));
        }

        size_t find_xml_tag_end(const std::string_view xml, size_t offset)
        {
            char quote = 0;
            for (; offset < xml.size(); ++offset)
            {
                const auto character = xml[offset];
                if (quote != 0)
                {
                    if (character == quote)
                    {
                        quote = 0;
                    }
                }
                else if (character == '\'' || character == '"')
                {
                    quote = character;
                }
                else if (character == '>')
                {
                    return offset;
                }
            }
            return std::string_view::npos;
        }

        std::unordered_map<std::string, std::string> parse_attributes(const std::string_view text, size_t offset)
        {
            std::unordered_map<std::string, std::string> attributes{};
            while (offset < text.size())
            {
                skip_xml_whitespace(text, offset);
                if (offset >= text.size() || text[offset] == '/')
                {
                    break;
                }
                const auto name_start = offset;
                while (offset < text.size() && is_xml_name_character(text[offset]))
                {
                    ++offset;
                }
                if (name_start == offset)
                {
                    ++offset;
                    continue;
                }
                const auto name = local_xml_name(text.substr(name_start, offset - name_start));
                skip_xml_whitespace(text, offset);
                if (offset >= text.size() || text[offset] != '=')
                {
                    continue;
                }
                ++offset;
                skip_xml_whitespace(text, offset);
                if (offset >= text.size() || (text[offset] != '\'' && text[offset] != '"'))
                {
                    continue;
                }
                const auto quote = text[offset++];
                const auto value_start = offset;
                while (offset < text.size() && text[offset] != quote)
                {
                    ++offset;
                }
                attributes[name] = decode_xml_entity(std::string(text.substr(value_start, offset - value_start)));
                if (offset < text.size())
                {
                    ++offset;
                }
            }
            return attributes;
        }

        xml_document parse_xml(const std::string_view xml)
        {
            xml_document document{};
            std::vector<size_t> stack{};
            for (size_t offset = 0; (offset = xml.find('<', offset)) != std::string_view::npos;)
            {
                if (xml.substr(offset).starts_with("<!--"))
                {
                    const auto end = xml.find("-->", offset + 4);
                    offset = end == std::string_view::npos ? xml.size() : end + 3;
                    continue;
                }
                if (xml.substr(offset).starts_with("<![CDATA["))
                {
                    const auto end = xml.find("]]>", offset + 9);
                    offset = end == std::string_view::npos ? xml.size() : end + 3;
                    continue;
                }
                const auto end = find_xml_tag_end(xml, offset + 1);
                if (end == std::string_view::npos)
                {
                    break;
                }
                auto position = offset + 1;
                skip_xml_whitespace(xml, position);
                if (position >= end || xml[position] == '?' || xml[position] == '!')
                {
                    offset = end + 1;
                    continue;
                }
                if (xml[position] == '/')
                {
                    ++position;
                    skip_xml_whitespace(xml, position);
                    const auto name_start = position;
                    while (position < end && is_xml_name_character(xml[position]))
                    {
                        ++position;
                    }
                    if (!stack.empty() && name_start != position &&
                        document.elements[stack.back()].name == local_xml_name(xml.substr(name_start, position - name_start)))
                    {
                        stack.pop_back();
                    }
                    offset = end + 1;
                    continue;
                }
                const auto name_start = position;
                while (position < end && is_xml_name_character(xml[position]))
                {
                    ++position;
                }
                if (name_start == position)
                {
                    offset = end + 1;
                    continue;
                }
                xml_element element{};
                element.name = local_xml_name(xml.substr(name_start, position - name_start));
                element.attributes = parse_attributes(xml.substr(0, end), position);
                const auto index = document.elements.size();
                document.elements.push_back(std::move(element));
                if (stack.empty())
                {
                    document.roots.push_back(index);
                }
                else
                {
                    document.elements[stack.back()].children.push_back(index);
                }
                auto closing = end;
                while (closing > position && std::isspace(static_cast<unsigned char>(xml[closing - 1])) != 0)
                {
                    --closing;
                }
                if (closing == position || xml[closing - 1] != '/')
                {
                    stack.push_back(index);
                }
                offset = end + 1;
            }
            return document;
        }

        const xml_element* find_root_xml_element(const xml_document& document, const std::string_view name)
        {
            for (const auto index : document.roots)
            {
                if (document.elements[index].name == name)
                {
                    return &document.elements[index];
                }
            }
            return nullptr;
        }

        std::vector<const xml_element*> find_xml_children(const xml_document& document, const xml_element& parent,
                                                          const std::string_view name)
        {
            std::vector<const xml_element*> result{};
            for (const auto index : parent.children)
            {
                if (document.elements[index].name == name)
                {
                    result.push_back(&document.elements[index]);
                }
            }
            return result;
        }

        std::vector<const xml_element*> find_xml_elements(const xml_document& document, const std::string_view name)
        {
            std::vector<const xml_element*> result{};
            for (const auto& element : document.elements)
            {
                if (element.name == name)
                {
                    result.push_back(&element);
                }
            }
            return result;
        }

        const xml_element* find_xml_descendant(const xml_document& document, const xml_element& parent, const std::string_view name)
        {
            std::vector<size_t> pending(parent.children.begin(), parent.children.end());
            while (!pending.empty())
            {
                const auto index = pending.back();
                pending.pop_back();
                const auto& element = document.elements[index];
                if (element.name == name)
                {
                    return &element;
                }
                pending.insert(pending.end(), element.children.begin(), element.children.end());
            }
            return nullptr;
        }

        std::vector<const xml_element*> find_xml_descendants(const xml_document& document, const xml_element& parent,
                                                             const std::string_view name)
        {
            std::vector<const xml_element*> result{};
            std::vector<size_t> pending(parent.children.begin(), parent.children.end());
            while (!pending.empty())
            {
                const auto index = pending.back();
                pending.pop_back();
                const auto& element = document.elements[index];
                if (element.name == name)
                {
                    result.push_back(&element);
                }
                pending.insert(pending.end(), element.children.begin(), element.children.end());
            }
            return result;
        }

        std::optional<version> parse_version(const std::string_view text)
        {
            version result{};
            size_t start = 0;
            for (size_t index = 0; index < result.parts.size(); ++index)
            {
                const auto end = text.find('.', start);
                const auto part = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
                if (part.empty())
                {
                    return std::nullopt;
                }
                uint32_t value{};
                const auto [parse_end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
                if (error != std::errc{} || parse_end != part.data() + part.size() || value > std::numeric_limits<uint16_t>::max())
                {
                    return std::nullopt;
                }
                result.parts[index] = static_cast<uint16_t>(value);
                if (index + 1 < result.parts.size())
                {
                    if (end == std::string_view::npos)
                    {
                        return std::nullopt;
                    }
                    start = end + 1;
                }
                else if (end != std::string_view::npos)
                {
                    return std::nullopt;
                }
            }
            return result;
        }

        std::optional<assembly_identity> parse_identity(const std::unordered_map<std::string, std::string>& attributes)
        {
            const auto name = attributes.find("name");
            if (name == attributes.end())
            {
                return std::nullopt;
            }

            assembly_identity identity{};
            identity.name = name->second;
            if (const auto entry = attributes.find("processorarchitecture"); entry != attributes.end())
            {
                identity.processor_architecture = entry->second;
            }
            if (const auto entry = attributes.find("publickeytoken"); entry != attributes.end())
            {
                identity.public_key_token = entry->second;
            }
            if (const auto entry = attributes.find("type"); entry != attributes.end())
            {
                identity.type = entry->second;
            }
            if (const auto entry = attributes.find("language"); entry != attributes.end())
            {
                identity.language = entry->second;
            }
            if (const auto entry = attributes.find("version"); entry != attributes.end())
            {
                const auto parsed = parse_version(entry->second);
                if (!parsed)
                {
                    return std::nullopt;
                }
                identity.assembly_version = *parsed;
                identity.version_text = entry->second;
            }
            return identity;
        }

        std::string decode_manifest_text(const std::vector<std::byte>& data)
        {
            const auto is_utf16_little_endian = data.size() >= 2 && data[0] == std::byte{0xff} && data[1] == std::byte{0xfe};
            const auto is_utf16_big_endian = data.size() >= 2 && data[0] == std::byte{0xfe} && data[1] == std::byte{0xff};
            if (is_utf16_little_endian || is_utf16_big_endian)
            {
                const auto length = (data.size() - 2) / sizeof(char16_t);
                std::u16string text(length, u'\0');
                for (size_t index = 0; index < length; ++index)
                {
                    const auto first = std::to_integer<uint8_t>(data[2 + index * 2]);
                    const auto second = std::to_integer<uint8_t>(data[3 + index * 2]);
                    text[index] = static_cast<char16_t>(is_utf16_little_endian ? first | (second << 8) : (first << 8) | second);
                }
                return u16_to_u8(text);
            }

            size_t offset = 0;
            if (data.size() >= 3 && data[0] == std::byte{0xef} && data[1] == std::byte{0xbb} && data[2] == std::byte{0xbf})
            {
                offset = 3;
            }
            return std::string(reinterpret_cast<const char*>(data.data() + offset), data.size() - offset);
        }

        manifest_document parse_manifest(const std::string& xml)
        {
            manifest_document manifest{};
            const auto document = parse_xml(xml);
            const auto* assembly = find_root_xml_element(document, "assembly");
            if (assembly == nullptr)
            {
                return manifest;
            }
            const auto identities = find_xml_children(document, *assembly, "assemblyidentity");
            if (!identities.empty())
            {
                manifest.identity = parse_identity(identities.front()->attributes);
            }

            for (const auto* dependency : find_xml_children(document, *assembly, "dependency"))
            {
                if (const auto* dependency_identity = find_xml_descendant(document, *dependency, "assemblyidentity"))
                {
                    if (auto identity = parse_identity(dependency_identity->attributes))
                    {
                        const auto optional = dependency->attributes.find("optional");
                        manifest.dependencies.push_back(
                            {.identity = std::move(*identity),
                             .optional = optional != dependency->attributes.end() && equal_insensitive(optional->second, "yes")});
                    }
                }
            }

            for (const auto* file : find_xml_children(document, *assembly, "file"))
            {
                if (const auto name = file->attributes.find("name"); name != file->attributes.end())
                {
                    manifest.files.push_back(u8_to_u16(name->second));
                }
            }
            return manifest;
        }

        template <typename T>
        std::optional<T> read_object(const std::vector<std::byte>& data, const size_t offset)
        {
            if (offset > data.size() || sizeof(T) > data.size() - offset)
            {
                return std::nullopt;
            }
            T result{};
            std::memcpy(&result, data.data() + offset, sizeof(result));
            return result;
        }

        std::optional<size_t> rva_to_file_offset(const std::vector<std::byte>& image, const size_t nt_offset, const uint32_t rva)
        {
            const auto section_count = read_object<uint16_t>(image, nt_offset + 6);
            const auto optional_size = read_object<uint16_t>(image, nt_offset + 20);
            const auto header_size = read_object<uint32_t>(image, nt_offset + 24 + 60);
            if (!section_count || !optional_size || !header_size)
            {
                return std::nullopt;
            }
            if (rva < *header_size)
            {
                return rva;
            }

            const auto first_section = nt_offset + 24 + *optional_size;
            for (uint16_t index = 0; index < *section_count; ++index)
            {
                const auto section = first_section + index * 40;
                const auto virtual_size = read_object<uint32_t>(image, section + 8);
                const auto virtual_address = read_object<uint32_t>(image, section + 12);
                const auto raw_size = read_object<uint32_t>(image, section + 16);
                const auto raw_offset = read_object<uint32_t>(image, section + 20);
                if (!virtual_size || !virtual_address || !raw_size || !raw_offset)
                {
                    return std::nullopt;
                }
                const auto mapped_size = std::max(*virtual_size, *raw_size);
                if (rva >= *virtual_address && rva - *virtual_address < mapped_size)
                {
                    return static_cast<size_t>(*raw_offset) + (rva - *virtual_address);
                }
            }
            return std::nullopt;
        }

        std::optional<uint32_t> find_resource_entry(const std::vector<std::byte>& image, const size_t resource_base,
                                                    const uint32_t directory_offset, const std::optional<uint16_t> id)
        {
            const auto named_count = read_object<uint16_t>(image, resource_base + directory_offset + 12);
            const auto id_count = read_object<uint16_t>(image, resource_base + directory_offset + 14);
            if (!named_count || !id_count)
            {
                return std::nullopt;
            }
            const auto total = static_cast<uint32_t>(*named_count) + *id_count;
            for (uint32_t index = 0; index < total; ++index)
            {
                const auto entry_offset = resource_base + directory_offset + 16 + index * 8;
                const auto name = read_object<uint32_t>(image, entry_offset);
                const auto target = read_object<uint32_t>(image, entry_offset + 4);
                if (!name || !target)
                {
                    return std::nullopt;
                }
                if (!id || (!(*name & 0x80000000) && static_cast<uint16_t>(*name) == *id))
                {
                    return *target;
                }
            }
            return std::nullopt;
        }

        std::optional<std::string> read_embedded_manifest(const std::filesystem::path& path, const uint16_t resource_id = 1)
        {
            std::vector<std::byte> image{};
            if (!utils::io::read_file(path, &image) || image.size() < 0x40)
            {
                return std::nullopt;
            }
            const auto nt_offset = read_object<uint32_t>(image, 0x3c);
            if (!nt_offset)
            {
                return std::nullopt;
            }
            const auto signature = read_object<uint32_t>(image, *nt_offset);
            if (!signature || *signature != 0x4550)
            {
                return std::nullopt;
            }
            const auto optional = static_cast<size_t>(*nt_offset) + 24;
            const auto magic = read_object<uint16_t>(image, optional);
            if (!magic || (*magic != 0x10b && *magic != 0x20b))
            {
                return std::nullopt;
            }
            const auto directories = optional + (*magic == 0x10b ? 96 : 112);
            const auto resource_rva = read_object<uint32_t>(image, directories + 2 * 8);
            if (!resource_rva || !*resource_rva)
            {
                return std::nullopt;
            }
            const auto resource_base = rva_to_file_offset(image, *nt_offset, *resource_rva);
            if (!resource_base)
            {
                return std::nullopt;
            }

            const auto type = find_resource_entry(image, *resource_base, 0, static_cast<uint16_t>(rt_manifest));
            if (!type || !(*type & 0x80000000))
            {
                return std::nullopt;
            }
            auto name = find_resource_entry(image, *resource_base, *type & 0x7fffffff, resource_id);
            if (!name)
            {
                name = find_resource_entry(image, *resource_base, *type & 0x7fffffff, std::nullopt);
            }
            if (!name || !(*name & 0x80000000))
            {
                return std::nullopt;
            }
            const auto language = find_resource_entry(image, *resource_base, *name & 0x7fffffff, std::nullopt);
            if (!language || (*language & 0x80000000))
            {
                return std::nullopt;
            }
            const auto data_entry = *resource_base + *language;
            const auto data_rva = read_object<uint32_t>(image, data_entry);
            const auto data_size = read_object<uint32_t>(image, data_entry + 4);
            if (!data_rva || !data_size)
            {
                return std::nullopt;
            }
            const auto data_offset = rva_to_file_offset(image, *nt_offset, *data_rva);
            if (!data_offset || *data_offset > image.size() || *data_size > image.size() - *data_offset)
            {
                return std::nullopt;
            }
            std::vector<std::byte> manifest(image.begin() + *data_offset, image.begin() + *data_offset + *data_size);
            return decode_manifest_text(manifest);
        }

        std::optional<std::string> read_text_file(const std::filesystem::path& path)
        {
            std::vector<std::byte> data{};
            if (!utils::io::read_file(path, &data))
            {
                return std::nullopt;
            }
            return decode_manifest_text(data);
        }

        std::optional<source_document> read_application_manifest(const mapped_module& executable)
        {
            if (auto embedded = read_embedded_manifest(executable.path))
            {
                return source_document{.contents = std::move(*embedded), .path = executable.module_path.u16string()};
            }
            auto external = executable.path;
            external += ".manifest";
            if (auto manifest = read_text_file(external))
            {
                return source_document{.contents = std::move(*manifest), .path = executable.module_path.u16string() + u".manifest"};
            }
            external = executable.path;
            external += ".Manifest";
            if (auto manifest = read_text_file(external))
            {
                return source_document{.contents = std::move(*manifest), .path = executable.module_path.u16string() + u".Manifest"};
            }
            return std::nullopt;
        }

        std::optional<source_document> read_application_config(const mapped_module& executable)
        {
            auto config = executable.path;
            config += ".config";
            if (auto contents = read_text_file(config))
            {
                return source_document{.contents = std::move(*contents), .path = executable.module_path.u16string() + u".config"};
            }
            config = executable.path;
            config += ".Config";
            if (auto contents = read_text_file(config))
            {
                return source_document{.contents = std::move(*contents), .path = executable.module_path.u16string() + u".Config"};
            }
            return std::nullopt;
        }

        bool identity_matches(const assembly_identity& candidate, const assembly_identity& requested, const bool compare_version)
        {
            if (!equal_insensitive(candidate.name, requested.name) ||
                (!requested.public_key_token.empty() && !equal_insensitive(candidate.public_key_token, requested.public_key_token)) ||
                (!requested.processor_architecture.empty() && requested.processor_architecture != "*" &&
                 !equal_insensitive(candidate.processor_architecture, requested.processor_architecture)) ||
                (!requested.type.empty() && !equal_insensitive(candidate.type, requested.type)) ||
                (!requested.language.empty() && requested.language != "*" && !equal_insensitive(candidate.language, requested.language)))
            {
                return false;
            }
            return !compare_version || requested.version_text.empty() || candidate.assembly_version == requested.assembly_version;
        }

        bool is_unified_system_assembly(const assembly_identity& identity)
        {
            return to_lower(identity.name).starts_with("microsoft.windows.") &&
                   equal_insensitive(identity.public_key_token, "6595b64144ccf1df");
        }

        bool has_compatible_system_version(const assembly_identity& candidate, const assembly_identity& requested)
        {
            return is_unified_system_assembly(requested) && candidate.assembly_version.parts[0] == requested.assembly_version.parts[0] &&
                   candidate.assembly_version.parts[1] == requested.assembly_version.parts[1];
        }

        std::string winsxs_name(std::string name)
        {
            name = to_lower(std::move(name));
            if (name.size() > 40)
            {
                name = name.substr(0, 19) + ".." + name.substr(name.size() - 19);
            }
            return name;
        }

        bool is_better_system_candidate(const assembly_identity& candidate, const assembly_identity& current,
                                        const std::string_view preferred_language)
        {
            const auto candidate_uses_preferred_language = equal_insensitive(candidate.language, preferred_language);
            const auto current_uses_preferred_language = equal_insensitive(current.language, preferred_language);
            if (candidate_uses_preferred_language != current_uses_preferred_language)
            {
                return candidate_uses_preferred_language;
            }
            return candidate.assembly_version > current.assembly_version;
        }

        bool version_in_range(const version& value, const std::string_view range)
        {
            const auto separator = range.find('-');
            const auto minimum = parse_version(range.substr(0, separator));
            const auto maximum = parse_version(separator == std::string_view::npos ? range : range.substr(separator + 1));
            return minimum && maximum && value >= *minimum && value <= *maximum;
        }

        bool apply_binding_redirects(const std::string& xml, assembly_identity& identity)
        {
            const auto document = parse_xml(xml);
            for (const auto* dependent_assembly : find_xml_elements(document, "dependentassembly"))
            {
                const auto* identity_element = find_xml_descendant(document, *dependent_assembly, "assemblyidentity");
                if (identity_element == nullptr)
                {
                    continue;
                }
                const auto policy_identity = parse_identity(identity_element->attributes);
                if (!policy_identity || !identity_matches(*policy_identity, identity, false))
                {
                    continue;
                }
                for (const auto* redirect : find_xml_descendants(document, *dependent_assembly, "bindingredirect"))
                {
                    const auto old_version = redirect->attributes.find("oldversion");
                    const auto new_version = redirect->attributes.find("newversion");
                    if (old_version == redirect->attributes.end() || new_version == redirect->attributes.end() ||
                        !version_in_range(identity.assembly_version, old_version->second))
                    {
                        continue;
                    }
                    const auto parsed = parse_version(new_version->second);
                    if (parsed)
                    {
                        identity.assembly_version = *parsed;
                        identity.version_text = new_version->second;
                        return true;
                    }
                }
            }
            return false;
        }

        bool allows_publisher_policy(const std::string& xml, const assembly_identity& identity)
        {
            const auto document = parse_xml(xml);
            for (const auto* dependent_assembly : find_xml_elements(document, "dependentassembly"))
            {
                const auto* identity_element = find_xml_descendant(document, *dependent_assembly, "assemblyidentity");
                if (identity_element == nullptr)
                {
                    continue;
                }
                const auto policy_identity = parse_identity(identity_element->attributes);
                if (!policy_identity || !identity_matches(*policy_identity, identity, false))
                {
                    continue;
                }
                for (const auto* policy : find_xml_descendants(document, *dependent_assembly, "publisherpolicy"))
                {
                    const auto apply = policy->attributes.find("apply");
                    if (apply != policy->attributes.end() && equal_insensitive(apply->second, "no"))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        std::string architecture_name(const assembly_identity& identity, const mapped_module& executable)
        {
            if (!identity.processor_architecture.empty() && identity.processor_architecture != "*")
            {
                return to_lower(identity.processor_architecture);
            }
            return executable.machine == IMAGE_FILE_MACHINE_I386 ? "x86" : "amd64";
        }

        std::vector<std::filesystem::path> directory_entries(const std::filesystem::path& directory)
        {
            std::vector<std::filesystem::path> entries{};
            std::error_code error{};
            std::filesystem::directory_iterator iterator(directory, error);
            const std::filesystem::directory_iterator end{};
            while (!error && iterator != end)
            {
                entries.push_back(iterator->path());
                iterator.increment(error);
            }
            std::ranges::sort(entries);
            return entries;
        }

        void apply_system_policy(const file_system& file_system, const windows_path& manifests_path, assembly_identity& identity,
                                 const mapped_module& executable)
        {
            const auto directory = file_system.translate(manifests_path);
            const auto name_fragment = winsxs_name(identity.name);
            const auto token_fragment = to_lower(identity.public_key_token);
            const auto architecture = architecture_name(identity, executable);
            const auto version_prefix =
                std::to_string(identity.assembly_version.parts[0]) + "." + std::to_string(identity.assembly_version.parts[1]) + ".";
            const auto prefix = architecture + "_policy." + version_prefix + name_fragment + "_";
            for (const auto& path : directory_entries(directory))
            {
                const auto filename = to_lower(path.filename().string());
                if (!filename.starts_with(prefix) || (!token_fragment.empty() && filename.find(token_fragment) == std::string::npos) ||
                    !filename.ends_with(".manifest"))
                {
                    continue;
                }
                if (const auto policy = read_text_file(path))
                {
                    apply_binding_redirects(*policy, identity);
                }
            }
        }

        std::optional<resolved_assembly> resolve_private_assembly(const mapped_module& executable, const assembly_identity& identity)
        {
            const auto application_directory = executable.path.parent_path();
            const auto identity_name = std::filesystem::path(u8_to_u16(identity.name));
            const std::array candidates{
                application_directory / (identity_name.u16string() + u".manifest"),
                application_directory / identity_name / (identity_name.u16string() + u".manifest"),
            };
            for (const auto& candidate : candidates)
            {
                const auto xml = read_text_file(candidate);
                if (!xml)
                {
                    continue;
                }
                auto manifest = parse_manifest(*xml);
                if (!manifest.identity || !identity_matches(*manifest.identity, identity, true))
                {
                    continue;
                }
                const auto relative_directory = candidate.parent_path().lexically_relative(application_directory);
                const auto relative_manifest = candidate.lexically_relative(application_directory);
                resolved_assembly result{};
                result.identity = std::move(*manifest.identity);
                result.manifest_path = (executable.module_path.parent() / windows_path(relative_manifest)).u16string();
                result.directory_name = relative_directory == "." ? std::u16string{} : relative_directory.u16string();
                result.files = std::move(manifest.files);
                result.dependencies = std::move(manifest.dependencies);
                result.flags = assembly_information_private;
                return result;
            }

            const std::array embedded_candidates{
                application_directory / (identity_name.u16string() + u".dll"),
                application_directory / identity_name / (identity_name.u16string() + u".dll"),
            };
            for (const auto& candidate : embedded_candidates)
            {
                const auto xml = read_embedded_manifest(candidate, 2);
                if (!xml)
                {
                    continue;
                }
                auto manifest = parse_manifest(*xml);
                if (!manifest.identity || !identity_matches(*manifest.identity, identity, true))
                {
                    continue;
                }
                const auto relative_directory = candidate.parent_path().lexically_relative(application_directory);
                const auto relative_manifest = candidate.lexically_relative(application_directory);
                resolved_assembly result{};
                result.identity = std::move(*manifest.identity);
                result.manifest_path = (executable.module_path.parent() / windows_path(relative_manifest)).u16string();
                result.directory_name = relative_directory == "." ? std::u16string{} : relative_directory.u16string();
                result.files = std::move(manifest.files);
                result.dependencies = std::move(manifest.dependencies);
                result.flags = assembly_information_private;
                return result;
            }
            return std::nullopt;
        }

        std::optional<resolved_assembly> resolve_winsxs_assembly(const file_system& file_system, const windows_path& manifests_path,
                                                                 const windows_path& winsxs_path, const assembly_identity& identity,
                                                                 const mapped_module& executable, const std::string_view preferred_language)
        {
            const auto directory = file_system.translate(manifests_path);
            const auto architecture = architecture_name(identity, executable);
            const auto prefix = architecture + "_" + winsxs_name(identity.name) + "_";
            const auto token_fragment = "_" + to_lower(identity.public_key_token) + "_";
            std::optional<resolved_assembly> exact{};
            std::optional<resolved_assembly> compatible{};
            for (const auto& path : directory_entries(directory))
            {
                const auto filename = to_lower(path.filename().string());
                if (!filename.starts_with(prefix) || !filename.ends_with(".manifest") ||
                    (!identity.public_key_token.empty() && filename.find(token_fragment) == std::string::npos))
                {
                    continue;
                }
                const auto xml = read_text_file(path);
                if (!xml)
                {
                    continue;
                }
                auto manifest = parse_manifest(*xml);
                if (!manifest.identity || !identity_matches(*manifest.identity, identity, false))
                {
                    continue;
                }
                const auto stem = path.stem().u16string();
                std::error_code error{};
                if (!std::filesystem::is_directory(file_system.translate(winsxs_path / windows_path(stem)), error))
                {
                    error.clear();
                    continue;
                }
                resolved_assembly result{};
                result.identity = std::move(*manifest.identity);
                result.manifest_path = (manifests_path / windows_path(path.filename())).u16string();
                result.directory_name = stem;
                result.files = std::move(manifest.files);
                result.dependencies = std::move(manifest.dependencies);
                if (identity_matches(result.identity, identity, true))
                {
                    if (identity.language != "*" || equal_insensitive(result.identity.language, preferred_language))
                    {
                        return result;
                    }
                    if (!exact || is_better_system_candidate(result.identity, exact->identity, preferred_language))
                    {
                        exact = std::move(result);
                    }
                    continue;
                }
                if (has_compatible_system_version(result.identity, identity) &&
                    (!compatible || is_better_system_candidate(result.identity, compatible->identity, preferred_language)))
                {
                    compatible = std::move(result);
                }
            }
            if (exact)
            {
                return exact;
            }
            return compatible;
        }

        uint32_t hash_string(const std::u16string_view value)
        {
            uint32_t hash = 0;
            for (auto character : value)
            {
                if (character >= u'a' && character <= u'z')
                {
                    character -= u'a' - u'A';
                }
                hash = hash * 65599 + character;
            }
            return hash;
        }

        std::u16string encode_identity(const assembly_identity& identity)
        {
            std::string encoded = identity.name;
            if (!identity.processor_architecture.empty())
            {
                encoded += ",processorArchitecture=\"" + identity.processor_architecture + "\"";
            }
            if (!identity.public_key_token.empty())
            {
                encoded += ",publicKeyToken=\"" + identity.public_key_token + "\"";
            }
            if (!identity.type.empty())
            {
                encoded += ",type=\"" + identity.type + "\"";
            }
            if (!identity.version_text.empty())
            {
                encoded += ",version=\"" + identity.version_text + "\"";
            }
            if (!identity.language.empty() && identity.language != "*")
            {
                encoded += ",language=\"" + identity.language + "\"";
            }
            return u8_to_u16(encoded);
        }

        uint32_t checked_u32(const size_t value)
        {
            if (value > std::numeric_limits<uint32_t>::max())
            {
                throw std::runtime_error("Activation context is too large");
            }
            return static_cast<uint32_t>(value);
        }

        uint32_t byte_length(const std::u16string_view value)
        {
            return checked_u32(value.size() * sizeof(char16_t));
        }

        struct assembly_section_result
        {
            std::vector<std::byte> data{};
            std::vector<string_section_entry> entries{};
        };

        assembly_section_result build_assembly_section(const std::vector<resolved_assembly>& assemblies,
                                                       const std::u16string_view application_directory)
        {
            byte_buffer section{};
            const auto header_offset = section.append<string_section_header>();

            assembly_global_information global{};
            global.application_directory_path_type = activation_context_path_type_win32_file;
            global.application_directory_length = byte_length(application_directory);
            global.application_directory_offset = sizeof(global);
            global.resource_name = 1;
            const auto global_offset = section.append(global);
            section.append_string(application_directory);
            global.size = checked_u32(section.size() - global_offset);
            section.write(global_offset, global);
            section.align(4);

            const auto entries_offset = section.append_array<string_section_entry>(assemblies.size());
            std::vector<string_section_entry> entries(assemblies.size());
            for (size_t index = 0; index < assemblies.size(); ++index)
            {
                const auto& assembly = assemblies[index];
                auto& entry = entries[index];
                entry.pseudokey = assembly.pseudokey;
                entry.assembly_roster_index = checked_u32(index + 1);
                if (index != 0)
                {
                    const auto name = u8_to_u16(assembly.identity.name);
                    entry.key_offset = checked_u32(section.append_string(name));
                    entry.key_length = byte_length(name);
                    section.align(4);
                }

                entry.offset = checked_u32(section.size());
                assembly_information information{};
                information.size = sizeof(information);
                information.flags = assembly.flags;
                information.manifest_version_major = 1;
                information.file_count = checked_u32(assembly.files.size());
                const auto information_offset = section.append(information);

                const auto identity = index == 0 ? std::u16string{} : encode_identity(assembly.identity);
                if (!identity.empty())
                {
                    information.encoded_identity_length = byte_length(identity);
                    information.encoded_identity_offset = checked_u32(section.append_string(identity));
                    section.align(4);
                }
                if (!assembly.manifest_path.empty())
                {
                    information.manifest_path_type = activation_context_path_type_win32_file;
                    information.manifest_path_length = byte_length(assembly.manifest_path);
                    information.manifest_path_offset = checked_u32(section.append_string(assembly.manifest_path));
                    section.align(4);
                }
                if (!assembly.policy_path.empty())
                {
                    information.policy_path_type = activation_context_path_type_win32_file;
                    information.policy_path_length = byte_length(assembly.policy_path);
                    information.policy_path_offset = checked_u32(section.append_string(assembly.policy_path));
                    section.align(4);
                }
                if (!assembly.directory_name.empty())
                {
                    information.assembly_directory_name_length = byte_length(assembly.directory_name);
                    information.assembly_directory_name_offset = checked_u32(section.append_string(assembly.directory_name));
                    section.align(4);
                }
                if (!assembly.identity.language.empty() && assembly.identity.language != "*")
                {
                    const auto language = u8_to_u16(assembly.identity.language);
                    information.language_length = byte_length(language);
                    information.language_offset = checked_u32(section.append_string(language));
                    section.align(4);
                }
                section.write(information_offset, information);
                entry.length = checked_u32(section.size() - entry.offset);
                section.write(entries_offset + index * sizeof(entry), entry);
            }

            uint32_t hash_table_offset{};
            if (entries.size() > 1)
            {
                hash_table_offset = checked_u32(section.append<string_section_hash_table>());
                const auto bucket_count = entries.size() - 1;
                const auto buckets_offset = section.append_array<string_section_hash_bucket>(bucket_count);
                std::vector<std::vector<uint32_t>> chains(bucket_count);
                for (size_t index = 1; index < entries.size(); ++index)
                {
                    chains[entries[index].pseudokey % bucket_count].push_back(
                        checked_u32(entries_offset + index * sizeof(string_section_entry)));
                }
                for (size_t index = 0; index < chains.size(); ++index)
                {
                    string_section_hash_bucket bucket{};
                    bucket.chain_count = checked_u32(chains[index].size());
                    if (!chains[index].empty())
                    {
                        bucket.chain_offset = checked_u32(section.size());
                        for (const auto entry_offset : chains[index])
                        {
                            section.append(entry_offset);
                        }
                    }
                    section.write(buckets_offset + index * sizeof(bucket), bucket);
                }
                section.write(hash_table_offset, string_section_hash_table{.bucket_table_entry_count = checked_u32(bucket_count),
                                                                           .bucket_table_offset = checked_u32(buckets_offset)});
            }

            string_section_header header{};
            header.magic = string_section_magic;
            header.header_size = sizeof(header);
            header.format_version = 1;
            header.data_format_version = 1;
            header.flags = string_section_case_insensitive | string_section_entries_in_pseudokey_order;
            header.element_count = checked_u32(entries.size());
            header.element_list_offset = checked_u32(entries_offset);
            header.hash_algorithm = hash_string_algorithm_x65599;
            header.search_structure_offset = hash_table_offset;
            header.user_data_offset = checked_u32(global_offset);
            header.user_data_size = global.size;
            section.write(header_offset, header);
            return {.data = section.release(), .entries = std::move(entries)};
        }

        struct dll_entry
        {
            std::u16string name{};
            uint32_t roster_index{};
            uint32_t hash{};
            uint32_t entry_offset{};
        };

        std::vector<std::byte> build_dll_section(const std::vector<resolved_assembly>& assemblies)
        {
            std::vector<dll_entry> dlls{};
            for (size_t index = 0; index < assemblies.size(); ++index)
            {
                for (const auto& file : assemblies[index].files)
                {
                    dll_entry dll{};
                    dll.name = file;
                    dll.roster_index = checked_u32(index + 1);
                    dll.hash = hash_string(file);
                    dlls.push_back(std::move(dll));
                }
            }
            if (dlls.empty())
            {
                return {};
            }

            byte_buffer section{};
            const auto header_offset = section.append<string_section_header>();
            const auto entries_offset = section.append_array<string_section_entry>(dlls.size());
            for (size_t index = 0; index < dlls.size(); ++index)
            {
                auto& dll = dlls[index];
                string_section_entry entry{};
                entry.pseudokey = dll.hash;
                entry.key_offset = checked_u32(section.append_string(dll.name));
                entry.key_length = byte_length(dll.name);
                entry.assembly_roster_index = dll.roster_index;
                section.align(4);
                dll_redirection redirection{};
                redirection.size = sizeof(redirection);
                redirection.flags = dll_redirection_path_omits_assembly_root;
                entry.offset = checked_u32(section.append(redirection));
                entry.length = sizeof(dll_redirection);
                dll.entry_offset = checked_u32(entries_offset + index * sizeof(entry));
                section.write(dll.entry_offset, entry);
            }

            const auto hash_table_offset = section.append<string_section_hash_table>();
            const auto buckets_offset = section.append_array<string_section_hash_bucket>(dlls.size());
            std::vector<std::vector<uint32_t>> chains(dlls.size());
            for (const auto& dll : dlls)
            {
                chains[dll.hash % dlls.size()].push_back(dll.entry_offset);
            }
            for (size_t index = 0; index < chains.size(); ++index)
            {
                string_section_hash_bucket bucket{};
                bucket.chain_count = checked_u32(chains[index].size());
                if (!chains[index].empty())
                {
                    bucket.chain_offset = checked_u32(section.size());
                    for (const auto entry_offset : chains[index])
                    {
                        section.append(entry_offset);
                    }
                }
                section.write(buckets_offset + index * sizeof(bucket), bucket);
            }
            section.write(hash_table_offset, string_section_hash_table{.bucket_table_entry_count = checked_u32(dlls.size()),
                                                                       .bucket_table_offset = checked_u32(buckets_offset)});

            string_section_header header{};
            header.magic = string_section_magic;
            header.header_size = sizeof(header);
            header.format_version = 1;
            header.data_format_version = 1;
            header.flags = string_section_case_insensitive;
            header.element_count = checked_u32(dlls.size());
            header.element_list_offset = checked_u32(entries_offset);
            header.hash_algorithm = hash_string_algorithm_x65599;
            header.search_structure_offset = checked_u32(hash_table_offset);
            section.write(header_offset, header);
            return section.release();
        }

        std::vector<std::byte> build_blob(const std::vector<resolved_assembly>& assemblies, const std::u16string_view application_directory)
        {
            auto assembly_section = build_assembly_section(assemblies, application_directory);
            auto dll_section = build_dll_section(assemblies);

            byte_buffer result{};
            const auto header_offset = result.append<activation_context_data>();
            const auto roster_header_offset = result.append<assembly_roster_header>();
            const auto roster_entries_offset = result.append_array<assembly_roster_entry>(assemblies.size() + 1);
            const auto toc_header_offset = result.append<activation_context_toc_header>();
            const auto toc_entry_count = dll_section.empty() ? 1u : 2u;
            const auto toc_entries_offset = result.append_array<activation_context_toc_entry>(toc_entry_count);
            result.align(4);

            const auto assembly_section_offset = result.size();
            result.append(assembly_section.data);
            const auto dll_section_offset = result.size();
            result.append(dll_section);

            assembly_roster_entry invalid_roster_entry{};
            invalid_roster_entry.flags = roster_entry_invalid;
            result.write(roster_entries_offset, invalid_roster_entry);
            for (size_t index = 0; index < assemblies.size(); ++index)
            {
                const auto& source = assembly_section.entries[index];
                assembly_roster_entry roster{};
                roster.flags = index == 0 ? roster_entry_root : 0;
                roster.pseudokey = source.pseudokey;
                roster.assembly_name_offset = source.key_offset ? checked_u32(assembly_section_offset + source.key_offset) : 0;
                roster.assembly_name_length = source.key_length;
                roster.assembly_information_offset = checked_u32(assembly_section_offset + source.offset);
                roster.assembly_information_length = source.length;
                result.write(roster_entries_offset + (index + 1) * sizeof(roster), roster);
            }

            result.write(roster_header_offset,
                         assembly_roster_header{.header_size = sizeof(assembly_roster_header),
                                                .hash_algorithm = hash_string_algorithm_x65599,
                                                .entry_count = checked_u32(assemblies.size() + 1),
                                                .first_entry_offset = checked_u32(roster_entries_offset),
                                                .assembly_information_section_offset = checked_u32(assembly_section_offset)});
            result.write(toc_header_offset, activation_context_toc_header{.header_size = sizeof(activation_context_toc_header),
                                                                          .entry_count = toc_entry_count,
                                                                          .first_entry_offset = checked_u32(toc_entries_offset),
                                                                          .flags = 2});
            result.write(toc_entries_offset, activation_context_toc_entry{.id = activation_context_section_assembly_information,
                                                                          .offset = checked_u32(assembly_section_offset),
                                                                          .length = checked_u32(assembly_section.data.size()),
                                                                          .format = activation_context_section_format_string_table});
            if (!dll_section.empty())
            {
                result.write(toc_entries_offset + sizeof(activation_context_toc_entry),
                             activation_context_toc_entry{.id = activation_context_section_dll_redirection,
                                                          .offset = checked_u32(dll_section_offset),
                                                          .length = checked_u32(dll_section.size()),
                                                          .format = activation_context_section_format_string_table});
            }
            activation_context_data header{};
            header.magic = activation_context_magic;
            header.header_size = sizeof(header);
            header.format_version = 1;
            header.total_size = checked_u32(result.size());
            header.default_toc_offset = checked_u32(toc_header_offset);
            header.assembly_roster_offset = checked_u32(roster_header_offset);
            result.write(header_offset, header);
            return result.release();
        }
    }

    std::vector<std::byte> build_process_activation_context(const file_system& file_system, const mapped_module& executable,
                                                            const windows_path& system_root, const std::string_view preferred_language)
    {
        const auto manifest_source = read_application_manifest(executable);
        if (!manifest_source)
        {
            return {};
        }
        auto manifest = parse_manifest(manifest_source->contents);
        const auto config = read_application_config(executable);
        const auto winsxs_path = system_root / windows_path(u"WinSxS");
        const auto manifests_path = winsxs_path / windows_path(u"Manifests");

        resolved_assembly root{};
        if (manifest.identity)
        {
            root.identity = *manifest.identity;
        }
        root.manifest_path = manifest_source->path;
        if (config)
        {
            root.policy_path = config->path;
        }
        root.files = std::move(manifest.files);
        root.flags = assembly_information_root | assembly_information_private;

        std::vector<resolved_assembly> assemblies{};
        assemblies.push_back(std::move(root));
        std::deque<assembly_dependency> pending_dependencies(manifest.dependencies.begin(), manifest.dependencies.end());
        while (!pending_dependencies.empty())
        {
            auto dependency = std::move(pending_dependencies.front());
            pending_dependencies.pop_front();
            auto& identity = dependency.identity;
            if (identity.processor_architecture.empty() || identity.processor_architecture == "*")
            {
                identity.processor_architecture = architecture_name(identity, executable);
            }

            if (!config || allows_publisher_policy(config->contents, identity))
            {
                apply_system_policy(file_system, manifests_path, identity, executable);
            }
            if (config)
            {
                apply_binding_redirects(config->contents, identity);
            }

            auto resolved = resolve_private_assembly(executable, identity);
            if (!resolved)
            {
                resolved = resolve_winsxs_assembly(file_system, manifests_path, winsxs_path, identity, executable, preferred_language);
            }
            if (!resolved)
            {
                if (dependency.optional)
                {
                    continue;
                }
                throw std::runtime_error("Unable to resolve side-by-side assembly: " + identity.name +
                                         ", version=" + identity.version_text);
            }
            if (std::ranges::any_of(assemblies.begin() + 1, assemblies.end(), [&](const resolved_assembly& assembly) {
                    return identity_matches(assembly.identity, resolved->identity, true) ||
                           (!assembly.manifest_path.empty() && assembly.manifest_path == resolved->manifest_path);
                }))
            {
                continue;
            }
            for (auto& nested_dependency : resolved->dependencies)
            {
                pending_dependencies.push_back(std::move(nested_dependency));
            }
            resolved->pseudokey = hash_string(u8_to_u16(resolved->identity.name));
            assemblies.push_back(std::move(*resolved));
        }

        if (assemblies.size() > 2)
        {
            std::ranges::stable_sort(assemblies.begin() + 1, assemblies.end(), {}, &resolved_assembly::pseudokey);
        }

        auto application_directory = executable.module_path.parent().u16string();
        if (!application_directory.empty() && application_directory.back() != u'\\')
        {
            application_directory.push_back(u'\\');
        }
        return build_blob(assemblies, application_directory);
    }
}

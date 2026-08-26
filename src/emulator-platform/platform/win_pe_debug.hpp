#pragma once

#include "win_pefile.hpp"

#include <utils/buffer_accessor.hpp>
#include <utils/io.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace sogen
{
    struct pdb_signature
    {
        std::string guid{};
        uint32_t age{};
        std::string path{};

        bool valid() const
        {
            return !this->guid.empty() && this->age != 0;
        }

        std::string_view filename() const
        {
            const auto separator = this->path.find_last_of("/\\");
            return std::string_view{this->path}.substr(separator == std::string::npos ? 0 : separator + 1);
        }
    };

    namespace winpe
    {
        enum class image_layout
        {
            file,
            mapped,
        };

        namespace detail
        {
            constexpr uint32_t image_debug_type_codeview = 2;

            struct image_debug_directory
            {
                uint32_t characteristics{};
                uint32_t time_date_stamp{};
                uint16_t major_version{};
                uint16_t minor_version{};
                uint32_t type{};
                uint32_t size_of_data{};
                uint32_t address_of_raw_data{};
                uint32_t pointer_to_raw_data{};
            };

            static_assert(sizeof(image_debug_directory) == 28);

            inline std::string format_guid(const std::span<const std::byte, 16> guid)
            {
                uint32_t data1{};
                uint16_t data2{};
                uint16_t data3{};
                memcpy(&data1, guid.data(), sizeof(data1));
                memcpy(&data2, guid.data() + 4, sizeof(data2));
                memcpy(&data3, guid.data() + 6, sizeof(data3));

                std::ostringstream stream{};
                stream << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << data1 << std::setw(4) << data2 << std::setw(4)
                       << data3;

                for (size_t i = 8; i < guid.size(); ++i)
                {
                    stream << std::setw(2) << std::to_integer<unsigned>(guid[i]);
                }

                return stream.str();
            }

            inline std::optional<pdb_signature> parse_rsds_record(const utils::safe_buffer_accessor<const std::byte>& buffer,
                                                                  const size_t offset, const size_t size)
            {
                if (size < 24)
                {
                    return std::nullopt;
                }

                const auto* data = buffer.get_pointer_for_range(offset, size);
                if (memcmp(data, "RSDS", 4) != 0)
                {
                    return std::nullopt;
                }

                std::span<const std::byte, 16> guid{data + 4, 16};
                uint32_t age{};
                memcpy(&age, data + 20, sizeof(age));

                std::string path{};
                for (size_t i = 24; i < size && data[i] != std::byte{}; ++i)
                {
                    path.push_back(static_cast<char>(data[i]));
                }

                return pdb_signature{
                    .guid = format_guid(guid),
                    .age = age,
                    .path = std::move(path),
                };
            }

            template <typename T>
            std::optional<size_t> rva_to_file_offset(const utils::safe_buffer_accessor<const std::byte>& buffer,
                                                     const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset,
                                                     const uint32_t rva)
            {
                if (rva < nt_headers.OptionalHeader.SizeOfHeaders)
                {
                    return rva;
                }

                const auto section_offset = get_first_section_offset(nt_headers, nt_headers_offset);
                const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(section_offset));

                for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
                {
                    const auto section = sections.get(i);
                    const auto section_size = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
                    if (rva >= section.VirtualAddress && rva < section.VirtualAddress + section_size)
                    {
                        return section.PointerToRawData + (rva - section.VirtualAddress);
                    }
                }

                return std::nullopt;
            }
        }

        template <typename T>
        std::optional<pdb_signature> read_pdb_signature(const utils::safe_buffer_accessor<const std::byte>& buffer,
                                                        const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset,
                                                        const image_layout layout)
        {
            const auto& debug_entry = nt_headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            if (debug_entry.VirtualAddress == 0 || debug_entry.Size < sizeof(detail::image_debug_directory))
            {
                return std::nullopt;
            }

            const auto debug_offset = layout == image_layout::mapped
                                          ? std::optional<size_t>{debug_entry.VirtualAddress}
                                          : detail::rva_to_file_offset(buffer, nt_headers, nt_headers_offset, debug_entry.VirtualAddress);
            if (!debug_offset)
            {
                return std::nullopt;
            }

            const auto debug_directories = buffer.as<detail::image_debug_directory>(*debug_offset);
            const auto count = debug_entry.Size / sizeof(detail::image_debug_directory);
            for (size_t i = 0; i < count; ++i)
            {
                const auto debug = debug_directories.get(i);
                if (debug.type != detail::image_debug_type_codeview || debug.size_of_data == 0)
                {
                    continue;
                }

                const auto codeview_offset = layout == image_layout::mapped ? debug.address_of_raw_data : debug.pointer_to_raw_data;
                if (auto signature = detail::parse_rsds_record(buffer, codeview_offset, debug.size_of_data))
                {
                    return signature;
                }
            }

            return std::nullopt;
        }

        inline std::optional<pdb_signature> read_pdb_signature(const std::filesystem::path& file)
        {
            const auto data = utils::io::read_file(file);
            if (data.empty())
            {
                return std::nullopt;
            }

            utils::safe_buffer_accessor<const std::byte> buffer{data};
            try
            {
                const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
                const auto nt_headers_offset = dos_header.e_lfanew;
                const auto nt_signature = buffer.as<uint32_t>(nt_headers_offset).get();
                if (dos_header.e_magic != PEDosHeader_t::k_Magic || nt_signature != PENTHeaders_t<uint32_t>::k_Signature)
                {
                    return std::nullopt;
                }

                const auto magic_offset = nt_headers_offset + sizeof(uint32_t) + sizeof(PEFileHeader_t);
                const auto magic = buffer.as<uint16_t>(magic_offset).get();
                if (magic == PEOptionalHeader_t<uint32_t>::k_Magic)
                {
                    const auto nt_headers = buffer.as<PENTHeaders_t<uint32_t>>(nt_headers_offset).get();
                    return read_pdb_signature(buffer, nt_headers, nt_headers_offset, image_layout::file);
                }
                if (magic == PEOptionalHeader_t<uint64_t>::k_Magic)
                {
                    const auto nt_headers = buffer.as<PENTHeaders_t<uint64_t>>(nt_headers_offset).get();
                    return read_pdb_signature(buffer, nt_headers, nt_headers_offset, image_layout::file);
                }
            }
            catch (...)
            {
            }

            return std::nullopt;
        }
    }
} // namespace sogen

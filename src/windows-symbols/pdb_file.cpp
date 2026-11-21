#include "pdb_file.hpp"

#include <utils/buffer_accessor.hpp>
#include <utils/io.hpp>
#include <platform/platform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
#include <utility>

#include <PDB.h>
#include <PDB_RawFile.h>
#include <PDB_Types.h>
#include <PDB_DBITypes.h>

namespace sogen::symbols
{
    namespace
    {
        using byte_view = std::span<const std::byte>;
        using symbol_map = std::map<std::pair<uint32_t, uint32_t>, std::string>;
        using record = PDB::CodeView::DBI::Record;
        using record_kind = PDB::CodeView::DBI::SymbolRecordKind;

        template <typename T>
        T read_object(const byte_view bytes, const size_t offset = 0)
        {
            return utils::safe_buffer_accessor{bytes}.template as<T>(offset).get();
        }

        byte_view checked_slice(const byte_view bytes, const size_t offset, const size_t size)
        {
            utils::safe_buffer_accessor{bytes}.validate(offset, size);
            return bytes.subspan(offset, size);
        }

        size_t block_count(const size_t size, const uint32_t block_size)
        {
            return size / block_size + (size % block_size != 0);
        }

        std::vector<std::byte> read_blocks(const byte_view bytes, const PDB::SuperBlock& header, const byte_view indices, const size_t size)
        {
            if (size > bytes.size())
            {
                throw std::runtime_error("PDB stream exceeds file size");
            }

            std::vector<std::byte> result(size);
            for (size_t i = 0, offset = 0; offset < size; ++i)
            {
                const auto index = read_object<uint32_t>(indices, i * sizeof(uint32_t));
                if (index >= header.blockCount)
                {
                    throw std::runtime_error("Invalid PDB block index");
                }

                const auto count = std::min<size_t>(header.blockSize, size - offset);
                const auto source = checked_slice(bytes, static_cast<size_t>(index) * header.blockSize, count);
                std::memcpy(result.data() + offset, source.data(), count);
                offset += count;
            }

            return result;
        }

        void validate_msf(const byte_view bytes)
        {
            const auto& header = *reinterpret_cast<const PDB::SuperBlock*>(checked_slice(bytes, 0, sizeof(PDB::SuperBlock)).data());
            if (!std::has_single_bit(header.blockSize) || header.blockSize < 512 || header.blockSize > 65536 ||
                header.directorySize > std::numeric_limits<uint32_t>::max() - header.blockSize ||
                static_cast<uint64_t>(header.blockCount) * header.blockSize > bytes.size() ||
                PDB::ValidateFile(bytes.data(), bytes.size()) != PDB::ErrorCode::Success)
            {
                throw std::runtime_error("Invalid PDB superblock");
            }

            const auto directory_blocks = block_count(header.directorySize, header.blockSize);
            const auto index_size = directory_blocks * sizeof(uint32_t);
            const auto indices = read_blocks(bytes, header, bytes.subspan(offsetof(PDB::SuperBlock, directoryBlockIndices)), index_size);
            const auto directory_data = read_blocks(bytes, header, indices, header.directorySize);
            const byte_view directory{directory_data};
            const auto stream_count = read_object<uint32_t>(directory);
            if (stream_count > (directory.size() - sizeof(uint32_t)) / sizeof(uint32_t))
            {
                throw std::runtime_error("Invalid PDB stream count");
            }

            const auto sizes = checked_slice(directory, sizeof(uint32_t), static_cast<size_t>(stream_count) * sizeof(uint32_t));
            size_t offset = sizeof(uint32_t) + sizes.size();
            for (uint32_t i = 0; i < stream_count; ++i)
            {
                const auto size = read_object<uint32_t>(sizes, i * sizeof(uint32_t));
                if (size == PDB::NilPageSize)
                {
                    continue;
                }

                if (size > bytes.size() || size > std::numeric_limits<uint32_t>::max() - header.blockSize)
                {
                    throw std::runtime_error("Invalid PDB stream size");
                }

                const auto count = block_count(size, header.blockSize);
                const auto blocks = checked_slice(directory, offset, count * sizeof(uint32_t));
                for (size_t j = 0; j < count; ++j)
                {
                    if (read_object<uint32_t>(blocks, j * sizeof(uint32_t)) >= header.blockCount)
                    {
                        throw std::runtime_error("Invalid PDB stream block");
                    }
                }
                offset += blocks.size();
            }
        }

        PDB::CoalescedMSFStream read_stream(const PDB::RawFile& file, const uint32_t index)
        {
            if (index >= file.GetStreamCount() || file.GetStreamSize(index) == 0)
            {
                throw std::runtime_error("Missing PDB stream");
            }
            return file.CreateMSFStream<PDB::CoalescedMSFStream>(index);
        }

        byte_view stream_bytes(const PDB::CoalescedMSFStream& stream)
        {
            return {stream.GetDataAtOffset<const std::byte>(0), stream.GetSize()};
        }

        PDB::DBI::StreamHeader read_dbi_header(const byte_view bytes)
        {
            const auto header = read_object<PDB::DBI::StreamHeader>(bytes);
            if (header.signature != PDB::DBI::StreamHeader::Signature || header.version < PDB::DBI::StreamHeader::Version::V70)
            {
                throw std::runtime_error("Unsupported PDB DBI header");
            }
            return header;
        }

        pdb_signature read_signature(const PDB::RawFile& file, const std::filesystem::path& path)
        {
            const auto info = read_stream(file, 1);
            const auto header = read_object<PDB::Header>(stream_bytes(info));
            if (header.version < PDB::Header::Version::VC70)
            {
                throw std::runtime_error("Unsupported PDB info header");
            }

            const auto dbi = read_stream(file, 3);
            const auto dbi_header = read_dbi_header(stream_bytes(dbi));
            pdb_signature signature{
                .guid = winpe::detail::format_guid(std::as_bytes(std::span<const PDB::GUID, 1>{&header.guid, 1})),
                .age = dbi_header.age,
                .path = path.string(),
            };

            if (!signature.valid())
            {
                throw std::runtime_error("Invalid PDB identity");
            }

            return signature;
        }

        template <typename Symbol>
        void add_symbol(const byte_view bytes, symbol_map& symbols, const bool replace)
        {
            constexpr auto name_offset = offsetof(Symbol, name);
            const auto section = read_object<uint16_t>(bytes, offsetof(Symbol, section));
            const auto offset = read_object<uint32_t>(bytes, offsetof(Symbol, offset));
            const auto name = utils::safe_buffer_accessor{bytes}.as_string(name_offset);
            if (section == 0 || name.empty())
            {
                return;
            }

            const auto address = std::pair<uint32_t, uint32_t>{section, offset};

            if (replace)
            {
                symbols.insert_or_assign(address, name);
            }
            else
            {
                symbols.try_emplace(address, name);
            }
        }

        void read_symbols(const byte_view bytes, symbol_map& symbols)
        {
            for (size_t offset = 0; offset < bytes.size();)
            {
                const auto header = read_object<PDB::CodeView::DBI::RecordHeader>(bytes, offset);
                const auto size = static_cast<size_t>(header.size) + sizeof(uint16_t);
                if (size < sizeof(header))
                {
                    throw std::runtime_error("Invalid CodeView record size");
                }

                const auto payload = checked_slice(bytes, offset, size).subspan(sizeof(header));

                switch (header.kind)
                {
                case record_kind::S_PUB32:
                    if ((read_object<uint32_t>(payload) & static_cast<uint32_t>(PDB::CodeView::DBI::PublicSymbolFlags::Function)) != 0)
                    {
                        add_symbol<decltype(std::declval<record>().data.S_PUB32)>(payload, symbols, false);
                    }
                    break;
                case record_kind::S_GPROC32:
                case record_kind::S_LPROC32:
                case record_kind::S_GPROC32_ID:
                case record_kind::S_LPROC32_ID:
                case record_kind::S_LPROC32_DPC:
                case record_kind::S_LPROC32_DPC_ID:
                    add_symbol<decltype(std::declval<record>().data.S_GPROC32)>(payload, symbols, true);
                    break;
                default:
                    break;
                }

                offset = (offset + size + 3) & ~size_t{3};
            }
        }

        void read_module_symbols(const PDB::RawFile& file, const byte_view modules, symbol_map& symbols)
        {
            for (size_t offset = 0; offset < modules.size();)
            {
                const auto module = read_object<PDB::DBI::ModuleInfo>(modules, offset);
                offset += sizeof(module);

                for (size_t i = 0; i < 2; ++i)
                {
                    const auto name = utils::safe_buffer_accessor{modules}.as_string(offset);
                    offset += name.size() + 1;
                }

                offset = (offset + 3) & ~size_t{3};

                if (offset > modules.size())
                {
                    throw std::runtime_error("Invalid PDB module alignment");
                }

                if (module.moduleSymbolStreamIndex == PDB::NilStreamIndex || module.symbolSize == 0)
                {
                    continue;
                }

                const auto stream = read_stream(file, module.moduleSymbolStreamIndex);
                const auto bytes = checked_slice(stream_bytes(stream), 0, module.symbolSize);
                if (read_object<uint32_t>(bytes) != 4)
                {
                    throw std::runtime_error("Unsupported CodeView module signature");
                }

                read_symbols(bytes.subspan(sizeof(uint32_t)), symbols);
            }
        }

        bool signatures_match(const pdb_signature& lhs, const pdb_signature& rhs)
        {
            return winpe::detail::normalize_guid(lhs.guid) == winpe::detail::normalize_guid(rhs.guid) && lhs.age == rhs.age;
        }
    }

    pdb_file::pdb_file(std::filesystem::path path)
        : path_(std::move(path))
    {
    }

    pdb_read_result pdb_file::read()
    {
        this->signature = {};
        this->symbols.clear();
        try
        {
            const auto data = utils::io::read_file(this->path_);
            validate_msf(data);

            const auto file = PDB::CreateRawFile(data.data());
            auto parsed_signature = read_signature(file, this->path_);
            symbol_map parsed_symbols{};

            const auto dbi = read_stream(file, 3);
            const auto bytes = stream_bytes(dbi);
            const auto header = read_dbi_header(bytes);
            if (header.symbolRecordStreamIndex != PDB::NilStreamIndex)
            {
                const auto stream = read_stream(file, header.symbolRecordStreamIndex);
                read_symbols(stream_bytes(stream), parsed_symbols);
            }
            read_module_symbols(file, checked_slice(bytes, sizeof(header), header.moduleInfoSize), parsed_symbols);

            this->signature = std::move(parsed_signature);
            this->symbols = std::move(parsed_symbols);
            return {.success = true};
        }
        catch (const std::exception& e)
        {
            return {.error = e.what()};
        }
    }

    pdb_validation_result pdb_file::validate(const pdb_signature& expected) const
    {
        std::vector<std::byte> data{};
        if (!utils::io::read_file(this->path_, &data))
        {
            return {.status = pdb_validation_status::validation_error, .error = "Failed to read PDB file"};
        }

        try
        {
            validate_msf(data);
            const auto file = PDB::CreateRawFile(data.data());
            const auto actual = read_signature(file, this->path_);
            return {.status = signatures_match(actual, expected) ? pdb_validation_status::match : pdb_validation_status::mismatch};
        }
        catch (const std::exception& e)
        {
            return {.status = pdb_validation_status::invalid_artifact, .error = e.what()};
        }
    }

    bool pdb_file::matches(const pdb_signature& expected) const
    {
        return signatures_match(this->signature, expected);
    }
}

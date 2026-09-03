#include "../std_include.hpp"
#include "macos_cache_symbols.hpp"

#include "macho_export_trie.hpp"

#include <algorithm>
#include <cstring>

namespace sogen
{
    namespace
    {
        constexpr uint64_t MACHO_HEADER_READ = 0x4000;
        constexpr uint64_t MAX_EXPORT_TRIE = 32ull * 1024 * 1024;

        struct trie_location
        {
            uint64_t address{};
            uint64_t size{};
        };

        // The trie's own load command gives a *file* offset, and in a shared cache that offset belongs to
        // whichever subcache holds __LINKEDIT rather than to the image. Converting it through the
        // __LINKEDIT segment -- vmaddr plus the distance from the segment's fileoff -- lands on the
        // address the reader can actually find, which is the only way to reach it from here.
        std::optional<trie_location> find_export_trie(const std::span<const std::byte> image)
        {
            if (image.size() < sizeof(macho::mach_header_64))
            {
                return std::nullopt;
            }

            macho::mach_header_64 header{};
            std::memcpy(&header, image.data(), sizeof(header));

            if (header.magic != macho::MH_MAGIC_64)
            {
                return std::nullopt;
            }

            std::optional<uint64_t> linkedit_vmaddr{};
            std::optional<uint64_t> linkedit_fileoff{};
            std::optional<macho::linkedit_data_command> trie{};

            uint64_t cursor = sizeof(header);
            for (uint32_t i = 0; i < header.ncmds; ++i)
            {
                if (cursor + sizeof(macho::load_command) > image.size())
                {
                    return std::nullopt;
                }

                macho::load_command command{};
                std::memcpy(&command, image.data() + cursor, sizeof(command));

                if (command.cmdsize < sizeof(macho::load_command) || cursor + command.cmdsize > image.size())
                {
                    return std::nullopt;
                }

                if (command.cmd == macho::LC_SEGMENT_64 && command.cmdsize >= sizeof(macho::segment_command_64))
                {
                    macho::segment_command_64 segment{};
                    std::memcpy(&segment, image.data() + cursor, sizeof(segment));

                    // segname is a fixed 16-byte field that is not required to be terminated, so the
                    // length is found rather than assumed.
                    const auto* first = segment.segname.data();
                    const auto* last = std::find(first, first + segment.segname.size(), '\0');

                    if (std::string_view{first, static_cast<size_t>(last - first)} == "__LINKEDIT")
                    {
                        linkedit_vmaddr = segment.vmaddr;
                        linkedit_fileoff = segment.fileoff;
                    }
                }
                else if (command.cmd == macho::LC_DYLD_EXPORTS_TRIE && command.cmdsize >= sizeof(macho::linkedit_data_command))
                {
                    macho::linkedit_data_command data{};
                    std::memcpy(&data, image.data() + cursor, sizeof(data));
                    trie = data;
                }

                cursor += command.cmdsize;
            }

            if (!trie || !linkedit_vmaddr || !linkedit_fileoff || trie->datasize == 0 || trie->datasize > MAX_EXPORT_TRIE)
            {
                return std::nullopt;
            }

            if (trie->dataoff < *linkedit_fileoff)
            {
                return std::nullopt;
            }

            return trie_location{.address = *linkedit_vmaddr + (trie->dataoff - *linkedit_fileoff), .size = trie->datasize};
        }
    }

    const std::vector<macos_cache_symbol>& macos_cache_symbols::table_for(const dyld_cache_image_entry& image) const
    {
        const auto existing = this->tables_.find(image.address);
        if (existing != this->tables_.end())
        {
            return existing->second;
        }

        std::vector<macos_cache_symbol> symbols{};

        const auto header = this->cache_->read_at_address(image.address, MACHO_HEADER_READ);
        if (const auto location = find_export_trie(header))
        {
            const auto bytes = this->cache_->read_at_address(location->address, location->size);
            if (!bytes.empty())
            {
                const std::span<const uint8_t> trie{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};

                walk_macho_export_trie(trie, [&](const std::string& name, const uint64_t offset, const uint64_t flags) {
                    symbols.push_back(macos_cache_symbol{.name = name, .address = image.address + offset, .flags = flags});
                });
            }
        }

        std::ranges::sort(symbols, {}, &macos_cache_symbol::address);

        return this->tables_.emplace(image.address, std::move(symbols)).first->second;
    }

    const std::vector<macos_cache_symbol>& macos_cache_symbols::exports_of(const std::string_view image_path) const
    {
        static const std::vector<macos_cache_symbol> none{};

        if (this->cache_ == nullptr)
        {
            return none;
        }

        const auto* image = this->cache_->find_image_by_path(image_path);
        return image == nullptr ? none : this->table_for(*image);
    }

    std::optional<uint64_t> macos_cache_symbols::find_export(const std::string_view image_path, const std::string_view symbol) const
    {
        if (this->cache_ == nullptr)
        {
            return std::nullopt;
        }

        const auto* image = this->cache_->find_image_by_path(image_path);
        if (image == nullptr)
        {
            return std::nullopt;
        }

        for (const auto& entry : this->table_for(*image))
        {
            if (entry.name == symbol)
            {
                return entry.has_address() ? std::optional<uint64_t>{entry.address} : std::nullopt;
            }
        }

        return std::nullopt;
    }

    std::optional<macos_cache_symbol> macos_cache_symbols::lookup(const uint64_t unslid_address) const
    {
        const auto* image = this->cache_->find_image_by_address(unslid_address);
        if (image == nullptr)
        {
            return std::nullopt;
        }

        const auto& symbols = this->table_for(*image);
        if (symbols.empty())
        {
            return std::nullopt;
        }

        const auto above = std::ranges::upper_bound(symbols, unslid_address, {}, &macos_cache_symbol::address);
        if (above == symbols.begin())
        {
            return std::nullopt;
        }

        auto found = *std::prev(above);
        found.offset = unslid_address - found.address;
        return found;
    }
}

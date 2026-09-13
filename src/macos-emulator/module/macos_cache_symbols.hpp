#pragma once

#include "dyld_shared_cache.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    namespace macho_export_flag
    {
        constexpr uint64_t KIND_MASK = 0x03;
        constexpr uint64_t KIND_REGULAR = 0x00;
        constexpr uint64_t REEXPORT = 0x08;
        constexpr uint64_t STUB_AND_RESOLVER = 0x10;
    }

    struct macos_cache_symbol
    {
        std::string name{};
        uint64_t address{};
        uint64_t offset{};
        uint64_t flags{};

        // Only a regular export's trie payload is an image offset. A re-export's is a library ordinal
        // followed by the name in that library, and a resolver's first word is a stub rather than the
        // function -- reading either as an address produces a plausible-looking number that points at
        // nothing. Measured: CoreGraphics re-exports _CGWindowContextCreate from SkyLight, and taking its
        // payload as an offset lands on an unaligned address shared with a second symbol.
        bool has_address() const
        {
            return (this->flags & macho_export_flag::KIND_MASK) == macho_export_flag::KIND_REGULAR &&
                   (this->flags & (macho_export_flag::REEXPORT | macho_export_flag::STUB_AND_RESOLVER)) == 0;
        }
    };

    // Turns an address in the shared cache into the exported function containing it.
    //
    // The cache carries no symbol table for its images, only an export trie each, so this walks the trie
    // of whichever image holds the address and keeps the result sorted. Tables are built on demand: a
    // cache has thousands of images and a run touches a handful.
    class macos_cache_symbols
    {
      public:
        macos_cache_symbols() = default;

        explicit macos_cache_symbols(const dyld_shared_cache_reader& cache)
            : cache_(&cache)
        {
        }

        bool has_cache() const
        {
            return this->cache_ != nullptr;
        }

        // The nearest export at or below the address, within the image containing it. Absent when the
        // address is outside every image, the image exports nothing, or nothing is exported below it --
        // which is not the same as "no symbol exists": a static function has no export, and the nearest
        // export above it would be a wrong answer rather than an imprecise one.
        std::optional<macos_cache_symbol> lookup(uint64_t unslid_address) const;

        // The other direction: the address an image exports under a name. Absent when the cache holds no
        // image at that install name or the image does not export the symbol -- both of which are
        // ordinary outcomes, because these names drift between macOS releases.
        std::optional<uint64_t> find_export(std::string_view image_path, std::string_view symbol) const;

        // Every export of an image, in address order. Empty for an image the cache does not hold or one
        // whose trie is unreadable. This is what lets the GUI routine tables be regenerated from the
        // cache itself rather than from an external extractor.
        const std::vector<macos_cache_symbol>& exports_of(std::string_view image_path) const;

      private:
        const std::vector<macos_cache_symbol>& table_for(const dyld_cache_image_entry& image) const;

        const dyld_shared_cache_reader* cache_{};
        mutable std::map<uint64_t, std::vector<macos_cache_symbol>> tables_{};
    };
}

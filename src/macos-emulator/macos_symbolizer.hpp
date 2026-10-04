#pragma once

#include "std_include.hpp"

#include "module/dyld_shared_cache.hpp"
#include "module/macos_cache_symbols.hpp"
#include "module/macos_module_manager.hpp"

#include <optional>
#include <string>

namespace sogen
{
    struct macos_address_origin
    {
        std::string module{};
        uint64_t base{};
        uint64_t offset{};
        bool in_shared_cache{};

        // The nearest export at or below the address, when there is one. Empty is not "no function":
        // a static function has no export, and the name here is then the exported one before it. That is
        // why the offset from the symbol is kept -- a large one is the signal that the answer is nearby
        // rather than exact.
        std::string symbol{};
        uint64_t symbol_offset{};
    };

    class macos_symbolizer
    {
      public:
        void attach_modules(const macos_module_manager& modules);
        bool attach_shared_cache(const std::filesystem::path& host_cache_path);

        bool has_shared_cache() const
        {
            return this->cache_.has_value();
        }

        std::optional<macos_address_origin> describe(uint64_t address) const;
        std::string format(uint64_t address) const;

      private:
        const macos_module_manager* modules_{};
        std::optional<dyld_shared_cache_reader> cache_{};
        std::optional<macos_cache_symbols> symbols_{};
    };
}

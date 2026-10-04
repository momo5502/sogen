#include "std_include.hpp"
#include "macos_symbolizer.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>

namespace sogen
{
    void macos_symbolizer::attach_modules(const macos_module_manager& modules)
    {
        this->modules_ = &modules;
    }

    bool macos_symbolizer::attach_shared_cache(const std::filesystem::path& host_cache_path)
    {
        std::error_code error{};
        if (!std::filesystem::is_regular_file(host_cache_path, error) || error)
        {
            return false;
        }

        try
        {
            this->cache_ = dyld_shared_cache_reader::open(host_cache_path);
            this->symbols_.emplace(*this->cache_);
        }
        catch (const std::exception&)
        {
            this->cache_.reset();
            return false;
        }

        return true;
    }

    std::optional<macos_address_origin> macos_symbolizer::describe(const uint64_t address) const
    {
        if (this->modules_)
        {
            for (const auto& [start, module] : this->modules_->get_modules())
            {
                if (!module.contains(address))
                {
                    continue;
                }

                macos_address_origin origin{};
                origin.module = module.name;
                origin.base = module.image_base;
                origin.offset = address - module.image_base;
                return origin;
            }
        }

        if (this->cache_)
        {
            const auto span_start = this->cache_->shared_region_start();
            const auto span_size = this->cache_->shared_region_size();

            // Private mode maps at slide 0, so a guest address inside the span is already the unslid one.
            if (address >= span_start && address - span_start < span_size)
            {
                macos_address_origin origin{};
                origin.in_shared_cache = true;

                if (const auto* image = this->cache_->find_image_by_address(address))
                {
                    origin.module = image->path;
                    origin.base = image->address;
                    origin.offset = address - image->address;

                    if (this->symbols_)
                    {
                        if (const auto symbol = this->symbols_->lookup(address))
                        {
                            origin.symbol = symbol->name;
                            origin.symbol_offset = symbol->offset;
                        }
                    }
                }
                else
                {
                    origin.module = "dyld_shared_cache";
                    origin.base = span_start;
                    origin.offset = address - span_start;
                }

                return origin;
            }
        }

        return std::nullopt;
    }

    std::string macos_symbolizer::format(const uint64_t address) const
    {
        const auto origin = this->describe(address);

        std::array<char, 512> buffer{};
        if (!origin)
        {
            std::snprintf(buffer.data(), buffer.size(), "0x%" PRIx64, address);
            return buffer.data();
        }

        // The symbol replaces the module offset when there is one, because a name is what a reader can act
        // on. The module stays, since two images can export the same name.
        if (!origin->symbol.empty())
        {
            std::snprintf(buffer.data(), buffer.size(), "0x%" PRIx64 " (%s`%s+0x%" PRIx64 ")", address,
                          std::filesystem::path{origin->module}.filename().string().c_str(), origin->symbol.c_str(), origin->symbol_offset);
            return buffer.data();
        }

        std::snprintf(buffer.data(), buffer.size(), "0x%" PRIx64 " (%s+0x%" PRIx64 "%s)", address, origin->module.c_str(), origin->offset,
                      origin->in_shared_cache ? ", shared cache" : "");
        return buffer.data();
    }
}

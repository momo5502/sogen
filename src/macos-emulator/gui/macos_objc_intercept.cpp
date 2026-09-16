#include "../std_include.hpp"
#include "macos_objc_intercept.hpp"

#include "../macos_emulator.hpp"
#include "../module/dyld_cache_pager.hpp"

#include <algorithm>
#include <optional>

namespace sogen
{
    namespace
    {
        // Layout constants measured on the 25G76 cache and cross-checked against the host libobjc running
        // on the same cache; the walk and the numbers are in
        constexpr uint64_t OBJC_CLASS_ISA = 0x00;
        constexpr uint64_t OBJC_CLASS_BITS = 0x20;
        constexpr uint64_t FAST_DATA_MASK = 0x00007ffffffffff8ULL;
        constexpr uint64_t SIGNATURE_STRIP = 0x0000ffffffffffffULL;
        constexpr uint64_t CLASS_RO_BASE_METHODS = 0x20;
        constexpr uint64_t BASE_METHODS_LIST_OF_LISTS = 1;
        constexpr uint32_t METHOD_LIST_IS_RELATIVE = 0x80000000;
        constexpr uint32_t METHOD_LIST_SELECTOR_OFFSETS = 0x40000000;
        constexpr uint32_t METHOD_LIST_ENTSIZE_MASK = 0x0000fffc;
        constexpr uint64_t RELATIVE_METHOD_ENTRY_SIZE = 12;
        constexpr uint32_t MAX_LIST_ENTRIES = 0x10000;
        // The objc opt blob on 25G76 is version 2: an array of 64-bit VM offsets from the shared region
        // start. +0x30 holds the selector string base that relative method name offsets resolve against
        // (libobjc's sharedCacheRelativeMethodBase).
        constexpr uint32_t OBJC_OPT_VERSION = 2;
        constexpr uint64_t OBJC_OPT_SELECTOR_BASE_OFFSET = 0x30;
        constexpr size_t MAX_CSTRING = 256;

        // Reads guest memory that may sit in an unmaterialised cache chunk. Host-side reads do not fault
        // the lazy pager, so each read pulls its chunk in first.
        class objc_guest_reader
        {
          public:
            explicit objc_guest_reader(macos_emulator& emu)
                : emu_(&emu),
                  pager_(emu.cache_pager.get())
            {
            }

            bool read(const uint64_t address, void* destination, const size_t size) const
            {
                if (this->pager_ != nullptr)
                {
                    for (const uint64_t endpoint : {address, address + size - 1})
                    {
                        if (this->pager_->covers(endpoint) && !this->pager_->page_in(endpoint))
                        {
                            return false;
                        }
                    }
                }

                return this->emu_->memory.try_read_memory(address, destination, size);
            }

            template <typename T>
            std::optional<T> read_value(const uint64_t address) const
            {
                T value{};
                if (!this->read(address, &value, sizeof(value)))
                {
                    return std::nullopt;
                }

                return value;
            }

            std::optional<std::string> read_cstring(const uint64_t address) const
            {
                std::string result{};
                uint64_t cursor = address;

                while (result.size() < MAX_CSTRING)
                {
                    const auto page_left = static_cast<size_t>(MACOS_PAGE_SIZE - cursor % MACOS_PAGE_SIZE);
                    const auto chunk = std::min({page_left, MAX_CSTRING - result.size(), size_t{128}});

                    char buffer[128]{};
                    if (!this->read(cursor, buffer, chunk))
                    {
                        return std::nullopt;
                    }

                    for (size_t i = 0; i < chunk; ++i)
                    {
                        if (buffer[i] == '\0')
                        {
                            return result;
                        }

                        result.push_back(buffer[i]);
                    }

                    cursor += chunk;
                }

                return std::nullopt;
            }

          private:
            macos_emulator* emu_{};
            dyld_cache_pager* pager_{};
        };

        struct method_search
        {
            std::string_view selector{};
            uint64_t sel{};
            uint64_t imp{};
            bool found{};
        };

        std::optional<uint64_t> selector_string_base(const objc_guest_reader& reader, const dyld_shared_cache_reader& cache)
        {
            const auto start = cache.shared_region_start();

            const auto opts_offset = reader.read_value<uint64_t>(start + macho::dyld_cache::OBJC_OPTS_OFFSET);
            if (!opts_offset || *opts_offset == 0)
            {
                return std::nullopt;
            }

            const auto opts = start + *opts_offset;
            const auto version = reader.read_value<uint32_t>(opts);
            if (!version || *version != OBJC_OPT_VERSION)
            {
                return std::nullopt;
            }

            const auto base = reader.read_value<uint64_t>(opts + OBJC_OPT_SELECTOR_BASE_OFFSET);
            if (!base || *base == 0)
            {
                return std::nullopt;
            }

            return start + *base;
        }

        void scan_method_list(const objc_guest_reader& reader, const uint64_t selector_base, const uint64_t list, method_search& search)
        {
            const auto entsize_flags = reader.read_value<uint32_t>(list);
            const auto count = reader.read_value<uint32_t>(list + 4);
            if (!entsize_flags || !count)
            {
                return;
            }

            const auto entsize = *entsize_flags & METHOD_LIST_ENTSIZE_MASK;
            const bool relative = (*entsize_flags & METHOD_LIST_IS_RELATIVE) != 0;
            const bool selector_offsets = (*entsize_flags & METHOD_LIST_SELECTOR_OFFSETS) != 0;
            if (!relative || !selector_offsets || entsize != RELATIVE_METHOD_ENTRY_SIZE || *count == 0 || *count > MAX_LIST_ENTRIES)
            {
                return;
            }

            for (uint32_t i = 0; i < *count; ++i)
            {
                const auto entry = list + 8 + static_cast<uint64_t>(i) * RELATIVE_METHOD_ENTRY_SIZE;
                const auto name_offset = reader.read_value<int32_t>(entry);
                const auto imp_offset = reader.read_value<int32_t>(entry + 8);
                if (!name_offset || !imp_offset)
                {
                    return;
                }

                const auto sel = static_cast<uint64_t>(static_cast<int64_t>(selector_base) + *name_offset);
                const auto name = reader.read_cstring(sel);
                if (name && *name == search.selector)
                {
                    search.sel = sel;
                    search.imp = static_cast<uint64_t>(static_cast<int64_t>(entry + 8) + *imp_offset);
                    search.found = true;
                    return;
                }
            }
        }

        // class_ro_t::baseMethods is either one method_list_t or, with the low tag bit set, a
        // relative_list_list_t: an array of 8-byte entries {uint16 imageIndex; int48 listOffset} whose
        // offset is relative to the entry itself. The lists are scanned in array order, which is the order
        // the runtime's lookup iterates them, so the first match is the IMP a guest call would reach.
        void scan_base_methods(const objc_guest_reader& reader, const uint64_t selector_base, const uint64_t ro, method_search& search)
        {
            const auto raw = reader.read_value<uint64_t>(ro + CLASS_RO_BASE_METHODS);
            if (!raw)
            {
                return;
            }

            const auto base_methods = *raw & SIGNATURE_STRIP;
            if (base_methods == 0)
            {
                return;
            }

            if ((base_methods & BASE_METHODS_LIST_OF_LISTS) == 0)
            {
                scan_method_list(reader, selector_base, base_methods, search);
                return;
            }

            const auto lists = base_methods & ~BASE_METHODS_LIST_OF_LISTS;
            const auto count = reader.read_value<uint32_t>(lists + 4);
            if (!count || *count == 0 || *count > MAX_LIST_ENTRIES)
            {
                return;
            }

            for (uint32_t i = 0; i < *count && !search.found; ++i)
            {
                const auto entry = lists + 8 + static_cast<uint64_t>(i) * 8;
                const auto packed = reader.read_value<uint64_t>(entry);
                if (!packed)
                {
                    return;
                }

                const auto offset = static_cast<int64_t>(*packed) >> 16;
                scan_method_list(reader, selector_base, static_cast<uint64_t>(static_cast<int64_t>(entry) + offset), search);
            }
        }

        bool in_shared_region(const dyld_shared_cache_reader& cache, const uint64_t address)
        {
            return address >= cache.shared_region_start() && address < cache.shared_region_start() + cache.shared_region_size();
        }

        bool find_method(const objc_guest_reader& reader, const dyld_shared_cache_reader& cache, const uint64_t selector_base,
                         const uint64_t class_address, const bool class_method, method_search& search)
        {
            auto target = class_address;
            if (class_method)
            {
                const auto isa = reader.read_value<uint64_t>(class_address + OBJC_CLASS_ISA);
                if (!isa)
                {
                    return false;
                }

                target = *isa & SIGNATURE_STRIP;
            }

            const auto bits = reader.read_value<uint64_t>(target + OBJC_CLASS_BITS);
            if (!bits)
            {
                return false;
            }

            const auto ro = *bits & FAST_DATA_MASK;
            if (!in_shared_region(cache, ro))
            {
                return false;
            }

            scan_base_methods(reader, selector_base, ro, search);
            return true;
        }
    }

    std::vector<macos_objc_method_binding> bind_objc_methods(macos_emulator& emu, const dyld_shared_cache_reader& cache,
                                                             const macos_cache_symbols& symbols, macos_native_dispatch& dispatch,
                                                             const std::vector<macos_objc_method>& methods)
    {
        std::vector<macos_objc_method_binding> bindings{};
        bindings.reserve(methods.size());

        const objc_guest_reader reader{emu};
        const auto selector_base = selector_string_base(reader, cache);
        if (!selector_base && !methods.empty())
        {
            emu.log.warn("ObjC method interception found no selector table in the shared cache on this system\n");
        }

        size_t bound = 0;

        for (const auto& method : methods)
        {
            macos_objc_method_binding binding{
                .name = std::string{method.class_method ? "+[" : "-["} + method.class_name + " " + method.selector + "]",
            };

            const auto fail = [&](const char* reason) {
                emu.log.warn("ObjC method %s is not available on this system (%s); calls to it will be reported as unimplemented\n",
                             binding.name.c_str(), reason);
                bindings.push_back(std::move(binding));
            };

            if (method.handler == nullptr || method.selector.empty() || method.class_name.empty())
            {
                fail("incomplete registration");
                continue;
            }

            if (!selector_base)
            {
                fail("no selector table");
                continue;
            }

            const auto class_symbol = "_OBJC_CLASS_$_" + method.class_name;
            const auto class_address = symbols.find_export(method.image, class_symbol);
            if (!class_address)
            {
                fail("class is not exported");
                continue;
            }

            method_search search{.selector = method.selector};
            if (!find_method(reader, cache, *selector_base, *class_address, method.class_method, search))
            {
                fail("class metadata is unreadable");
                continue;
            }

            if (!search.found)
            {
                fail("selector not in the class's method lists");
                continue;
            }

            if ((search.imp & 3u) != 0 || !in_shared_region(cache, search.imp))
            {
                fail("implausible method implementation address");
                continue;
            }

            if (!patch_native_entry(emu, search.imp))
            {
                fail("failed to install the native trap");
                continue;
            }

            dispatch.bind_entry(search.imp, binding.name, method.handler);

            binding.sel = search.sel;
            binding.imp = search.imp;
            binding.bound = true;
            ++bound;
            bindings.push_back(std::move(binding));
        }

        emu.log.info("ObjC: %zu of %zu methods intercepted, %zu unavailable on this system\n", bound, methods.size(),
                     methods.size() - bound);

        return bindings;
    }
}

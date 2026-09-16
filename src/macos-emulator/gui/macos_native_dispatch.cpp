#include "../std_include.hpp"
#include "macos_native_dispatch.hpp"

#include "../macos_emulator.hpp"
#include "../module/dyld_cache_pager.hpp"

#include <algorithm>
#include <cstring>

namespace sogen
{
    namespace
    {
        constexpr size_t MACOS_NATIVE_MAX_ARGUMENTS = 8;

        std::string routine_key(const std::string_view image, const std::string_view symbol)
        {
            return std::string{image} + '|' + std::string{symbol};
        }
    }

    uint64_t macos_native_call::arg(const size_t index) const
    {
        if (index >= MACOS_NATIVE_MAX_ARGUMENTS)
        {
            return 0;
        }

        return this->emu.reg(static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::x0) + index));
    }

    float macos_native_call::arg_float(const size_t index) const
    {
        if (index >= MACOS_NATIVE_MAX_ARGUMENTS)
        {
            return 0.0f;
        }

        const auto raw =
            static_cast<uint32_t>(this->emu.reg(static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::s0) + index)));

        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    double macos_native_call::arg_double(const size_t index) const
    {
        if (index >= MACOS_NATIVE_MAX_ARGUMENTS)
        {
            return 0.0;
        }

        const auto raw = this->emu.reg(static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::d0) + index));

        double value = 0.0;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    void macos_native_call::ret(const uint64_t value) const
    {
        this->ret_pair(value, 0);
    }

    void macos_native_call::ret_pair(const uint64_t low, const uint64_t high) const
    {
        this->emu.reg(arm64_register::x0, low);
        this->emu.reg(arm64_register::x1, high);
    }

    bool patch_native_entry(macos_emulator& emu, const uint64_t address)
    {
        if ((address & 3u) != 0)
        {
            return false;
        }

        // Under lazy cache paging the export's bytes do not exist until something faults them in, and a
        // host-side read is not a fault. Patching has to pull the page in itself, and pin it: an evicted
        // chunk is re-fetched from the file, which has never heard of the trap.
        auto* pager = emu.cache_pager.get();
        const bool paged = pager != nullptr && pager->covers(address);
        if (paged && !pager->page_in(address))
        {
            return false;
        }

        uint32_t existing = 0;
        if (!emu.memory.try_read_memory(address, &existing, sizeof(existing)))
        {
            return false;
        }

        if (existing == MACOS_ARM64_SVC_80)
        {
            return true;
        }

        // Guest page protections do not stand in the way: try_write_memory writes through them, which is
        // what lets an r-x cache page be patched at all. Measured, so no read-back check follows -- a
        // write that reports success is one whose bytes are there.
        const uint32_t trap = MACOS_ARM64_SVC_80;
        if (!emu.memory.try_write_memory(address, &trap, sizeof(trap)))
        {
            return false;
        }

        if (paged)
        {
            (void)pager->pin(address);
        }

        return true;
    }

    void macos_native_dispatch::register_routine(std::string image, std::string symbol, const macos_native_handler handler)
    {
        if (handler == nullptr || symbol.empty())
        {
            return;
        }

        if (!this->registered_keys_.insert(routine_key(image, symbol)).second)
        {
            return;
        }

        this->routines_.push_back(macos_native_routine{
            .image = std::move(image),
            .symbol = std::move(symbol),
            .handler = handler,
        });
    }

    void macos_native_dispatch::bind_entry(const uint64_t entry, std::string name, const macos_native_handler handler)
    {
        if (handler == nullptr)
        {
            return;
        }

        this->bindings_[entry] = macos_native_binding{.name = std::move(name), .handler = handler};
    }

    size_t macos_native_dispatch::bind(macos_emulator& emu, const macos_cache_symbols& symbols)
    {
        size_t newly_bound = 0;

        for (const auto& routine : this->routines_)
        {
            if (this->bound_symbols_.contains(routine.symbol))
            {
                continue;
            }

            const auto address = symbols.find_export(routine.image, routine.symbol);
            if (!address.has_value())
            {
                if (std::ranges::find(this->unbound_, routine.symbol) == this->unbound_.end())
                {
                    this->unbound_.push_back(routine.symbol);
                    emu.log.warn("GUI routine %s is not exported by %s on this system; calls to it will be reported as "
                                 "unimplemented\n",
                                 routine.symbol.c_str(), routine.image.c_str());
                }

                continue;
            }

            if (!patch_native_entry(emu, *address))
            {
                emu.log.warn("Failed to install the native trap for %s at 0x%" PRIx64 "\n", routine.symbol.c_str(), *address);

                // A failed patch is an unbound routine like any other: counting it keeps bound + unbound
                // equal to registered, so a bind that installed nothing cannot report a silent 0 of N.
                if (std::ranges::find(this->unbound_, routine.symbol) == this->unbound_.end())
                {
                    this->unbound_.push_back(routine.symbol);
                }

                continue;
            }

            this->bind_entry(*address, routine.symbol, routine.handler);
            this->bound_symbols_.insert(routine.symbol);
            ++newly_bound;
        }

        return newly_bound;
    }

    bool macos_native_dispatch::handles(const uint64_t entry) const
    {
        return this->bindings_.contains(entry);
    }

    std::string_view macos_native_dispatch::name_of(const uint64_t entry) const
    {
        const auto found = this->bindings_.find(entry);
        return (found == this->bindings_.end()) ? std::string_view{} : std::string_view{found->second.name};
    }

    macos_native_handler macos_native_dispatch::handler_for(const uint64_t entry) const
    {
        const auto found = this->bindings_.find(entry);
        return found == this->bindings_.end() ? nullptr : found->second.handler;
    }

    bool macos_native_dispatch::invoke(macos_emulator& emu, const uint64_t entry)
    {
        const auto found = this->bindings_.find(entry);
        if (found == this->bindings_.end())
        {
            return false;
        }

        auto& backend = emu.emu();
        const auto link_register = backend.reg(arm64_register::lr);

        const macos_native_call call{
            .emu_ref = emu,
            .emu = backend,
            .entry = entry,
            .name = found->second.name,
        };

        found->second.handler(call);

        // A handler that has started a guest call has already set pc to the callee, and overwriting it
        // with lr here would skip the call it just arranged.
        if (backend.reg(arm64_register::pc) == entry + 4)
        {
            backend.reg(arm64_register::pc, link_register);
        }

        return true;
    }

    void macos_native_dispatch::reset()
    {
        this->bindings_.clear();
        this->bound_symbols_.clear();
        this->unbound_.clear();
    }
}

#pragma once

#include "../std_include.hpp"
#include "../module/macos_cache_symbols.hpp"

#include <arch_emulator.hpp>

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    class macos_emulator;

    struct macos_native_call
    {
        macos_emulator& emu_ref;
        arm64_64_emulator& emu;
        uint64_t entry{};
        std::string_view name{};

        uint64_t arg(size_t index) const;

        // AAPCS puts floating-point arguments in their own register bank, so they are not at any index
        // into arg(). SLSNewWindow's x and y are C floats and arrive in s0 and s1 while its connection id
        // and type are in x0 and x1; reading them as integer arguments yields the region pointer.
        float arg_float(size_t index) const;
        double arg_double(size_t index) const;

        void ret(uint64_t value) const;
        void ret_pair(uint64_t low, uint64_t high) const;
    };

    using macos_native_handler = void (*)(const macos_native_call&);

    struct macos_native_routine
    {
        std::string image{};
        std::string symbol{};
        macos_native_handler handler{};
    };

    struct macos_native_binding
    {
        std::string name{};
        macos_native_handler handler{};
    };

    // Replaces a shared cache export with a C++ implementation by writing svc #0x80 over its first
    // instruction. The cache is mapped MAP_PRIVATE, so the write is copy-on-write and the host's cache
    // file is untouched. The trap lands in the Darwin svc hook that already exists, which is the one
    // place unicorn supports writing pc from -- a UC_HOOK_CODE callback does not, which is why this is a
    // patch rather than an execution hook plus a redirect.
    class macos_native_dispatch
    {
      public:
        void register_routine(std::string image, std::string symbol, macos_native_handler handler);

        // Never fails the run. A symbol this macOS release does not export is recorded in
        // unbound_symbols() and logged once, because these names drift between releases and a missing one
        // has to degrade to an unimplemented-routine report rather than to a crash. A resolved export
        // whose trap cannot be installed lands in unbound_symbols() too, so bound + unbound always add
        // up to registered_count().
        size_t bind(macos_emulator& emu, const macos_cache_symbols& symbols);
        void bind_entry(uint64_t entry, std::string name, macos_native_handler handler);

        bool handles(uint64_t entry) const;
        bool invoke(macos_emulator& emu, uint64_t entry);

        size_t registered_count() const
        {
            return this->routines_.size();
        }

        size_t bound_count() const
        {
            return this->bindings_.size();
        }

        const std::vector<macos_native_routine>& routines() const
        {
            return this->routines_;
        }

        const std::vector<std::string>& unbound_symbols() const
        {
            return this->unbound_;
        }

        std::string_view name_of(uint64_t entry) const;

        // The handler already bound at an entry, so that a routine can be reinstalled at the same
        // address with a different interception mechanism without naming its handler twice.
        macos_native_handler handler_for(uint64_t entry) const;

        void reset();

      private:
        std::vector<macos_native_routine> routines_{};
        std::map<uint64_t, macos_native_binding> bindings_{};
        std::set<std::string> registered_keys_{};
        std::set<std::string> bound_symbols_{};
        std::vector<std::string> unbound_{};
    };

    bool patch_native_entry(macos_emulator& emu, uint64_t address);
}

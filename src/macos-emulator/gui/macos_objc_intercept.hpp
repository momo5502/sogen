#pragma once

#include "../std_include.hpp"
#include "macos_native_dispatch.hpp"
#include "../module/dyld_shared_cache.hpp"
#include "../module/macos_cache_symbols.hpp"

#include <string>
#include <vector>

namespace sogen
{
    class macos_emulator;

    struct macos_objc_method
    {
        std::string image{};
        std::string class_name{};
        std::string selector{};
        bool class_method{};
        macos_native_handler handler{};
    };

    struct macos_objc_method_binding
    {
        std::string name{};
        uint64_t sel{};
        uint64_t imp{};
        bool bound{};
    };

    // svc-patches ObjC method implementations so calls route to C++ handlers under the macos_native_call
    // contract (x0=self, x1=_cmd, x2+ args). Method IMPs are in no export trie, so the export-based path
    // cannot reach them; this walks the precomputed class metadata the dyld shared cache carries instead.
    // Every method that does not resolve is reported by name in the returned bindings and the log.
    std::vector<macos_objc_method_binding> bind_objc_methods(macos_emulator& emu, const dyld_shared_cache_reader& cache,
                                                             const macos_cache_symbols& symbols, macos_native_dispatch& dispatch,
                                                             const std::vector<macos_objc_method>& methods);
}

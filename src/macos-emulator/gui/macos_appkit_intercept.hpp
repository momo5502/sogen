#pragma once

#include "../std_include.hpp"

#include "../module/dyld_shared_cache.hpp"
#include "../module/macos_cache_symbols.hpp"
#include "macos_layer_tree.hpp"
#include "macos_objc_intercept.hpp"

#include <vector>

namespace sogen
{
    class macos_emulator;

    // AppKit methods whose real implementation depends on a system service sogen does not model, and
    // which abort rather than degrade when it is absent. Each one is answered with the value AppKit
    // itself produces for the absent case, never with a guess at what the service would have said.
    std::vector<macos_objc_method> macos_appkit_methods();

    std::vector<macos_layer_tree_binding> macos_appkit_bind(macos_emulator& emu, const dyld_shared_cache_reader& cache,
                                                            const macos_cache_symbols& symbols, macos_native_dispatch& dispatch);
}

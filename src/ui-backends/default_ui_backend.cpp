#include "std_include.hpp"
#include <platform/ui_backend.hpp>

namespace sogen
{
    std::unique_ptr<ui_backend> create_default_ui_backend(const ui_backend_options& options)
    {
#ifdef OS_EMSCRIPTEN
        (void)options;
        return create_web_ui_backend();
#elif defined(SOGEN_HAS_SDL3)
        return create_sdl_ui_backend(options);
#else
        return std::make_unique<null_ui_backend>();
#endif
    }
}

#include "../std_include.hpp"
#include <platform/ui_backend.hpp>

namespace sogen
{
    std::unique_ptr<ui_backend> create_default_ui_backend()
    {
#ifdef OS_EMSCRIPTEN
        return create_web_ui_backend();
#elif defined(SOGEN_HAS_SDL3)
        return create_sdl_ui_backend();
#else
        return std::make_unique<null_ui_backend>();
#endif
    }
}

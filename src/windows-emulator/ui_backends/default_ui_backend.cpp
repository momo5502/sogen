#include "../std_include.hpp"
#include <platform/ui_backend.hpp>

namespace sogen
{
    std::unique_ptr<ui_backend> create_win32_ui_backend();
    std::unique_ptr<ui_backend> create_sdl_ui_backend();

    std::unique_ptr<ui_backend> create_default_ui_backend()
    {
#ifdef SOGEN_USE_SDL3_UI_BACKEND
        return create_sdl_ui_backend();
#else
        return create_win32_ui_backend();
#endif
    }
}

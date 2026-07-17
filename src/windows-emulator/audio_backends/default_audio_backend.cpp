#include "../std_include.hpp"
#include <platform/audio_backend.hpp>

namespace sogen
{
    std::unique_ptr<audio_backend> create_default_audio_backend()
    {
#ifdef SOGEN_HAS_SDL3
        return create_sdl_audio_backend();
#else
        return std::make_unique<null_audio_backend>();
#endif
    }
}

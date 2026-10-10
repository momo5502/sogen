#include "../std_include.hpp"
#include <platform/console_backend.hpp>

namespace sogen
{
    std::unique_ptr<console_backend> create_default_console_backend()
    {
#ifdef OS_WINDOWS
        return create_stream_console_backend();
#elif defined(OS_EMSCRIPTEN)
        return std::make_unique<null_console_backend>();
#else
        return create_posix_console_backend();
#endif
    }

} // namespace sogen

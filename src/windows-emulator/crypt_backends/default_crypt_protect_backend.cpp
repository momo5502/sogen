#include "../std_include.hpp"
#include <platform/crypt_protect_backend.hpp>

namespace sogen
{
    std::unique_ptr<crypt_protect_backend> create_default_crypt_protect_backend(const std::filesystem::path& emulation_root)
    {
        return create_windows_dpapi_backend(emulation_root);
    }
}

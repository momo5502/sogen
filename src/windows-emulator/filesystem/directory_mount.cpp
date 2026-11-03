#include "directory_mount.hpp"
#include "host_directory_provider.hpp"
#include "layered_provider.hpp"

namespace sogen
{
    std::unique_ptr<mount_provider> create_directory_mount(const directory_mount& mount)
    {
        return std::make_unique<layered_provider>(std::make_unique<host_directory_provider>(mount.host_directory, host_links::refuse),
                                                  mount.upper_directory, mount.state_directory);
    }

} // namespace sogen

#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace sogen
{

    namespace utils::exec
    {
        class external_command_exit_error : public std::runtime_error
        {
          public:
            using std::runtime_error::runtime_error;
        };

        std::string run_command_capture(const std::vector<std::string>& args);
    }

} // namespace sogen

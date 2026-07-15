#pragma once

#include <filesystem>
#include <memory>

namespace sogen
{

    class analysis_reporter;

    std::unique_ptr<analysis_reporter> create_stdout_file_reporter(const std::filesystem::path& path);

} // namespace sogen

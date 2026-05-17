#pragma once

#include <filesystem>
#include <memory>

class analysis_reporter;

std::unique_ptr<analysis_reporter> create_jsonl_reporter(const std::filesystem::path& path);

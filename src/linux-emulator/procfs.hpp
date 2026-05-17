#pragma once

#include "std_include.hpp"
#include "linux_stat.hpp"

#include <cstdio>
#include <optional>

// Forward declarations
class linux_emulator;

// Synthesizes /proc filesystem content from emulator state.
// All generated content is returned as strings or byte vectors.
// The caller (syscall handlers) is responsible for creating fds
// backed by tmpfile() with the generated content.
class procfs
{
  public:
    procfs() = default;

    // Check if a guest path is a procfs path we should intercept.
    // Covers /proc/self/*, /proc/<pid>/*, and /proc/sys/*.
    static bool is_procfs_path(std::string_view path);

    // Check if a guest path is a procfs symlink (readlink target).
    // e.g., /proc/self/exe, /proc/self/fd/N
    static bool is_procfs_symlink(std::string_view path);

    // Generate content for a readable /proc file.
    // Returns the content as a string, or std::nullopt if the path is not recognized.
    std::optional<std::string> generate_content(const linux_emulator& emu, std::string_view path) const;

    // Resolve a procfs symlink target (e.g., /proc/self/exe -> /path/to/binary).
    // Returns the target string, or std::nullopt if not a symlink.
    std::optional<std::string> resolve_symlink(const linux_emulator& emu, std::string_view path) const;

    // Create a FILE* handle backed by tmpfile() containing the generated content.
    // The caller takes ownership of the returned handle.
    // Returns nullptr if the path is not recognized.
    FILE* open_procfs_file(const linux_emulator& emu, std::string_view path) const;

    // Fill a synthetic stat buffer for a procfs path.
    // Returns true if the path was recognized, false otherwise.
    bool stat_procfs(const linux_emulator& emu, std::string_view path, linux_stat& st) const;

  private:
    // Individual generators
    std::string generate_maps(const linux_emulator& emu) const;
    std::string generate_cmdline(const linux_emulator& emu) const;
    std::string generate_environ(const linux_emulator& emu) const;
    std::string generate_status(const linux_emulator& emu) const;
    std::string generate_auxv(const linux_emulator& emu) const;
    std::string generate_proc_stat(const linux_emulator& emu) const;
    std::string generate_osrelease() const;
};

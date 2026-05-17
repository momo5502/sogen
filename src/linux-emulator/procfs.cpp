#include "std_include.hpp"
#include "procfs.hpp"
#include "linux_emulator.hpp"

#include <cstring>
#include <cinttypes>
#include <ctime>

bool procfs::is_procfs_path(const std::string_view path)
{
    return path.starts_with("/proc/self/") || path == "/proc/self" || path.starts_with("/proc/sys/") ||
           path.starts_with("/proc/thread-self/");
}

bool procfs::is_procfs_symlink(const std::string_view path)
{
    if (path == "/proc/self/exe")
    {
        return true;
    }

    if (path == "/proc/self/cwd")
    {
        return true;
    }

    // /proc/self/fd/N
    if (path.starts_with("/proc/self/fd/") && path.size() > 14)
    {
        return true;
    }

    return false;
}

std::optional<std::string> procfs::generate_content(const linux_emulator& emu, const std::string_view path) const
{
    // Normalize: /proc/<pid>/ is treated as /proc/self/ if pid matches
    std::string_view effective_path = path;

    if (effective_path == "/proc/self/maps")
    {
        return this->generate_maps(emu);
    }
    if (effective_path == "/proc/self/cmdline")
    {
        return this->generate_cmdline(emu);
    }
    if (effective_path == "/proc/self/environ")
    {
        return this->generate_environ(emu);
    }
    if (effective_path == "/proc/self/status")
    {
        return this->generate_status(emu);
    }
    if (effective_path == "/proc/self/auxv")
    {
        return this->generate_auxv(emu);
    }
    if (effective_path == "/proc/self/stat")
    {
        return this->generate_proc_stat(emu);
    }
    if (effective_path == "/proc/sys/kernel/osrelease")
    {
        return this->generate_osrelease();
    }

    return std::nullopt;
}

std::optional<std::string> procfs::resolve_symlink(const linux_emulator& emu, const std::string_view path) const
{
    if (path == "/proc/self/exe")
    {
        return emu.mod_manager.get_executable_path().string();
    }

    if (path == "/proc/self/cwd")
    {
        // Return emulation root or "/"
        if (!emu.emulation_root.empty())
        {
            return emu.emulation_root.string();
        }
        return "/";
    }

    // /proc/self/fd/N
    if (path.starts_with("/proc/self/fd/"))
    {
        const auto fd_str = path.substr(14);
        char* end = nullptr;
        const auto fd_num = static_cast<int>(strtol(std::string(fd_str).c_str(), &end, 10));
        if (end && *end == '\0')
        {
            const auto& fds = emu.process.fds.get_fds();
            auto it = fds.find(fd_num);
            if (it != fds.end())
            {
                if (!it->second.host_path.empty())
                {
                    return it->second.host_path;
                }

                // Return a synthetic name for special fds
                switch (it->second.type)
                {
                case fd_type::pipe_read:
                case fd_type::pipe_write:
                    return "pipe:[" + std::to_string(fd_num) + "]";
                case fd_type::socket:
                    return "socket:[" + std::to_string(fd_num) + "]";
                case fd_type::eventfd:
                    return "anon_inode:[eventfd]";
                default:
                    return "/dev/fd/" + std::to_string(fd_num);
                }
            }
        }
    }

    return std::nullopt;
}

FILE* procfs::open_procfs_file(const linux_emulator& emu, const std::string_view path) const
{
    auto content = this->generate_content(emu, path);
    if (!content)
    {
        return nullptr;
    }

    FILE* fp = tmpfile();
    if (!fp)
    {
        return nullptr;
    }

    if (!content->empty())
    {
        fwrite(content->data(), 1, content->size(), fp);
    }

    // Seek back to beginning so reads start from the top
    fseek(fp, 0, SEEK_SET);
    return fp;
}

bool procfs::stat_procfs(const linux_emulator& emu, const std::string_view path, linux_stat& st) const
{
    memset(&st, 0, sizeof(st));

    // Symlinks
    if (is_procfs_symlink(path))
    {
        st.st_mode = 0120777; // S_IFLNK | 0777
        st.st_nlink = 1;
        st.st_uid = emu.process.uid;
        st.st_gid = emu.process.gid;
        st.st_size = 0;
        st.st_blksize = 1024;
        st.st_ino = static_cast<uint64_t>(std::hash<std::string_view>{}(path));
        return true;
    }

    // Directories
    if (path == "/proc/self" || path == "/proc/self/fd" || path == "/proc/self/task")
    {
        st.st_mode = 040555; // S_IFDIR | 0555
        st.st_nlink = 2;
        st.st_uid = emu.process.uid;
        st.st_gid = emu.process.gid;
        st.st_size = 0;
        st.st_blksize = 1024;
        st.st_ino = static_cast<uint64_t>(std::hash<std::string_view>{}(path));
        return true;
    }

    // Regular files (readable)
    auto content = this->generate_content(emu, path);
    if (content)
    {
        st.st_mode = 0100444; // S_IFREG | 0444
        st.st_nlink = 1;
        st.st_uid = emu.process.uid;
        st.st_gid = emu.process.gid;
        st.st_size = static_cast<int64_t>(content->size());
        st.st_blksize = 1024;
        st.st_blocks = static_cast<int64_t>((content->size() + 511) / 512);
        st.st_ino = static_cast<uint64_t>(std::hash<std::string_view>{}(path));
        return true;
    }

    return false;
}

// ---- Content generators ----

std::string procfs::generate_maps(const linux_emulator& emu) const
{
    // Format: start-end perms offset dev inode pathname
    std::string result;

    for (const auto& [addr, region] : emu.memory.get_mapped_regions())
    {
        const auto end = addr + region.length;

        // Decode permissions
        char perms[5] = "----";
        if ((region.permissions & memory_permission::read) != memory_permission::none)
        {
            perms[0] = 'r';
        }
        if ((region.permissions & memory_permission::write) != memory_permission::none)
        {
            perms[1] = 'w';
        }
        if ((region.permissions & memory_permission::exec) != memory_permission::none)
        {
            perms[2] = 'x';
        }
        perms[3] = 'p'; // private mapping

        // Try to find a module at this address
        std::string pathname;
        for (const auto& [mod_base, mod] : emu.mod_manager.get_modules())
        {
            if (addr >= mod.image_base && addr < mod.image_base + mod.size_of_image)
            {
                pathname = mod.path.string();
                break;
            }
        }

        // Special regions
        if (pathname.empty())
        {
            if (addr >= 0x7fffff7de000ULL && addr <= 0x7ffffffde000ULL)
            {
                pathname = "[stack]";
            }
            else if (addr >= emu.process.brk_base && addr < emu.process.brk_current)
            {
                pathname = "[heap]";
            }
            else if (emu.vdso.get_base() != 0 && addr >= emu.vdso.get_base() && addr < emu.vdso.get_base() + emu.vdso.get_size())
            {
                pathname = "[vdso]";
            }
        }

        char line[256];
        snprintf(line, sizeof(line), "%012" PRIx64 "-%012" PRIx64 " %s %08x %02x:%02x %-10u %s\n", addr, end, perms,
                 0,    // offset
                 0, 0, // dev major:minor
                 0,    // inode
                 pathname.c_str());

        result += line;
    }

    return result;
}

std::string procfs::generate_cmdline(const linux_emulator& emu) const
{
    // /proc/self/cmdline: null-separated argv
    std::string result;
    for (const auto& arg : emu.process.argv)
    {
        result += arg;
        result += '\0';
    }
    return result;
}

std::string procfs::generate_environ(const linux_emulator& emu) const
{
    // /proc/self/environ: null-separated envp
    std::string result;
    for (const auto& env : emu.process.envp)
    {
        result += env;
        result += '\0';
    }
    return result;
}

std::string procfs::generate_status(const linux_emulator& emu) const
{
    // /proc/self/status: key-value pairs
    std::string result;

    // Name
    const auto& exe_path = emu.mod_manager.get_executable_path();
    auto name = exe_path.filename().string();
    if (name.size() > 15)
    {
        name = name.substr(0, 15);
    }

    result += "Name:\t" + name + "\n";
    result += "Umask:\t0022\n";
    result += "State:\tR (running)\n";
    result += "Tgid:\t" + std::to_string(emu.process.pid) + "\n";
    result += "Ngid:\t0\n";
    result += "Pid:\t" + std::to_string(emu.process.pid) + "\n";
    result += "PPid:\t" + std::to_string(emu.process.ppid) + "\n";
    result += "TracerPid:\t0\n";
    result += "Uid:\t" + std::to_string(emu.process.uid) + "\t" + std::to_string(emu.process.euid) + "\t" +
              std::to_string(emu.process.uid) + "\t" + std::to_string(emu.process.uid) + "\n";
    result += "Gid:\t" + std::to_string(emu.process.gid) + "\t" + std::to_string(emu.process.egid) + "\t" +
              std::to_string(emu.process.gid) + "\t" + std::to_string(emu.process.gid) + "\n";
    result += "FDSize:\t64\n";
    result += "Groups:\t" + std::to_string(emu.process.gid) + "\n";

    // VmSize: estimate from mapped regions
    uint64_t vm_size = 0;
    uint64_t vm_rss = 0;
    for (const auto& [addr, region] : emu.memory.get_mapped_regions())
    {
        vm_size += region.length;
        vm_rss += region.length; // All mapped memory is "resident" in emulation
    }

    result += "VmPeak:\t" + std::to_string(vm_size / 1024) + " kB\n";
    result += "VmSize:\t" + std::to_string(vm_size / 1024) + " kB\n";
    result += "VmLck:\t0 kB\n";
    result += "VmPin:\t0 kB\n";
    result += "VmHWM:\t" + std::to_string(vm_rss / 1024) + " kB\n";
    result += "VmRSS:\t" + std::to_string(vm_rss / 1024) + " kB\n";

    result += "Threads:\t" + std::to_string(emu.process.threads.size()) + "\n";
    result += "SigQ:\t0/0\n";
    result += "SigPnd:\t0000000000000000\n";
    result += "ShdPnd:\t0000000000000000\n";

    // Signal mask from active thread
    uint64_t sig_mask = 0;
    if (emu.process.active_thread)
    {
        sig_mask = emu.process.active_thread->signal_mask;
    }

    char mask_buf[32];
    snprintf(mask_buf, sizeof(mask_buf), "%016" PRIx64, sig_mask);
    result += "SigBlk:\t";
    result += mask_buf;
    result += "\n";
    result += "SigIgn:\t0000000000000000\n";
    result += "SigCgt:\t0000000000000000\n";
    result += "CapInh:\t0000000000000000\n";
    result += "CapPrm:\t0000000000000000\n";
    result += "CapEff:\t0000000000000000\n";
    result += "CapBnd:\t000001ffffffffff\n";
    result += "CapAmb:\t0000000000000000\n";
    result += "Seccomp:\t0\n";

    return result;
}

std::string procfs::generate_auxv(const linux_emulator& emu) const
{
    // /proc/self/auxv: raw binary auxiliary vector
    // For now, return an empty auxv (just AT_NULL terminator)
    // A real implementation would read the auxv from the initial stack.
    (void)emu;
    uint64_t null_entry[2] = {0, 0};
    return std::string(reinterpret_cast<const char*>(null_entry), sizeof(null_entry));
}

std::string procfs::generate_proc_stat(const linux_emulator& emu) const
{
    // /proc/self/stat: single line of process statistics
    // Format: pid (comm) state ppid pgrp session tty_nr tpgid flags ...
    const auto& exe_path = emu.mod_manager.get_executable_path();
    auto comm = exe_path.filename().string();

    char buf[512];
    snprintf(buf, sizeof(buf),
             "%u (%s) R %u %u %u 0 -1 0 0 0 0 0 0 0 0 0 0 %zu 0 0 %" PRIu64 " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
             emu.process.pid,                  // pid
             comm.c_str(),                     // comm
             emu.process.ppid,                 // ppid
             emu.process.pid,                  // pgrp
             emu.process.pid,                  // session
             emu.process.threads.size(),       // num_threads
             emu.get_executed_instructions()); // vsize (repurpose)

    return buf;
}

std::string procfs::generate_osrelease() const
{
    return "5.15.0-sogen\n";
}

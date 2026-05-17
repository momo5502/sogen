#pragma once

#include "std_include.hpp"
#include "linux_syscall_utils.hpp"
#include "linux_syscall_numbers.hpp"

// Linux errno constants (with LINUX_ prefix to avoid clash with system <errno.h> macros)
namespace linux_errno
{
    constexpr int64_t LINUX_EPERM = 1;
    constexpr int64_t LINUX_ENOENT = 2;
    constexpr int64_t LINUX_ESRCH = 3;
    constexpr int64_t LINUX_EINTR = 4;
    constexpr int64_t LINUX_EIO = 5;
    constexpr int64_t LINUX_ENXIO = 6;
    constexpr int64_t LINUX_E2BIG = 7;
    constexpr int64_t LINUX_ENOEXEC = 8;
    constexpr int64_t LINUX_EBADF = 9;
    constexpr int64_t LINUX_ECHILD = 10;
    constexpr int64_t LINUX_EAGAIN = 11;
    constexpr int64_t LINUX_ENOMEM = 12;
    constexpr int64_t LINUX_EACCES = 13;
    constexpr int64_t LINUX_EFAULT = 14;
    constexpr int64_t LINUX_ENOTBLK = 15;
    constexpr int64_t LINUX_EBUSY = 16;
    constexpr int64_t LINUX_EEXIST = 17;
    constexpr int64_t LINUX_EXDEV = 18;
    constexpr int64_t LINUX_ENODEV = 19;
    constexpr int64_t LINUX_ENOTDIR = 20;
    constexpr int64_t LINUX_EISDIR = 21;
    constexpr int64_t LINUX_EINVAL = 22;
    constexpr int64_t LINUX_ENFILE = 23;
    constexpr int64_t LINUX_EMFILE = 24;
    constexpr int64_t LINUX_ENOTTY = 25;
    constexpr int64_t LINUX_ETXTBSY = 26;
    constexpr int64_t LINUX_EFBIG = 27;
    constexpr int64_t LINUX_ENOSPC = 28;
    constexpr int64_t LINUX_ESPIPE = 29;
    constexpr int64_t LINUX_EROFS = 30;
    constexpr int64_t LINUX_EMLINK = 31;
    constexpr int64_t LINUX_EPIPE = 32;
    constexpr int64_t LINUX_EDOM = 33;
    constexpr int64_t LINUX_ERANGE = 34;
    constexpr int64_t LINUX_ENOSYS = 38;
    constexpr int64_t LINUX_ENOTEMPTY = 39;
    constexpr int64_t LINUX_EWOULDBLOCK = LINUX_EAGAIN;
    constexpr int64_t LINUX_EDEADLK = 35;
    constexpr int64_t LINUX_ENAMETOOLONG = 36;
    constexpr int64_t LINUX_ENOLCK = 37;
    constexpr int64_t LINUX_ENOTSOCK = 88;
    constexpr int64_t LINUX_EDESTADDRREQ = 89;
    constexpr int64_t LINUX_EMSGSIZE = 90;
    constexpr int64_t LINUX_EPROTOTYPE = 91;
    constexpr int64_t LINUX_ENOPROTOOPT = 92;
    constexpr int64_t LINUX_EPROTONOSUPPORT = 93;
    constexpr int64_t LINUX_ESOCKTNOSUPPORT = 94;
    constexpr int64_t LINUX_EOPNOTSUPP = 95;
    constexpr int64_t LINUX_EPFNOSUPPORT = 96;
    constexpr int64_t LINUX_EAFNOSUPPORT = 97;
    constexpr int64_t LINUX_EADDRINUSE = 98;
    constexpr int64_t LINUX_EADDRNOTAVAIL = 99;
    constexpr int64_t LINUX_ENETDOWN = 100;
    constexpr int64_t LINUX_ENETUNREACH = 101;
    constexpr int64_t LINUX_ENETRESET = 102;
    constexpr int64_t LINUX_ECONNABORTED = 103;
    constexpr int64_t LINUX_ECONNRESET = 104;
    constexpr int64_t LINUX_ENOBUFS = 105;
    constexpr int64_t LINUX_EISCONN = 106;
    constexpr int64_t LINUX_ENOTCONN = 107;
    constexpr int64_t LINUX_ESHUTDOWN = 108;
    constexpr int64_t LINUX_ETOOMANYREFS = 109;
    constexpr int64_t LINUX_ETIMEDOUT = 110;
    constexpr int64_t LINUX_ECONNREFUSED = 111;
    constexpr int64_t LINUX_EHOSTDOWN = 112;
    constexpr int64_t LINUX_EHOSTUNREACH = 113;
    constexpr int64_t LINUX_EALREADY = 114;
    constexpr int64_t LINUX_EINPROGRESS = 115;
}

struct linux_syscall_handler_entry
{
    linux_syscall_handler handler{};
    std::string name{};
};

class linux_emulator;

class linux_syscall_dispatcher
{
  public:
    linux_syscall_dispatcher() = default;

    void dispatch(linux_emulator& emu);
    void add_handlers();

    const linux_syscall_handler_entry* get_entry(uint64_t id) const
    {
        if (id >= this->handlers_.size())
        {
            return nullptr;
        }

        return &this->handlers_[id];
    }

  private:
    std::array<linux_syscall_handler_entry, 512> handlers_{};
};

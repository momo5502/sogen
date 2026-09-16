#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Two traps when reaching the original mach_msg from an interposer on this build: dlsym
   (RTLD_NEXT or handle-scoped) honors the interpose and returns this replacer (recursion
   until the guard page), and calling mach_msg_trap() directly is EXC_GUARD-killed outside
   libsystem_kernel. mach_msg_overwrite(..., MACH_MSG_NULL, 0) is the same syscall with an
   unused scatter-list tail, and its binding is not interposed. */
static int out_fd = STDERR_FILENO;
static os_unfair_lock out_lock = OS_UNFAIR_LOCK_INIT;
static unsigned long seq;

__attribute__((constructor)) static void wstrace_init(void)
{
    const char *path = getenv("WSTRACE_OUT");
    if (path)
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
            out_fd = fd;
    }
}

mach_msg_return_t wstrace_mach_msg(mach_msg_header_t *msg, mach_msg_option_t option,
                                   mach_msg_size_t send_size, mach_msg_size_t rcv_limit,
                                   mach_port_t rcv_name, mach_msg_timeout_t timeout, mach_port_t notify)
{
    void *caller = __builtin_return_address(0);
    const int send = (option & MACH_SEND_MSG) && msg;

    /* capture the request header before the call: a send|receive overwrites the buffer
       with the reply, whose msgh_id is request+100 (MIG reply convention) */
    mach_msg_id_t id = 0;
    mach_msg_size_t size = 0;
    mach_msg_bits_t bits = 0;
    mach_port_t rport = 0;
    if (send)
    {
        id = msg->msgh_id;
        size = msg->msgh_size;
        bits = msg->msgh_bits;
        rport = msg->msgh_remote_port;
    }

    /* one frame hop outward: when the immediate mach_msg caller is an unexported helper
       or block, the outer caller is usually the export that carried the message */
    void *outer = NULL;
    void **frame = __builtin_frame_address(0);
    if (frame)
    {
        void **caller_frame = frame[0];
        if (caller_frame > frame && (char *)caller_frame - (char *)frame < 1 << 20)
            outer = caller_frame[1];
    }

    Dl_info info, outer_info;
    memset(&info, 0, sizeof(info));
    memset(&outer_info, 0, sizeof(outer_info));
    if (send)
    {
        dladdr(caller, &info);
        if (outer)
            dladdr(outer, &outer_info);
    }

    mach_msg_return_t ret =
        mach_msg_overwrite(msg, option, send_size, rcv_limit, rcv_name, timeout, notify, MACH_MSG_NULL, 0);
    if (send)
    {
        const char *image = "";
        if (info.dli_fname)
        {
            const char *slash = strrchr(info.dli_fname, '/');
            image = slash ? slash + 1 : info.dli_fname;
        }
        char line[512];
        os_unfair_lock_lock(&out_lock);
        int n = snprintf(line, sizeof(line),
                         "%lu id=%d size=%u snd=%u rcv=%u bits=0x%x rport=0x%x ret=%d thread=0x%x "
                         "caller=%s+%#lx outer=%s+%#lx image=%s\n",
                         seq++, id, size, send_size, rcv_limit, bits, rport, ret,
                         pthread_mach_thread_np(pthread_self()),
                         info.dli_sname ? info.dli_sname : "?", info.dli_saddr ? caller - info.dli_saddr : 0,
                         outer_info.dli_sname ? outer_info.dli_sname : "?",
                         outer_info.dli_saddr ? outer - outer_info.dli_saddr : 0, image);
        if (n > 0)
            (void)!write(out_fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        os_unfair_lock_unlock(&out_lock);
    }
    return ret;
}

typedef struct
{
    const void *replacer;
    const void *replacee;
} interpose_t;

__attribute__((used)) static const interpose_t interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)wstrace_mach_msg, (const void *)mach_msg},
};

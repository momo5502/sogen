#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "native_arm64_single_step.hpp"

#include <asm/ptrace.h>
#include <linux/elf.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utils/finally.hpp>

#include <cerrno>
#include <cstring>
#include <system_error>

namespace sogen::native_arm64
{
    namespace
    {
        struct step_response
        {
            int error{};
            single_step_result result{};
            register_state registers{};
        };

        template <typename T>
        bool send_record(const int channel, const T& record)
        {
            ssize_t size{};
            do
            {
                size = send(channel, &record, sizeof(record), MSG_NOSIGNAL);
            } while (size < 0 && errno == EINTR);
            return size == static_cast<ssize_t>(sizeof(record));
        }

        template <typename T>
        bool receive_record(const int channel, T& record)
        {
            ssize_t size{};
            do
            {
                size = recv(channel, &record, sizeof(record), 0);
            } while (size < 0 && errno == EINTR);
            if (size == static_cast<ssize_t>(sizeof(record)))
            {
                return true;
            }
            if (size >= 0)
            {
                errno = EPIPE;
            }
            return false;
        }

        pid_t wait_for_process(const pid_t process, int& status)
        {
            pid_t result{};
            do
            {
                result = waitpid(process, &status, __WALL);
            } while (result < 0 && errno == EINTR);
            return result;
        }

        bool wait_for_stop(const pid_t thread, int& signal)
        {
            int status{};
            if (wait_for_process(thread, status) != thread || !WIFSTOPPED(status))
            {
                errno = ESRCH;
                return false;
            }
            signal = WSTOPSIG(status);
            return true;
        }

        template <typename T>
        bool get_registers(const pid_t thread, const unsigned int note, T& registers)
        {
            iovec buffer{&registers, sizeof(registers)};
            if (ptrace(PTRACE_GETREGSET, thread, reinterpret_cast<void*>(static_cast<uintptr_t>(note)), &buffer) < 0)
            {
                return false;
            }
            if (buffer.iov_len != sizeof(registers))
            {
                errno = EIO;
                return false;
            }
            return true;
        }

        template <typename T>
        bool set_registers(const pid_t thread, const unsigned int note, T& registers)
        {
            iovec buffer{&registers, sizeof(registers)};
            return ptrace(PTRACE_SETREGSET, thread, reinterpret_cast<void*>(static_cast<uintptr_t>(note)), &buffer) == 0;
        }

        [[noreturn]] void terminate_trace(const pid_t process)
        {
            constexpr char message[] = "Native ARM64: failed to restore traced host state\n";
            write(STDERR_FILENO, message, sizeof(message) - 1);
            kill(process, SIGKILL);
            _exit(1);
        }

        step_response execute_step(const pid_t process, const pid_t thread, const int entry_signal, const register_state& guest)
        {
            step_response response{};
            int signal{};
            for (;;)
            {
                if (!wait_for_stop(thread, signal))
                {
                    terminate_trace(process);
                }
                if (signal == entry_signal)
                {
                    break;
                }
                if (ptrace(PTRACE_CONT, thread, nullptr, reinterpret_cast<void*>(static_cast<uintptr_t>(signal))) < 0)
                {
                    terminate_trace(process);
                }
            }

            user_pt_regs host_registers{};
            user_fpsimd_state host_simd{};
            uint64_t host_tls{};
            const bool saved = get_registers(thread, NT_PRSTATUS, host_registers) && get_registers(thread, NT_PRFPREG, host_simd) &&
                               get_registers(thread, NT_ARM_TLS, host_tls);
            if (!saved)
            {
                response.error = errno;
            }
            else
            {
                user_pt_regs registers{};
                std::memcpy(registers.regs, guest.x.data(), sizeof(registers.regs));
                registers.sp = guest.sp;
                registers.pc = guest.pc;
                registers.pstate = guest.pstate & pstate_nzcv;
                user_fpsimd_state simd{};
                std::memcpy(simd.vregs, guest.vectors.data(), sizeof(simd.vregs));
                simd.fpcr = guest.fpcr;
                simd.fpsr = guest.fpsr;
                if (!set_registers(thread, NT_PRSTATUS, registers) || !set_registers(thread, NT_PRFPREG, simd) ||
                    ptrace(PTRACE_SINGLESTEP, thread, nullptr, nullptr) < 0)
                {
                    response.error = errno;
                }
                else
                {
                    if (!wait_for_stop(thread, response.result.signal))
                    {
                        terminate_trace(process);
                    }
                    if (ptrace(PTRACE_GETSIGINFO, thread, nullptr, &response.result.info) < 0 ||
                        !get_registers(thread, NT_PRSTATUS, registers) || !get_registers(thread, NT_PRFPREG, simd))
                    {
                        response.error = errno;
                    }
                    else
                    {
                        std::memcpy(response.registers.x.data(), registers.regs, sizeof(registers.regs));
                        response.registers.sp = registers.sp;
                        response.registers.pc = registers.pc;
                        response.registers.pstate = registers.pstate & pstate_nzcv;
                        std::memcpy(response.registers.vectors.data(), simd.vregs, sizeof(simd.vregs));
                        response.registers.fpcr = simd.fpcr;
                        response.registers.fpsr = simd.fpsr;
                    }
                }
                if (!set_registers(thread, NT_PRFPREG, host_simd) || !set_registers(thread, NT_ARM_TLS, host_tls) ||
                    !set_registers(thread, NT_PRSTATUS, host_registers))
                {
                    terminate_trace(process);
                }
            }
            if (ptrace(PTRACE_DETACH, thread, nullptr, nullptr) < 0)
            {
                terminate_trace(process);
            }
            return response;
        }
    }

    single_step_result single_step_native(register_state& registers, const int entry_signal)
    {
        int channels[2]{};
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "Create native single-step channel");
        }
        const auto close_channels = utils::finally([&] {
            close(channels[0]);
            if (channels[1] >= 0)
            {
                close(channels[1]);
            }
        });
        const auto process = getpid();
        const auto thread = static_cast<pid_t>(syscall(SYS_gettid));
        const auto child = fork();
        if (child < 0)
        {
            throw std::system_error(errno, std::generic_category(), "Create native single-step tracer");
        }
        if (child == 0)
        {
            close(channels[0]);
            int error{};
            if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != process ||
                ptrace(PTRACE_SEIZE, thread, nullptr, reinterpret_cast<void*>(static_cast<uintptr_t>(PTRACE_O_EXITKILL))) < 0)
            {
                error = errno != 0 ? errno : ESRCH;
            }
            if (!send_record(channels[1], error))
            {
                _exit(1);
            }
            if (error == 0)
            {
                const auto response = execute_step(process, thread, entry_signal, registers);
                if (!send_record(channels[1], response))
                {
                    _exit(1);
                }
            }
            _exit(0);
        }

        close(channels[1]);
        channels[1] = -1;
        const auto reap_child = utils::finally([&] {
            int status{};
            wait_for_process(child, status);
        });
        int error{};
        if (!receive_record(channels[0], error))
        {
            throw std::system_error(errno, std::generic_category(), "Receive native single-step readiness");
        }
        if (error != 0)
        {
            throw std::system_error(error, std::generic_category(), "Attach native single-step tracer");
        }
        if (syscall(SYS_tgkill, process, thread, entry_signal) < 0)
        {
            terminate_trace(process);
        }
        step_response response{};
        if (!receive_record(channels[0], response))
        {
            throw std::system_error(errno, std::generic_category(), "Receive native single-step state");
        }
        if (response.error != 0)
        {
            throw std::system_error(response.error, std::generic_category(), "Execute native single step");
        }
        registers = response.registers;
        return response.result;
    }
}

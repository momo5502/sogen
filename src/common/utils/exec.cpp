#include "exec.hpp"

#include <array>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#include <spawn.h>
#endif
#ifndef __EMSCRIPTEN__
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

namespace sogen
{

    namespace utils::exec
    {
        namespace
        {
#ifdef _WIN32
            std::string quote_windows_process_arg(const std::string_view arg)
            {
                std::string quoted{};
                quoted.push_back('"');

                size_t backslashes = 0;
                for (const auto ch : arg)
                {
                    if (ch == '\\')
                    {
                        ++backslashes;
                        continue;
                    }

                    if (ch == '"')
                    {
                        quoted.append(backslashes * 2 + 1, '\\');
                        quoted.push_back(ch);
                        backslashes = 0;
                        continue;
                    }

                    quoted.append(backslashes, '\\');
                    backslashes = 0;
                    quoted.push_back(ch);
                }

                quoted.append(backslashes * 2, '\\');
                quoted.push_back('"');
                return quoted;
            }
#endif
        }

        std::string run_command_capture(const std::vector<std::string>& args)
        {
            if (args.empty())
            {
                return {};
            }

#ifdef __EMSCRIPTEN__
            throw std::runtime_error("External commands are unavailable in WebAssembly: " + args.front());
#elif defined(_WIN32)
            SECURITY_ATTRIBUTES security_attributes{};
            security_attributes.nLength = sizeof(security_attributes);
            security_attributes.bInheritHandle = TRUE;

            HANDLE read_pipe{};
            HANDLE write_pipe{};
            if (!CreatePipe(&read_pipe, &write_pipe, &security_attributes, 0))
            {
                throw std::runtime_error("Failed to create output pipe for external command: " + args.front());
            }

            if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0))
            {
                CloseHandle(read_pipe);
                CloseHandle(write_pipe);
                throw std::runtime_error("Failed to configure output pipe for external command: " + args.front());
            }

            std::string command_line{};
            for (const auto& arg : args)
            {
                if (!command_line.empty())
                {
                    command_line.push_back(' ');
                }
                command_line += quote_windows_process_arg(arg);
            }

            STARTUPINFOA startup_info{};
            startup_info.cb = sizeof(startup_info);
            startup_info.dwFlags = STARTF_USESTDHANDLES;
            startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup_info.hStdOutput = write_pipe;
            startup_info.hStdError = write_pipe;

            PROCESS_INFORMATION process_info{};
            const auto created = CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                                &startup_info, &process_info);
            CloseHandle(write_pipe);

            if (!created)
            {
                CloseHandle(read_pipe);
                throw std::runtime_error("Failed to execute external command: " + args.front());
            }

            std::string output{};
            std::array<char, 4096> buffer{};
            DWORD bytes_read{};
            while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) && bytes_read != 0)
            {
                output.append(buffer.data(), bytes_read);
            }

            CloseHandle(read_pipe);
            WaitForSingleObject(process_info.hProcess, INFINITE);

            DWORD exit_code{};
            const auto got_exit_code = GetExitCodeProcess(process_info.hProcess, &exit_code);
            CloseHandle(process_info.hThread);
            CloseHandle(process_info.hProcess);

            if (!got_exit_code)
            {
                throw std::runtime_error("Failed to retrieve external command status: " + args.front());
            }

            if (exit_code != 0)
            {
                throw external_command_exit_error("External command failed: " + args.front() + "\n" + output);
            }

            return output;
#else
            std::vector<char*> argv{};
            argv.reserve(args.size() + 1);
            for (const auto& arg : args)
            {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            std::array<int, 2> output_pipe{};
            if (pipe(output_pipe.data()) != 0)
            {
                throw std::runtime_error("Failed to create output pipe for external command: " + args.front());
            }

            pid_t pid{};
#ifdef __ANDROID__
            pid = fork();
            if (pid < 0)
            {
                close(output_pipe[0]);
                close(output_pipe[1]);
                throw std::runtime_error("Failed to execute external command: " + args.front());
            }

            if (pid == 0)
            {
                close(output_pipe[0]);
                if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(output_pipe[1], STDERR_FILENO) < 0)
                {
                    _exit(127);
                }
                close(output_pipe[1]);
                execvp(argv.front(), argv.data());
                _exit(127);
            }
#else
            posix_spawn_file_actions_t file_actions{};
            if (posix_spawn_file_actions_init(&file_actions) != 0)
            {
                close(output_pipe[0]);
                close(output_pipe[1]);
                throw std::runtime_error("Failed to configure external command: " + args.front());
            }

            auto spawn_error = posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDOUT_FILENO);
            if (spawn_error == 0)
            {
                spawn_error = posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDERR_FILENO);
            }
            if (spawn_error == 0 && output_pipe[0] != STDOUT_FILENO && output_pipe[0] != STDERR_FILENO)
            {
                spawn_error = posix_spawn_file_actions_addclose(&file_actions, output_pipe[0]);
            }
            if (spawn_error == 0 && output_pipe[1] != STDOUT_FILENO && output_pipe[1] != STDERR_FILENO)
            {
                spawn_error = posix_spawn_file_actions_addclose(&file_actions, output_pipe[1]);
            }

            if (spawn_error != 0)
            {
                posix_spawn_file_actions_destroy(&file_actions);
                close(output_pipe[0]);
                close(output_pipe[1]);
                throw std::runtime_error("Failed to configure external command: " + args.front());
            }

            spawn_error = posix_spawnp(&pid, argv.front(), &file_actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&file_actions);

            if (spawn_error != 0)
            {
                close(output_pipe[1]);
                close(output_pipe[0]);
                throw std::runtime_error("Failed to execute external command: " + args.front());
            }
#endif

            close(output_pipe[1]);

            std::string output{};
            std::array<char, 4096> buffer{};
            while (true)
            {
                const auto count = read(output_pipe[0], buffer.data(), buffer.size());
                if (count > 0)
                {
                    output.append(buffer.data(), static_cast<size_t>(count));
                    continue;
                }
                if (count == 0)
                {
                    break;
                }
                if (errno == EINTR)
                {
                    continue;
                }

                close(output_pipe[0]);
                int ignored_status{};
                while (waitpid(pid, &ignored_status, 0) < 0 && errno == EINTR)
                {
                }
                throw std::runtime_error("Failed to read external command output: " + args.front());
            }

            close(output_pipe[0]);

            int status{};
            while (waitpid(pid, &status, 0) < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                throw std::runtime_error("Failed to retrieve external command status: " + args.front());
            }

            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                throw external_command_exit_error("External command failed: " + args.front() + "\n" + output);
            }

            return output;
#endif
        }
    }

} // namespace sogen

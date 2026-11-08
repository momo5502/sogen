#include "host_process.hpp"

#ifdef poll
#undef poll
#endif

#include <reproc++/reproc.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <tuple>

namespace sogen
{
    namespace
    {
        process_error map_process_error(const std::error_code& error)
        {
            if (error == std::errc::permission_denied)
            {
                return process_error::permission_denied;
            }
            if (error == std::errc::no_such_file_or_directory)
            {
                return process_error::unavailable;
            }
            if (error == std::errc::not_enough_memory || error == std::errc::resource_unavailable_try_again)
            {
                return process_error::resource_limit;
            }
            return process_error::internal_failure;
        }

        class subprocess final : public host_process
        {
          public:
            subprocess(reproc::process process, const uint32_t id)
                : process_(std::move(process)),
                  id_(id)
            {
            }

            uint32_t id() const override
            {
                return this->id_;
            }

            // reproc::process is not safe to use from two threads, so the thread in run() does the killing.
            void kill() override
            {
                this->kill_requested_ = true;
            }

            std::optional<int> run() override
            {
                std::array<uint8_t, 4096> buffer{};
                bool killed = false;
                for (;;)
                {
                    if (this->kill_requested_ && !killed)
                    {
                        killed = !this->process_.kill();
                    }

                    const auto [events, poll_error] =
                        this->process_.poll(reproc::event::out | reproc::event::err | reproc::event::exit, reproc::milliseconds(50));
                    if (poll_error && poll_error != std::errc::timed_out)
                    {
                        return std::nullopt;
                    }

                    for (const auto& [event, stream, file] : {std::tuple{reproc::event::out, reproc::stream::out, stdout},
                                                              std::tuple{reproc::event::err, reproc::stream::err, stderr}})
                    {
                        if ((events & event) == 0)
                        {
                            continue;
                        }
                        const auto [size, read_error] = this->process_.read(stream, buffer.data(), buffer.size());
                        if (!read_error)
                        {
                            (void)fwrite(buffer.data(), 1, size, file);
                            fflush(file);
                        }
                    }

                    if ((events & reproc::event::exit) != 0)
                    {
                        const auto [status, wait_error] = this->process_.wait(reproc::infinite);
                        if (wait_error)
                        {
                            return std::nullopt;
                        }
                        return status;
                    }
                }
            }

          private:
            reproc::process process_{};
            uint32_t id_{};
            std::atomic_bool kill_requested_{};
        };

        class subprocess_launcher final : public host_process_launcher
        {
          public:
            host_process_launch_result launch(const std::vector<std::string>& command) override
            {
                reproc::process process{};
                reproc::options options{};
                options.redirect.in.type = reproc::redirect::parent;
                options.redirect.out.type = reproc::redirect::pipe;
                options.redirect.err.type = reproc::redirect::pipe;
                if (const auto start_error = process.start(command, options))
                {
                    return {.error = map_process_error(start_error)};
                }

                const auto [id, id_error] = process.pid();
                if (id_error || id <= 0)
                {
                    std::ignore = process.kill();
                    std::ignore = process.wait(reproc::infinite);
                    return {.error = process_error::internal_failure};
                }

                return {.process = std::make_unique<subprocess>(std::move(process), static_cast<uint32_t>(id))};
            }
        };
    }

    std::unique_ptr<host_process_launcher> create_subprocess_launcher()
    {
        return std::make_unique<subprocess_launcher>();
    }
}

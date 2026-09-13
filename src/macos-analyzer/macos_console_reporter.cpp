#include "std_include.hpp"

#include "macos_reporter.hpp"

namespace sogen
{
    namespace
    {
        class macos_console_reporter : public macos_analysis_reporter
        {
          public:
            macos_console_reporter(logger& log, const macos_console_reporter_settings settings)
                : log_(&log),
                  settings_(settings)
            {
            }

            void report(const macos_analysis_event& event) override
            {
                std::visit([this](const auto& value) { this->render(value); }, event);
            }

          private:
            logger* log_{};
            macos_console_reporter_settings settings_{};

            // Guest output is the one thing silent mode keeps: it is the program's own writing, not the
            // emulator talking about the program.
            static void render(const macos_stdout_chunk_event& event)
            {
                std::fwrite(event.data.data(), 1, event.data.size(), event.is_stderr ? stderr : stdout);
                std::fflush(event.is_stderr ? stderr : stdout);
            }

            void render(const macos_run_started_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->info("Emulating %s with %s\n", event.application.c_str(), event.backend_name.c_str());
            }

            void render(const macos_run_finished_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                if (event.exit_status.has_value())
                {
                    this->log_->success("Process exited with status %d after %" PRIu64 " instructions\n", *event.exit_status,
                                        event.instructions);
                    return;
                }

                this->log_->info("Emulation finished after %" PRIu64 " instructions: %s\n", event.instructions, event.stop_detail.c_str());
            }

            void render(const macos_run_failed_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->error("Emulation failed at 0x%" PRIx64 ": %s\n", event.pc, event.message.c_str());
            }

            void render(const macos_syscall_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                const auto* kind = event.is_mach_trap ? "mach trap" : "syscall";

                if (this->settings_.prepend_call_count)
                {
                    this->log_->print(color::dark_gray,
                                      "[%" PRIu64 "] [t%" PRIu64 "] Executing %s: %s (0x%" PRIx64 ") at 0x%" PRIx64 " (%s)\n",
                                      event.call_count, event.execution.thread_id, kind, event.syscall_name.c_str(), event.syscall_id,
                                      event.execution.pc, event.execution.module.c_str());
                    return;
                }

                this->log_->print(color::dark_gray, "[t%" PRIu64 "] Executing %s: %s (0x%" PRIx64 ") at 0x%" PRIx64 " (%s)\n",
                                  event.execution.thread_id, kind, event.syscall_name.c_str(), event.syscall_id, event.execution.pc,
                                  event.execution.module.c_str());
            }

            // An empty label is not a missing one: the fenced decoder emits a single unlabelled marker
            // row, and printing "--> : text" for it would read as a field whose name went missing.
            void render(const macos_trace_detail_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                if (event.label.empty())
                {
                    this->log_->print(color::gray, "    --> %s\n", event.value.c_str());
                    return;
                }

                this->log_->print(color::gray, "    --> %s: %s\n", event.label.c_str(), event.value.c_str());
            }

            void render(const macos_syscall_error_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                if (event.error_name.empty())
                {
                    this->log_->print(color::yellow, "    --> Failed: errno %" PRId64 "\n", event.error);
                    return;
                }

                this->log_->print(color::yellow, "    --> Failed: %s (%" PRId64 ")\n", event.error_name.c_str(), event.error);
            }

            void render(const macos_generic_access_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->print(color::gray, "    --> %s: %s\n", event.type.c_str(), event.name.c_str());
            }

            void render(const macos_generic_activity_event& event) const
            {
                if (this->settings_.silent || this->settings_.concise)
                {
                    return;
                }

                this->log_->print(color::gray, "    --> %s\n", event.details.c_str());
            }

            void render(const macos_suspicious_activity_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->warn("    --> %s at 0x%" PRIx64 " (%s)\n", event.details.c_str(), event.execution.pc,
                                 event.execution.module.c_str());
            }

            void render(const macos_memory_allocate_event& event) const
            {
                if (this->settings_.silent || this->settings_.concise)
                {
                    return;
                }

                this->log_->print(color::dark_gray, "    --> %s 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n",
                                  event.commit ? "Committed" : "Reserved", event.address, event.address + event.length,
                                  event.permissions.c_str());
            }

            void render(const macos_memory_protect_event& event) const
            {
                if (this->settings_.silent || this->settings_.concise)
                {
                    return;
                }

                this->log_->print(color::dark_gray, "    --> Changing protection at 0x%" PRIx64 "-0x%" PRIx64 " to %s\n", event.address,
                                  event.address + event.length, event.permissions.c_str());
            }

            void render(const macos_memory_release_event& event) const
            {
                if (this->settings_.silent || this->settings_.concise)
                {
                    return;
                }

                this->log_->print(color::dark_gray, "    --> Releasing 0x%" PRIx64 " - 0x%" PRIx64 "\n", event.address,
                                  event.address + event.length);
            }

            void render(const macos_module_load_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->info("Mapped %s at 0x%" PRIx64 " (0x%" PRIx64 " bytes)\n", event.path.c_str(), event.image_base,
                                 event.image_size);
            }

            void render(const macos_dyld_image_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->info("dyld loaded %s at 0x%" PRIx64 " (0x%" PRIx64 " bytes)\n", event.path.c_str(), event.image_base,
                                 event.image_size);
            }

            void render(const macos_thread_create_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->print(color::gray, "    --> Creating thread %" PRIu64 " at 0x%" PRIx64 " (argument 0x%" PRIx64 ")\n",
                                  event.thread_id, event.start_address, event.argument);
            }

            void render(const macos_thread_terminated_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->print(color::gray, "    --> Thread %" PRIu64 " terminated\n", event.thread_id);
            }

            void render(const macos_mach_port_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->print(color::gray, "    --> Port 0x%x (%s): %s\n", event.port_name, event.right.c_str(),
                                  event.description.c_str());
            }

            void render(const macos_mach_message_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->print(color::gray, "    --> Message %u to port 0x%x (%s)\n", event.message_id, event.remote_port,
                                  event.subsystem.c_str());
            }

            void render(const macos_process_exit_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->print(color::gray, "    --> Process exiting with status %d\n", event.exit_status);
            }

            void render(const macos_cpu_exception_event& event) const
            {
                if (this->settings_.silent)
                {
                    return;
                }

                this->log_->error("CPU exception %d (%s) at 0x%" PRIx64 "\n", event.exception_index, event.description.c_str(),
                                  event.address);
            }
        };
    }

    std::unique_ptr<macos_analysis_reporter> create_macos_console_reporter(logger& log, const macos_console_reporter_settings settings)
    {
        return std::make_unique<macos_console_reporter>(log, settings);
    }
}

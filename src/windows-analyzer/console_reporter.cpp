#include "std_include.hpp"

#include "analysis_reporter.hpp"
#include "analysis_reporter_common.hpp"

#include <cinttypes>
#include <logger.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace sogen
{

    using analysis_reporter_detail::make_overloaded;

    namespace
    {
        // A regular syscall's line. Verbose runs print one per syscall, tens of thousands a second, while the syscall
        // holds the kernel lock, so the logger formats it on its writer thread from this copy.
        struct syscall_line
        {
            bool has_call_count{};
            uint64_t call_count{};
            uint32_t syscall_id{};
            uint32_t thread_id{};
            uint32_t vcpu{};
            uint64_t rip{};
            uint64_t caller_rip{};
            std::array<char, 64> syscall_name{};
            std::array<char, 64> caller_module{};
        };

        template <size_t Size>
        void copy_truncated(std::array<char, Size>& to, const std::string_view from)
        {
            const auto size = std::min(from.size(), Size - 1);
            from.copy(to.data(), size);
            to[size] = '\0';
        }

        void format_syscall_line(const syscall_line& line, std::string& out)
        {
            if (line.has_call_count)
            {
                out += '[';
                out += std::to_string(line.call_count);
                out += "] ";
            }

            std::array<char, 0x200> buffer{};
            const auto size = std::snprintf(
                buffer.data(), buffer.size(),
                "Executing syscall: %s (0x%X) at 0x%" PRIx64 " via 0x%" PRIx64 " (%s) (tid: %" PRIu32 ", vCPU: %" PRIu32 ")\n",
                line.syscall_name.data(), line.syscall_id, line.rip, line.caller_rip, line.caller_module.data(), line.thread_id, line.vcpu);
            out.append(buffer.data(), std::min(static_cast<size_t>(std::max(size, 0)), buffer.size() - 1));
        }

        class console_analysis_reporter final : public analysis_reporter
        {
          public:
            console_analysis_reporter(logger& log, console_reporter_settings settings)
                : log_(log),
                  settings_(settings)
            {
            }

            void report(const analysis_event& event) override
            {
                if (this->settings_.silent)
                {
                    this->report_silent(event);
                }
                else
                {
                    report_regular(event);
                }
            }

            // The guest's output goes to stdout directly, so the lines the logger queued before it are flushed first.
            void report_silent(const analysis_event& event) const
            {
                std::visit(make_overloaded(
                               [&](const run_finished_event&) {
                                   this->log_.flush();
                                   fflush(stdout); //
                               },
                               [&](const stdout_chunk_event& e) {
                                   this->log_.flush();
                                   (void)fwrite(e.data.data(), 1, e.data.size(), stdout); //
                                   fflush(stdout);
                               },
                               [&](const auto&) {
                                   // Ignore all other events in silent mode.
                               }),
                           event);
            }

            std::string make_call_prefix(const uint64_t call_count) const
            {
                if (!this->settings_.prepend_call_count)
                {
                    return {};
                }

                return "[" + std::to_string(call_count) + "] ";
            }

            void report_regular(const analysis_event& event)
            {
                std::visit(
                    make_overloaded(
                        [&](const run_started_event& e) {
                            this->log_.force_print(color::gray, "Using emulator backend: %s\n", e.backend_name.c_str());
                        },
                        [&](const run_finished_event& e) {
                            if (e.exit_status.has_value())
                            {
                                this->log_.print(e.success ? color::green : color::red, "Emulation terminated with status: %X\n",
                                                 *e.exit_status);
                            }
                        },
                        [&](const run_failed_event& e) {
                            this->log_.error("Emulation failed at: 0x%" PRIx64 " - %s\n", e.rip, e.message.c_str());
                        },
                        [&](const instruction_summary_event& e) {
                            this->log_.print(color::white, "Instruction summary:\n");
                            for (const auto& entry : e.entries)
                            {
                                this->log_.print(color::white, "%s: %" PRIu64 "\n", entry.mnemonic.c_str(), entry.count);
                            }
                        },
                        [&](const buffered_stdout_event& e) {
                            if (this->settings_.buffer_stdout && !e.data.empty())
                            {
                                this->log_.info("%.*s%s", static_cast<int>(e.data.size()), e.data.data(),
                                                e.data.ends_with("\n") ? "" : "\n");
                            }
                        },
                        [&](const stdout_chunk_event& e) {
                            if (!this->settings_.buffer_stdout)
                            {
                                this->log_.info("%.*s%s", static_cast<int>(e.data.size()), e.data.data(),
                                                e.data.ends_with("\n") ? "" : "\n");
                            }
                        },
                        [&](const suspicious_activity_event& e) {
                            const auto addition = e.decoded_instruction.empty() ? std::string{} : " ("s + e.decoded_instruction + ")";
                            this->log_.print(color::pink, "Suspicious: %s%s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n", e.details.c_str(),
                                             addition.c_str(), e.execution.rip, e.execution.previous_ip.value_or(0),
                                             e.execution.previous_ip_module.value_or("<N/A>").c_str());
                        },
                        [&](const debug_string_event& e) { this->log_.info("--> Debug string: %s\n", e.details.c_str()); },
                        [&](const generic_activity_event& e) { this->log_.print(color::dark_gray, "%s\n", e.details.c_str()); },
                        [&](const generic_access_event& e) {
                            this->log_.print(color::dark_gray, "--> %s: %s\n", e.type.c_str(), e.name.c_str());
                        },
                        [&](const memory_allocate_event& e) {
                            this->log_.print(e.permissions.find('x') != std::string::npos ? color::gray : color::dark_gray,
                                             "--> %s 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n", e.commit ? "Committed" : "Allocating",
                                             e.address, e.address + e.length, e.permissions.c_str());
                        },
                        [&](const memory_protect_event& e) {
                            this->log_.print(color::dark_gray, "--> Changing protection at 0x%" PRIx64 "-0x%" PRIx64 " to %s\n", e.address,
                                             e.address + e.length, e.permissions.c_str());
                        },
                        [&](const memory_violation_event& e) {
                            const auto* label = e.violation_type == "protection" ? "Protection violation" : "Mapping violation";
                            this->log_.print(color::gray, "%s: 0x%" PRIx64 " (%" PRIx64 ") - %s at 0x%" PRIx64 " (%s)\n", label, e.address,
                                             e.size, e.operation.c_str(), e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const io_control_event& e) {
                            this->log_.print(color::dark_gray, "--> %s: 0x%X\n", e.device_name.c_str(), e.code);
                        },
                        [&](const socket_activity_event& e) {
                            this->log_.print(color::dark_gray, "Socket %s: %s\n", e.action.c_str(), e.address.c_str());
                        },
                        [&](const thread_create_event& e) {
                            std::string flags{};
                            for (const auto& flag : e.flags)
                            {
                                flags += ", ";
                                flags += flag;
                            }

                            this->log_.print(color::gray, "Thread created: tid %u, start address 0x%" PRIx64 " (param 0x%" PRIx64 ")%s\n",
                                             e.created_thread_id, e.start_address, e.argument, flags.c_str());
                        },
                        [&](const thread_terminated_event& e) {
                            this->log_.print(color::gray, "Thread terminated: tid %u\n", e.terminated_thread_id);
                        },
                        [&](const thread_set_name_event& e) {
                            this->log_.print(color::blue, "Setting thread (%u) name: %s\n", e.renamed_thread_id, e.name.c_str());
                        },
                        [&](const thread_switch_event&) {
                            // this->log_.print(color::dark_gray, "Performing thread switch: %X -> %X\n", e.previous_thread_id,
                            //                  e.next_thread_id);
                        },
                        [&](const module_load_event& e) { this->log_.log("Mapped %s at 0x%" PRIx64 "\n", e.path.c_str(), e.image_base); },
                        [&](const module_unload_event& e) {
                            this->log_.log("Unmapping %s (0x%" PRIx64 ")\n", e.path.c_str(), e.image_base);
                        },
                        [&](const import_read_event& e) {
                            this->log_.print(color::green, "Import read access: %s (%s) at 0x%" PRIx64 " (%s)\n", e.import_name.c_str(),
                                             e.import_module.c_str(), e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const import_write_event& e) {
                            this->log_.print(color::blue,
                                             "Import write access: %zd bytes with value 0x%" PRIX64 " to %s (%s) at 0x%" PRIx64 " (%s)\n",
                                             e.size, e.value, e.import_name.c_str(), e.import_module.c_str(), e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const object_access_event& e) {
                            this->log_.print(e.main_access ? color::green : color::dark_gray,
                                             "Object access: %s - 0x%" PRIx64 " 0x%" PRIx64 " (%s) at 0x%" PRIx64 " (%s)\n",
                                             e.type_name.c_str(), e.offset, e.size, e.member_name.value_or("<N/A>").c_str(),
                                             e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const environment_access_event& e) {
                            this->log_.print(e.main_access ? color::green : color::dark_gray,
                                             "Environment access: 0x%" PRIx64 " (0x%zX) at 0x%" PRIx64 " (%s)\n", e.offset,
                                             static_cast<size_t>(e.size), e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const function_execution_event& e) {
                            const auto prefix = this->make_call_prefix(e.call_count);
                            this->log_.print(e.interesting ? color::yellow : color::dark_gray,
                                             "%sExecuting function: %s (%s) (0x%" PRIx64 ") via 0x%" PRIx64 " (%s)\n", prefix.c_str(),
                                             e.function_name.c_str(), e.execution.rip_module.c_str(), e.execution.rip,
                                             e.execution.previous_ip.value_or(0), e.execution.previous_ip_module.value_or("<N/A>").c_str());
                            for (const auto& detail : e.details)
                            {
                                if (detail.label.empty())
                                {
                                    this->log_.print(color::dark_gray, "--> %s\n", detail.value.c_str());
                                }
                                else
                                {
                                    this->log_.print(color::dark_gray, "--> %s: %s\n", detail.label.c_str(), detail.value.c_str());
                                }
                            }
                        },
                        [&](const entry_point_execution_event& e) {
                            this->log_.print(e.interesting ? color::yellow : color::gray, "Executing entry point: %s (0x%" PRIx64 ")\n",
                                             e.execution.rip_module.c_str(), e.execution.rip);
                        },
                        [&](const foreign_code_transition_event& e) {
                            this->log_.print(e.interesting ? color::yellow : color::dark_gray,
                                             "Transition to foreign code: %s+0x%" PRIx64 " (%s) (0x%" PRIx64 ") via 0x%" PRIx64 " (%s)\n",
                                             e.function_name.c_str(), e.function_offset, e.execution.rip_module.c_str(), e.execution.rip,
                                             e.execution.previous_ip.value_or(0), e.execution.previous_ip_module.value_or("<N/A>").c_str());
                        },
                        [&](const section_first_execute_event& e) {
                            this->log_.print(
                                color::green, "Section %s (%s) first execute at 0x%" PRIx64 " 0x%" PRIx64 " (tid: %" PRIu32 ")\n",
                                e.module_name.c_str(), e.section_name.c_str(), e.execution.rip, e.file_address, e.execution.thread_id);
                        },
                        [&](const rdtsc_event& e) {
                            this->log_.print(color::blue, "Executing RDTSC instruction at 0x%" PRIx64 " (%s)\n", e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const rdtscp_event& e) {
                            this->log_.print(color::blue, "Executing RDTSCP instruction at 0x%" PRIx64 " (%s)\n", e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const cpuid_event& e) {
                            this->log_.print(color::blue, "Executing CPUID instruction with leaf 0x%X at 0x%" PRIx64 " (%s)\n", e.leaf,
                                             e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const syscall_event& e) {
                            const auto prefix = this->make_call_prefix(e.call_count);
                            switch (e.classification)
                            {
                            case syscall_classification::inline_syscall:
                                this->log_.print(color::blue,
                                                 "%sExecuting inline syscall: %s (0x%X) at 0x%" PRIx64 " (%s) (tid: %" PRIu32
                                                 ", vCPU: %" PRIu32 ")\n",
                                                 prefix.c_str(), e.syscall_name.c_str(), e.syscall_id, e.execution.rip,
                                                 e.execution.rip_module.c_str(), e.execution.thread_id, e.execution.vcpu);
                                break;
                            case syscall_classification::crafted_out_of_line:
                                this->log_.print(color::blue,
                                                 "%sCrafted out-of-line syscall: %s (0x%X) at 0x%" PRIx64 " (%s) via 0x%" PRIx64
                                                 " (%s) (tid: %" PRIu32 ", vCPU: %" PRIu32 ")\n",
                                                 prefix.c_str(), e.syscall_name.c_str(), e.syscall_id, e.execution.rip,
                                                 e.execution.rip_module.c_str(), e.execution.previous_ip.value_or(0),
                                                 e.execution.previous_ip_module.value_or("<N/A>").c_str(), e.execution.thread_id,
                                                 e.execution.vcpu);
                                break;
                            case syscall_classification::regular:
                            default: {
                                syscall_line line{.has_call_count = this->settings_.prepend_call_count,
                                                  .call_count = e.call_count,
                                                  .syscall_id = e.syscall_id,
                                                  .thread_id = e.execution.thread_id,
                                                  .vcpu = e.execution.vcpu,
                                                  .rip = e.execution.rip,
                                                  .caller_rip = e.caller_rip.value_or(0)};
                                copy_truncated(line.syscall_name, e.syscall_name);
                                copy_truncated(line.caller_module, e.caller_module.value_or("<N/A>"));
                                this->log_.print_deferred<format_syscall_line>(color::dark_gray, line);
                                break;
                            }
                            }
                        },
                        [&](const foreign_module_read_event& e) {
                            this->log_.print(color::pink, "Reading %zd bytes from module %s at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                                             e.size, e.module_name.c_str(), e.address, e.region_name.c_str(), e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const executable_read_event& e) {
                            this->log_.print(color::green,
                                             "Reading %zd bytes from executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                                             e.size, e.section_name.c_str(), e.address, e.execution.rip, e.execution.rip_module.c_str());
                        },
                        [&](const executable_write_event& e) {
                            this->log_.print(color::blue,
                                             "Writing %zd bytes with value 0x%" PRIX64 " to executable section %s at 0x%" PRIx64
                                             " via 0x%" PRIx64 " (%s)\n",
                                             e.size, e.value, e.section_name.c_str(), e.address, e.execution.rip,
                                             e.execution.rip_module.c_str());
                        },
                        [&](const fast_fail_event& e) {
                            this->log_.print(color::red, "Process requested fast fail with code %d\n", e.fail_code);
                        }),
                    event);
            }

          private:
            logger& log_;
            console_reporter_settings settings_{};
        };
    }

    std::unique_ptr<analysis_reporter> create_console_reporter(logger& log, const console_reporter_settings settings)
    {
        return std::make_unique<console_analysis_reporter>(log, settings);
    }

} // namespace sogen

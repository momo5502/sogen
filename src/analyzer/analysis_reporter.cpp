#include "std_include.hpp"

#include "analysis_reporter.hpp"

#include <cinttypes>
#include <logger.hpp>

namespace
{
    template <typename... Ts>
    struct overloaded : Ts...
    {
        using Ts::operator()...;
    };

    template <typename... Ts>
    auto make_overloaded(Ts&&... ts)
    {
        return overloaded<std::decay_t<Ts>...>{std::forward<Ts>(ts)...};
    }

    std::string hex_string(const uint64_t value)
    {
        char buffer[32] = {};
        snprintf(buffer, sizeof(buffer), "0x%" PRIx64, value);
        return buffer;
    }

    std::string escape_json(std::string_view value)
    {
        std::string escaped{};
        escaped.reserve(value.size() + 8);

        for (const auto ch : value)
        {
            switch (ch)
            {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    char buffer[8] = {};
                    snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(ch));
                    escaped += buffer;
                }
                else
                {
                    escaped.push_back(ch);
                }
                break;
            }
        }

        return escaped;
    }

    class json_object_builder
    {
      public:
        explicit json_object_builder(std::string& output)
            : output_(output)
        {
            this->output_ += '{';
        }

        ~json_object_builder()
        {
            this->output_ += '}';
        }

        void field(std::string_view key, std::string_view value)
        {
            this->key(key);
            this->output_ += '"';
            this->output_ += escape_json(value);
            this->output_ += '"';
        }

        void field(std::string_view key, const char* value)
        {
            this->field(key, std::string_view(value ? value : ""));
        }

        void field(std::string_view key, const std::string& value)
        {
            this->field(key, std::string_view(value));
        }

        void field(std::string_view key, const std::filesystem::path& value)
        {
            this->field(key, value.string());
        }

        void field(std::string_view key, const bool value)
        {
            this->key(key);
            this->output_ += value ? "true" : "false";
        }

        void field(std::string_view key, const uint32_t value)
        {
            this->key(key);
            this->output_ += std::to_string(value);
        }

        void field(std::string_view key, const uint64_t value)
        {
            this->field(key, std::to_string(value));
        }

        void hex_field(std::string_view key, const uint64_t value)
        {
            this->field(key, hex_string(value));
        }

        void optional_hex_field(std::string_view key, const std::optional<uint64_t>& value)
        {
            if (value.has_value())
            {
                this->hex_field(key, *value);
            }
        }

        void optional_string_field(std::string_view key, const std::optional<std::string>& value)
        {
            if (value.has_value())
            {
                this->field(key, *value);
            }
        }

        template <typename Callback>
        void object_field(std::string_view key, Callback&& callback)
        {
            this->key(key);
            json_object_builder child{this->output_};
            callback(child);
        }

        template <typename Callback>
        void array_field(std::string_view key, Callback&& callback)
        {
            this->key(key);
            this->output_ += '[';
            bool first = true;
            callback([&](const auto& writer) {
                if (!first)
                {
                    this->output_ += ',';
                }
                first = false;
                writer(this->output_);
            });
            this->output_ += ']';
        }

      private:
        std::string& output_;
        bool first_{true};

        void key(std::string_view key)
        {
            if (!this->first_)
            {
                this->output_ += ',';
            }

            this->first_ = false;
            this->output_ += '"';
            this->output_ += escape_json(key);
            this->output_ += "\":";
        }
    };

    std::string_view syscall_classification_name(const syscall_classification classification)
    {
        switch (classification)
        {
        case syscall_classification::regular:
            return "regular";
        case syscall_classification::inline_syscall:
            return "inline";
        case syscall_classification::crafted_out_of_line:
            return "crafted_out_of_line";
        default:
            return "unknown";
        }
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
            std::visit(
                make_overloaded(
                    [&](const run_started_event& e) {
                        if (!this->settings_.silent)
                        {
                            this->log_.force_print(color::gray, "Using emulator backend: %s\n", e.payload.backend_name.c_str());
                        }
                    },
                    [&](const run_finished_event& e) {
                        if (!this->settings_.silent && e.payload.exit_status.has_value())
                        {
                            this->log_.print(e.payload.success ? color::green : color::red, "Emulation terminated with status: %X\n",
                                             *e.payload.exit_status);
                        }
                    },
                    [&](const run_failed_event& e) {
                        this->log_.error("Emulation failed at: 0x%" PRIx64 " - %s\n", e.payload.rip, e.payload.message.c_str());
                    },
                    [&](const instruction_summary_event& e) {
                        this->log_.print(color::white, "Instruction summary:\n");
                        for (const auto& entry : e.payload.entries)
                        {
                            this->log_.print(color::white, "%s: %" PRIu64 "\n", entry.mnemonic.c_str(), entry.count);
                        }
                    },
                    [&](const buffered_stdout_event& e) {
                        if (this->settings_.buffer_stdout && !this->settings_.silent && !e.payload.data.empty())
                        {
                            this->log_.info("%.*s%s", static_cast<int>(e.payload.data.size()), e.payload.data.data(),
                                            e.payload.data.ends_with("\n") ? "" : "\n");
                        }
                    },
                    [&](const stdout_chunk_event& e) {
                        if (this->settings_.silent)
                        {
                            (void)fwrite(e.payload.data.data(), 1, e.payload.data.size(), stdout);
                        }
                        else if (!this->settings_.buffer_stdout)
                        {
                            this->log_.info("%.*s%s", static_cast<int>(e.payload.data.size()), e.payload.data.data(),
                                            e.payload.data.ends_with("\n") ? "" : "\n");
                        }
                    },
                    [&](const suspicious_activity_event& e) {
                        const auto addition =
                            e.payload.decoded_instruction.empty() ? std::string{} : " ("s + e.payload.decoded_instruction + ")";
                        this->log_.print(color::pink, "Suspicious: %s%s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                                         e.payload.details.c_str(), addition.c_str(), e.execution.rip, e.execution.previous_ip.value_or(0),
                                         e.execution.previous_ip_module.value_or("<N/A>").c_str());
                    },
                    [&](const debug_string_event& e) { this->log_.info("--> Debug string: %s\n", e.payload.details.c_str()); },
                    [&](const generic_activity_event& e) { this->log_.print(color::dark_gray, "%s\n", e.payload.details.c_str()); },
                    [&](const generic_access_event& e) {
                        this->log_.print(color::dark_gray, "--> %s: %s\n", e.payload.type.c_str(), e.payload.name.c_str());
                    },
                    [&](const memory_allocate_event& e) {
                        this->log_.print(e.payload.permissions.find('x') != std::string::npos ? color::gray : color::dark_gray,
                                         "--> %s 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n", e.payload.commit ? "Committed" : "Allocating",
                                         e.payload.address, e.payload.address + e.payload.length, e.payload.permissions.c_str());
                    },
                    [&](const memory_protect_event& e) {
                        this->log_.print(color::dark_gray, "--> Changing protection at 0x%" PRIx64 "-0x%" PRIx64 " to %s\n",
                                         e.payload.address, e.payload.address + e.payload.length, e.payload.permissions.c_str());
                    },
                    [&](const memory_violation_event& e) {
                        const auto* label = e.payload.violation_type == "protection" ? "Protection violation" : "Mapping violation";
                        this->log_.print(color::gray, "%s: 0x%" PRIx64 " (%" PRIx64 ") - %s at 0x%" PRIx64 " (%s)\n", label,
                                         e.payload.address, e.payload.size, e.payload.operation.c_str(), e.execution.rip,
                                         e.execution.rip_module.c_str());
                    },
                    [&](const io_control_event& e) {
                        this->log_.print(color::dark_gray, "--> %s: 0x%X\n", e.payload.device_name.c_str(), e.payload.code);
                    },
                    [&](const thread_create_event& e) {
                        std::string flags{};
                        for (const auto& flag : e.payload.flags)
                        {
                            flags += ", ";
                            flags += flag;
                        }

                        this->log_.print(color::gray, "Thread created: tid %u, start address 0x%" PRIx64 " (param 0x%" PRIx64 ")%s\n",
                                         e.payload.created_thread_id, e.payload.start_address, e.payload.argument, flags.c_str());
                    },
                    [&](const thread_terminated_event& e) {
                        this->log_.print(color::gray, "Thread terminated: tid %u\n", e.payload.terminated_thread_id);
                    },
                    [&](const thread_set_name_event& e) {
                        this->log_.print(color::blue, "Setting thread (%u) name: %s\n", e.payload.renamed_thread_id,
                                         e.payload.name.c_str());
                    },
                    [&](const thread_switch_event& e) {
                        this->log_.print(color::dark_gray, "Performing thread switch: %X -> %X\n", e.payload.previous_thread_id,
                                         e.payload.next_thread_id);
                    },
                    [&](const module_load_event& e) {
                        this->log_.log("Mapped %s at 0x%" PRIx64 "\n", e.payload.path.c_str(), e.payload.image_base);
                    },
                    [&](const module_unload_event& e) {
                        this->log_.log("Unmapping %s (0x%" PRIx64 ")\n", e.payload.path.c_str(), e.payload.image_base);
                    },
                    [&](const import_read_event& e) {
                        this->log_.print(color::green, "Import read access: %s (%s) at 0x%" PRIx64 " (%s)\n", e.payload.import_name.c_str(),
                                         e.payload.import_module.c_str(), e.execution.rip, e.execution.rip_module.c_str());
                    },
                    [&](const import_write_event& e) {
                        this->log_.print(color::blue,
                                         "Import write access: %zd bytes with value 0x%" PRIX64 " to %s (%s) at 0x%" PRIx64 " (%s)\n",
                                         e.payload.size, e.payload.value, e.payload.import_name.c_str(), e.payload.import_module.c_str(),
                                         e.execution.rip, e.execution.rip_module.c_str());
                    },
                    [&](const object_access_event& e) {
                        this->log_.print(e.payload.main_access ? color::green : color::dark_gray,
                                         "Object access: %s - 0x%" PRIx64 " 0x%" PRIx64 " (%s) at 0x%" PRIx64 " (%s)\n",
                                         e.payload.type_name.c_str(), e.payload.offset, e.payload.size,
                                         e.payload.member_name.value_or("<N/A>").c_str(), e.execution.rip, e.execution.rip_module.c_str());
                    },
                    [&](const environment_access_event& e) {
                        this->log_.print(e.payload.main_access ? color::green : color::dark_gray,
                                         "Environment access: 0x%" PRIx64 " (0x%zX) at 0x%" PRIx64 " (%s)\n", e.payload.offset,
                                         static_cast<size_t>(e.payload.size), e.execution.rip, e.execution.rip_module.c_str());
                    },
                    [&](const function_execution_event& e) {
                        this->log_.print(e.payload.interesting ? color::yellow : color::dark_gray,
                                         "Executing function: %s (%s) (0x%" PRIx64 ") via 0x%" PRIx64 " (%s)\n",
                                         e.payload.function_name.c_str(), e.execution.rip_module.c_str(), e.execution.rip,
                                         e.execution.previous_ip.value_or(0), e.execution.previous_ip_module.value_or("<N/A>").c_str());
                        for (const auto& detail : e.payload.details)
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
                        this->log_.print(e.payload.interesting ? color::yellow : color::gray, "Executing entry point: %s (0x%" PRIx64 ")\n",
                                         e.execution.rip_module.c_str(), e.execution.rip);
                    },
                    [&](const foreign_code_transition_event& e) {
                        this->log_.print(e.payload.interesting ? color::yellow : color::dark_gray,
                                         "Transition to foreign code: %s+0x%" PRIx64 " (%s) (0x%" PRIx64 ") via 0x%" PRIx64 " (%s)\n",
                                         e.payload.function_name.c_str(), e.payload.function_offset, e.execution.rip_module.c_str(),
                                         e.execution.rip, e.execution.previous_ip.value_or(0),
                                         e.execution.previous_ip_module.value_or("<N/A>").c_str());
                    },
                    [&](const section_first_execute_event& e) {
                        this->log_.print(color::green, "Section %s (%s) first execute at 0x%" PRIx64 " 0x%" PRIx64 " (tid: %" PRIu32 ")\n",
                                         e.payload.module_name.c_str(), e.payload.section_name.c_str(), e.execution.rip,
                                         e.payload.file_address, e.execution.thread_id);
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
                        this->log_.print(color::blue, "Executing CPUID instruction with leaf 0x%X at 0x%" PRIx64 " (%s)\n", e.payload.leaf,
                                         e.execution.rip, e.execution.rip_module.c_str());
                    },
                    [&](const syscall_event& e) {
                        switch (e.payload.classification)
                        {
                        case syscall_classification::inline_syscall:
                            this->log_.print(color::blue, "Executing inline syscall: %s (0x%X) at 0x%" PRIx64 " (%s)\n",
                                             e.payload.syscall_name.c_str(), e.payload.syscall_id, e.execution.rip,
                                             e.execution.rip_module.c_str());
                            break;
                        case syscall_classification::crafted_out_of_line:
                            this->log_.print(
                                color::blue, "Crafted out-of-line syscall: %s (0x%X) at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                                e.payload.syscall_name.c_str(), e.payload.syscall_id, e.execution.rip, e.execution.rip_module.c_str(),
                                e.execution.previous_ip.value_or(0), e.execution.previous_ip_module.value_or("<N/A>").c_str());
                            break;
                        case syscall_classification::regular:
                        default:
                            this->log_.print(color::dark_gray, "Executing syscall: %s (0x%X) at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                                             e.payload.syscall_name.c_str(), e.payload.syscall_id, e.execution.rip,
                                             e.payload.caller_rip.value_or(0), e.payload.caller_module.value_or("<N/A>").c_str());
                            break;
                        }
                    },
                    [&](const foreign_module_read_event& e) {
                        this->log_.print(color::pink, "Reading %zd bytes from module %s at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                                         e.payload.size, e.payload.module_name.c_str(), e.payload.address, e.payload.region_name.c_str(),
                                         e.execution.rip, e.execution.rip_module.c_str());
                    },
                    [&](const executable_read_event& e) {
                        this->log_.print(color::green,
                                         "Reading %zd bytes from executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                                         e.payload.size, e.payload.section_name.c_str(), e.payload.address, e.execution.rip,
                                         e.execution.rip_module.c_str());
                    },
                    [&](const executable_write_event& e) {
                        this->log_.print(color::blue,
                                         "Writing %zd bytes with value 0x%" PRIX64 " to executable section %s at 0x%" PRIx64
                                         " via 0x%" PRIx64 " (%s)\n",
                                         e.payload.size, e.payload.value, e.payload.section_name.c_str(), e.payload.address,
                                         e.execution.rip, e.execution.rip_module.c_str());
                    }),
                event);
        }

      private:
        logger& log_;
        console_reporter_settings settings_{};
    };

    class jsonl_analysis_reporter final : public analysis_reporter
    {
      public:
        explicit jsonl_analysis_reporter(const std::filesystem::path& path)
            : file_(path, std::ios::binary | std::ios::out | std::ios::trunc)
        {
            if (!this->file_)
            {
                throw std::runtime_error("Failed to open analysis report file: " + path.string());
            }
        }

        void report(const analysis_event& event) override
        {
            std::string line{};
            line.reserve(1024);

            {
                json_object_builder object{line};
                object.field("schemaVersion", 1U);

                std::visit(make_overloaded([&](const auto& e) {
                               object.field("sequence", e.header.sequence);
                               object.field("instructionCount", e.header.instruction_count);
                               this->event_type_fields(object, e);
                               this->event_payload_field(object, e);
                           }),
                           event);
            }

            this->file_ << line << '\n';
        }

        void flush() override
        {
            this->file_.flush();
        }

      private:
        std::ofstream file_{};

        template <typename Payload>
        void event_type_fields(json_object_builder& object, const observation_event<Payload>& e)
        {
            object.field("category", "observation");
            object.field("type", this->event_name(e));
            object.object_field("execution", [&](json_object_builder& execution) {
                execution.field("threadId", e.execution.thread_id);
                execution.hex_field("rip", e.execution.rip);
                execution.field("ripModule", e.execution.rip_module);
                execution.optional_hex_field("previousIp", e.execution.previous_ip);
                execution.optional_string_field("previousIpModule", e.execution.previous_ip_module);
            });
        }

        template <typename Payload>
        void event_type_fields(json_object_builder& object, const summary_event<Payload>& e)
        {
            object.field("category", "summary");
            object.field("type", this->event_name(e));
        }

        template <typename Event>
        std::string_view event_name(const Event&) const
        {
            return "unknown";
        }

#define EVENT_NAME(TYPE, NAME)                     \
    std::string_view event_name(const TYPE&) const \
    {                                              \
        return NAME;                               \
    }

        EVENT_NAME(run_started_event, "run_started");
        EVENT_NAME(run_finished_event, "run_finished");
        EVENT_NAME(run_failed_event, "run_failed");
        EVENT_NAME(instruction_summary_event, "instruction_summary");
        EVENT_NAME(buffered_stdout_event, "buffered_stdout");
        EVENT_NAME(stdout_chunk_event, "stdout_chunk");
        EVENT_NAME(suspicious_activity_event, "suspicious_activity");
        EVENT_NAME(debug_string_event, "debug_string");
        EVENT_NAME(generic_activity_event, "generic_activity");
        EVENT_NAME(generic_access_event, "generic_access");
        EVENT_NAME(memory_allocate_event, "memory_allocate");
        EVENT_NAME(memory_protect_event, "memory_protect");
        EVENT_NAME(memory_violation_event, "memory_violation");
        EVENT_NAME(io_control_event, "io_control");
        EVENT_NAME(thread_create_event, "thread_create");
        EVENT_NAME(thread_terminated_event, "thread_terminated");
        EVENT_NAME(thread_set_name_event, "thread_set_name");
        EVENT_NAME(thread_switch_event, "thread_switch");
        EVENT_NAME(module_load_event, "module_load");
        EVENT_NAME(module_unload_event, "module_unload");
        EVENT_NAME(import_read_event, "import_read");
        EVENT_NAME(import_write_event, "import_write");
        EVENT_NAME(object_access_event, "object_access");
        EVENT_NAME(environment_access_event, "environment_access");
        EVENT_NAME(function_execution_event, "function_execution");
        EVENT_NAME(entry_point_execution_event, "entry_point_execution");
        EVENT_NAME(foreign_code_transition_event, "foreign_code_transition");
        EVENT_NAME(section_first_execute_event, "section_first_execute");
        EVENT_NAME(rdtsc_event, "rdtsc");
        EVENT_NAME(rdtscp_event, "rdtscp");
        EVENT_NAME(cpuid_event, "cpuid");
        EVENT_NAME(syscall_event, "syscall");
        EVENT_NAME(foreign_module_read_event, "foreign_module_read");
        EVENT_NAME(executable_read_event, "executable_read");
        EVENT_NAME(executable_write_event, "executable_write");

#undef EVENT_NAME

        template <typename Event>
        void event_payload_field(json_object_builder& object, const Event& event)
        {
            object.object_field("payload", [&](json_object_builder& payload) { this->write_payload(payload, event.payload); });
        }

        void write_payload(json_object_builder& payload, const run_started_payload& value)
        {
            payload.field("backendName", value.backend_name);
            payload.field("mode", value.mode);
            if (!value.application.empty())
            {
                payload.field("application", value.application);
            }
            payload.array_field("arguments", [&](const auto& emit) {
                for (const auto& arg : value.arguments)
                {
                    emit([&](std::string& out) {
                        out += '"';
                        out += escape_json(arg);
                        out += '"';
                    });
                }
            });
        }

        void write_payload(json_object_builder& payload, const run_finished_payload& value)
        {
            payload.field("success", value.success);
            if (value.exit_status.has_value())
            {
                payload.field("exitStatus", *value.exit_status);
            }
        }

        void write_payload(json_object_builder& payload, const run_failed_payload& value)
        {
            payload.hex_field("rip", value.rip);
            payload.field("message", value.message);
        }

        void write_payload(json_object_builder& payload, const instruction_summary_payload& value)
        {
            payload.array_field("entries", [&](const auto& emit) {
                for (const auto& entry : value.entries)
                {
                    emit([&](std::string& out) {
                        json_object_builder entry_object{out};
                        entry_object.field("mnemonic", entry.mnemonic);
                        entry_object.field("count", entry.count);
                    });
                }
            });
        }

        void write_payload(json_object_builder& payload, const buffered_stdout_payload& value)
        {
            payload.field("data", value.data);
        }

        void write_payload(json_object_builder& payload, const stdout_chunk_payload& value)
        {
            payload.field("data", value.data);
        }

        void write_payload(json_object_builder& payload, const suspicious_activity_payload& value)
        {
            payload.field("details", value.details);
            if (!value.decoded_instruction.empty())
            {
                payload.field("decodedInstruction", value.decoded_instruction);
            }
        }

        void write_payload(json_object_builder& payload, const debug_string_payload& value)
        {
            payload.field("details", value.details);
        }

        void write_payload(json_object_builder& payload, const generic_activity_payload& value)
        {
            payload.field("details", value.details);
        }

        void write_payload(json_object_builder& payload, const generic_access_payload& value)
        {
            payload.field("accessType", value.type);
            payload.field("name", value.name);
        }

        void write_payload(json_object_builder& payload, const memory_allocate_payload& value)
        {
            payload.hex_field("address", value.address);
            payload.field("length", value.length);
            payload.field("permissions", value.permissions);
            payload.field("commit", value.commit);
        }

        void write_payload(json_object_builder& payload, const memory_protect_payload& value)
        {
            payload.hex_field("address", value.address);
            payload.field("length", value.length);
            payload.field("permissions", value.permissions);
        }

        void write_payload(json_object_builder& payload, const memory_violation_payload& value)
        {
            payload.hex_field("address", value.address);
            payload.field("size", value.size);
            payload.field("operation", value.operation);
            payload.field("violationType", value.violation_type);
        }

        void write_payload(json_object_builder& payload, const io_control_payload& value)
        {
            payload.field("deviceName", value.device_name);
            payload.field("code", value.code);
        }

        void write_payload(json_object_builder& payload, const thread_create_payload& value)
        {
            payload.field("threadId", value.created_thread_id);
            payload.hex_field("startAddress", value.start_address);
            payload.hex_field("argument", value.argument);
            payload.array_field("flags", [&](const auto& emit) {
                for (const auto& flag : value.flags)
                {
                    emit([&](std::string& out) {
                        out += '"';
                        out += escape_json(flag);
                        out += '"';
                    });
                }
            });
        }

        void write_payload(json_object_builder& payload, const thread_terminated_payload& value)
        {
            payload.field("threadId", value.terminated_thread_id);
        }

        void write_payload(json_object_builder& payload, const thread_set_name_payload& value)
        {
            payload.field("threadId", value.renamed_thread_id);
            payload.field("name", value.name);
        }

        void write_payload(json_object_builder& payload, const thread_switch_payload& value)
        {
            payload.field("previousThreadId", value.previous_thread_id);
            payload.field("nextThreadId", value.next_thread_id);
        }

        void write_payload(json_object_builder& payload, const module_load_payload& value)
        {
            payload.field("path", value.path);
            payload.hex_field("imageBase", value.image_base);
        }

        void write_payload(json_object_builder& payload, const module_unload_payload& value)
        {
            payload.field("path", value.path);
            payload.hex_field("imageBase", value.image_base);
        }

        void write_payload(json_object_builder& payload, const import_read_payload& value)
        {
            payload.hex_field("resolvedAddress", value.resolved_address);
            payload.field("importName", value.import_name);
            payload.field("importModule", value.import_module);
        }

        void write_payload(json_object_builder& payload, const import_write_payload& value)
        {
            payload.field("size", static_cast<uint64_t>(value.size));
            payload.hex_field("value", value.value);
            payload.field("importName", value.import_name);
            payload.field("importModule", value.import_module);
        }

        void write_payload(json_object_builder& payload, const object_access_payload& value)
        {
            payload.field("mainAccess", value.main_access);
            payload.field("typeName", value.type_name);
            payload.hex_field("offset", value.offset);
            payload.hex_field("size", value.size);
            if (value.member_name.has_value())
            {
                payload.field("memberName", *value.member_name);
            }
        }

        void write_payload(json_object_builder& payload, const environment_access_payload& value)
        {
            payload.field("mainAccess", value.main_access);
            payload.hex_field("offset", value.offset);
            payload.hex_field("size", value.size);
        }

        void write_payload(json_object_builder& payload, const function_execution_payload& value)
        {
            payload.field("functionName", value.function_name);
            payload.field("interesting", value.interesting);
            payload.field("stubbedWinVerifyTrust", value.stubbed_win_verify_trust);
            payload.array_field("details", [&](const auto& emit) {
                for (const auto& detail : value.details)
                {
                    emit([&](std::string& out) {
                        json_object_builder detail_object{out};
                        detail_object.field("label", detail.label);
                        detail_object.field("value", detail.value);
                    });
                }
            });
        }

        void write_payload(json_object_builder& payload, const entry_point_execution_payload& value)
        {
            payload.field("interesting", value.interesting);
        }

        void write_payload(json_object_builder& payload, const foreign_code_transition_payload& value)
        {
            payload.field("functionName", value.function_name);
            payload.hex_field("functionOffset", value.function_offset);
            payload.field("interesting", value.interesting);
        }

        void write_payload(json_object_builder& payload, const section_first_execute_payload& value)
        {
            payload.field("moduleName", value.module_name);
            payload.field("sectionName", value.section_name);
            payload.hex_field("fileAddress", value.file_address);
        }

        void write_payload(json_object_builder&, const rdtsc_payload&)
        {
        }

        void write_payload(json_object_builder&, const rdtscp_payload&)
        {
        }

        void write_payload(json_object_builder& payload, const cpuid_payload& value)
        {
            payload.field("leaf", value.leaf);
        }

        void write_payload(json_object_builder& payload, const syscall_payload& value)
        {
            payload.field("classification", syscall_classification_name(value.classification));
            payload.field("syscallId", value.syscall_id);
            payload.field("syscallName", value.syscall_name);
            payload.optional_hex_field("callerRip", value.caller_rip);
            payload.optional_string_field("callerModule", value.caller_module);
        }

        void write_payload(json_object_builder& payload, const foreign_module_read_payload& value)
        {
            payload.hex_field("address", value.address);
            payload.field("size", static_cast<uint64_t>(value.size));
            payload.field("moduleName", value.module_name);
            payload.field("regionName", value.region_name);
        }

        void write_payload(json_object_builder& payload, const executable_read_payload& value)
        {
            payload.hex_field("address", value.address);
            payload.field("size", static_cast<uint64_t>(value.size));
            payload.field("sectionName", value.section_name);
        }

        void write_payload(json_object_builder& payload, const executable_write_payload& value)
        {
            payload.hex_field("address", value.address);
            payload.field("size", static_cast<uint64_t>(value.size));
            payload.hex_field("value", value.value);
            payload.field("sectionName", value.section_name);
        }
    };
}

std::unique_ptr<analysis_reporter> create_console_reporter(logger& log, const console_reporter_settings settings)
{
    return std::make_unique<console_analysis_reporter>(log, settings);
}

std::unique_ptr<analysis_reporter> create_jsonl_reporter(const std::filesystem::path& path)
{
    return std::make_unique<jsonl_analysis_reporter>(path);
}

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
        std::array<char, 32> buffer{};
        snprintf(buffer.data(), buffer.size(), "0x%" PRIx64, value);
        return buffer.data();
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
                    std::array<char, 8> buffer{};
                    snprintf(buffer.data(), buffer.size(), "\\u%04x", static_cast<unsigned char>(ch));
                    escaped += buffer.data();
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
                            this->log_.force_print(color::gray, "Using emulator backend: %s\n", e.backend_name.c_str());
                        }
                    },
                    [&](const run_finished_event& e) {
                        if (!this->settings_.silent && e.exit_status.has_value())
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
                        if (this->settings_.buffer_stdout && !this->settings_.silent && !e.data.empty())
                        {
                            this->log_.info("%.*s%s", static_cast<int>(e.data.size()), e.data.data(), e.data.ends_with("\n") ? "" : "\n");
                        }
                    },
                    [&](const stdout_chunk_event& e) {
                        if (this->settings_.silent)
                        {
                            (void)fwrite(e.data.data(), 1, e.data.size(), stdout);
                        }
                        else if (!this->settings_.buffer_stdout)
                        {
                            this->log_.info("%.*s%s", static_cast<int>(e.data.size()), e.data.data(), e.data.ends_with("\n") ? "" : "\n");
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
                                         "--> %s 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n", e.commit ? "Committed" : "Allocating", e.address,
                                         e.address + e.length, e.permissions.c_str());
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
                    [&](const io_control_event& e) { this->log_.print(color::dark_gray, "--> %s: 0x%X\n", e.device_name.c_str(), e.code); },
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
                    [&](const thread_switch_event& e) {
                        this->log_.print(color::dark_gray, "Performing thread switch: %X -> %X\n", e.previous_thread_id, e.next_thread_id);
                    },
                    [&](const module_load_event& e) { this->log_.log("Mapped %s at 0x%" PRIx64 "\n", e.path.c_str(), e.image_base); },
                    [&](const module_unload_event& e) { this->log_.log("Unmapping %s (0x%" PRIx64 ")\n", e.path.c_str(), e.image_base); },
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
                                         e.type_name.c_str(), e.offset, e.size, e.member_name.value_or("<N/A>").c_str(), e.execution.rip,
                                         e.execution.rip_module.c_str());
                    },
                    [&](const environment_access_event& e) {
                        this->log_.print(e.main_access ? color::green : color::dark_gray,
                                         "Environment access: 0x%" PRIx64 " (0x%zX) at 0x%" PRIx64 " (%s)\n", e.offset,
                                         static_cast<size_t>(e.size), e.execution.rip, e.execution.rip_module.c_str());
                    },
                    [&](const function_execution_event& e) {
                        this->log_.print(e.interesting ? color::yellow : color::dark_gray,
                                         "Executing function: %s (%s) (0x%" PRIx64 ") via 0x%" PRIx64 " (%s)\n", e.function_name.c_str(),
                                         e.execution.rip_module.c_str(), e.execution.rip, e.execution.previous_ip.value_or(0),
                                         e.execution.previous_ip_module.value_or("<N/A>").c_str());
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
                        this->log_.print(color::green, "Section %s (%s) first execute at 0x%" PRIx64 " 0x%" PRIx64 " (tid: %" PRIu32 ")\n",
                                         e.module_name.c_str(), e.section_name.c_str(), e.execution.rip, e.file_address,
                                         e.execution.thread_id);
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
                        switch (e.classification)
                        {
                        case syscall_classification::inline_syscall:
                            this->log_.print(color::blue, "Executing inline syscall: %s (0x%X) at 0x%" PRIx64 " (%s)\n",
                                             e.syscall_name.c_str(), e.syscall_id, e.execution.rip, e.execution.rip_module.c_str());
                            break;
                        case syscall_classification::crafted_out_of_line:
                            this->log_.print(color::blue,
                                             "Crafted out-of-line syscall: %s (0x%X) at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                                             e.syscall_name.c_str(), e.syscall_id, e.execution.rip, e.execution.rip_module.c_str(),
                                             e.execution.previous_ip.value_or(0), e.execution.previous_ip_module.value_or("<N/A>").c_str());
                            break;
                        case syscall_classification::regular:
                        default:
                            this->log_.print(color::dark_gray, "Executing syscall: %s (0x%X) at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                                             e.syscall_name.c_str(), e.syscall_id, e.execution.rip, e.caller_rip.value_or(0),
                                             e.caller_module.value_or("<N/A>").c_str());
                            break;
                        }
                    },
                    [&](const foreign_module_read_event& e) {
                        this->log_.print(color::pink, "Reading %zd bytes from module %s at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                                         e.size, e.module_name.c_str(), e.address, e.region_name.c_str(), e.execution.rip,
                                         e.execution.rip_module.c_str());
                    },
                    [&](const executable_read_event& e) {
                        this->log_.print(color::green,
                                         "Reading %zd bytes from executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n", e.size,
                                         e.section_name.c_str(), e.address, e.execution.rip, e.execution.rip_module.c_str());
                    },
                    [&](const executable_write_event& e) {
                        this->log_.print(
                            color::blue,
                            "Writing %zd bytes with value 0x%" PRIX64 " to executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                            e.size, e.value, e.section_name.c_str(), e.address, e.execution.rip, e.execution.rip_module.c_str());
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
            line.reserve(768);

            {
                json_object_builder object{line};
                std::visit(make_overloaded([&](const auto& e) { write_record(object, e); }), event);
            }

            this->file_ << line << '\n';
        }

        void flush() override
        {
            this->file_.flush();
        }

      private:
        std::ofstream file_{};

        static void write_record(json_object_builder& object, const run_started_event& event)
        {
            object.field("type", "header");
            object.field("schema", 1U);
            write_fields(object, event);
        }

        static void write_record(json_object_builder& object, const run_finished_event& event)
        {
            object.field("type", "footer");
            write_fields(object, event);
        }

        static void write_record(json_object_builder& object, const run_failed_event& event)
        {
            object.field("type", "footer");
            object.field("success", false);
            write_fields(object, event);
        }

        template <typename Event>
        static void write_record(json_object_builder& object, const Event& event)
        {
            static_assert(std::is_base_of_v<observation_event, Event> || std::is_base_of_v<summary_event, Event>);

            object.field("type", event_name(event));

            if constexpr (std::is_base_of_v<observation_event, Event>)
            {
                object.field("ic", event.header.instruction_count);
                object.field("tid", event.execution.thread_id);
                object.hex_field("rip", event.execution.rip);
                object.field("mod", event.execution.rip_module);
                object.optional_hex_field("prev", event.execution.previous_ip);
                object.optional_string_field("prevMod", event.execution.previous_ip_module);
            }

            write_fields(object, event);
        }

        template <typename Event>
        static std::string_view event_name(const Event&)
        {
            return "unknown";
        }

#define EVENT_NAME(TYPE, NAME)                      \
    static std::string_view event_name(const TYPE&) \
    {                                               \
        return NAME;                                \
    }

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

        static void write_fields(json_object_builder& object, const run_started_event& event)
        {
            object.field("backend", event.backend_name);
            object.field("mode", event.mode);
            if (!event.application.empty())
            {
                object.field("app", event.application);
            }

            object.array_field("args", [&](const auto& emit) {
                for (const auto& arg : event.arguments)
                {
                    emit([&](std::string& out) {
                        out += '"';
                        out += escape_json(arg);
                        out += '"';
                    });
                }
            });
        }

        static void write_fields(json_object_builder& object, const run_finished_event& event)
        {
            object.field("success", event.success);
            if (event.exit_status.has_value())
            {
                object.field("exit", *event.exit_status);
            }
        }

        static void write_fields(json_object_builder& object, const run_failed_event& event)
        {
            object.hex_field("rip", event.rip);
            object.field("error", event.message);
        }

        static void write_fields(json_object_builder& object, const instruction_summary_event& event)
        {
            object.array_field("entries", [&](const auto& emit) {
                for (const auto& entry : event.entries)
                {
                    emit([&](std::string& out) {
                        json_object_builder entry_object{out};
                        entry_object.field("mnemonic", entry.mnemonic);
                        entry_object.field("count", entry.count);
                    });
                }
            });
        }

        static void write_fields(json_object_builder& object, const buffered_stdout_event& event)
        {
            object.field("data", event.data);
        }

        static void write_fields(json_object_builder& object, const stdout_chunk_event& event)
        {
            object.field("data", event.data);
        }

        static void write_fields(json_object_builder& object, const suspicious_activity_event& event)
        {
            object.field("details", event.details);
            if (!event.decoded_instruction.empty())
            {
                object.field("inst", event.decoded_instruction);
            }
        }

        static void write_fields(json_object_builder& object, const debug_string_event& event)
        {
            object.field("details", event.details);
        }

        static void write_fields(json_object_builder& object, const generic_activity_event& event)
        {
            object.field("details", event.details);
        }

        static void write_fields(json_object_builder& object, const generic_access_event& event)
        {
            object.field("accessType", event.type);
            object.field("name", event.name);
        }

        static void write_fields(json_object_builder& object, const memory_allocate_event& event)
        {
            object.hex_field("addr", event.address);
            object.field("len", event.length);
            object.field("perms", event.permissions);
            object.field("commit", event.commit);
        }

        static void write_fields(json_object_builder& object, const memory_protect_event& event)
        {
            object.hex_field("addr", event.address);
            object.field("len", event.length);
            object.field("perms", event.permissions);
        }

        static void write_fields(json_object_builder& object, const memory_violation_event& event)
        {
            object.hex_field("addr", event.address);
            object.field("size", event.size);
            object.field("op", event.operation);
            object.field("violation", event.violation_type);
        }

        static void write_fields(json_object_builder& object, const io_control_event& event)
        {
            object.field("device", event.device_name);
            object.field("code", event.code);
        }

        static void write_fields(json_object_builder& object, const thread_create_event& event)
        {
            object.field("createdTid", event.created_thread_id);
            object.hex_field("start", event.start_address);
            object.hex_field("arg", event.argument);
            object.array_field("flags", [&](const auto& emit) {
                for (const auto& flag : event.flags)
                {
                    emit([&](std::string& out) {
                        out += '"';
                        out += escape_json(flag);
                        out += '"';
                    });
                }
            });
        }

        static void write_fields(json_object_builder& object, const thread_terminated_event& event)
        {
            object.field("terminatedTid", event.terminated_thread_id);
        }

        static void write_fields(json_object_builder& object, const thread_set_name_event& event)
        {
            object.field("namedTid", event.renamed_thread_id);
            object.field("name", event.name);
        }

        static void write_fields(json_object_builder& object, const thread_switch_event& event)
        {
            object.field("fromTid", event.previous_thread_id);
            object.field("toTid", event.next_thread_id);
        }

        static void write_fields(json_object_builder& object, const module_load_event& event)
        {
            object.field("path", event.path);
            object.hex_field("base", event.image_base);
        }

        static void write_fields(json_object_builder& object, const module_unload_event& event)
        {
            object.field("path", event.path);
            object.hex_field("base", event.image_base);
        }

        static void write_fields(json_object_builder& object, const import_read_event& event)
        {
            object.hex_field("target", event.resolved_address);
            object.field("import", event.import_name);
            object.field("importMod", event.import_module);
        }

        static void write_fields(json_object_builder& object, const import_write_event& event)
        {
            object.field("size", static_cast<uint64_t>(event.size));
            object.hex_field("value", event.value);
            object.field("import", event.import_name);
            object.field("importMod", event.import_module);
        }

        static void write_fields(json_object_builder& object, const object_access_event& event)
        {
            object.field("mainAccess", event.main_access);
            object.field("typeName", event.type_name);
            object.hex_field("offset", event.offset);
            object.hex_field("size", event.size);
            if (event.member_name.has_value())
            {
                object.field("member", *event.member_name);
            }
        }

        static void write_fields(json_object_builder& object, const environment_access_event& event)
        {
            object.field("mainAccess", event.main_access);
            object.hex_field("offset", event.offset);
            object.hex_field("size", event.size);
        }

        static void write_fields(json_object_builder& object, const function_execution_event& event)
        {
            object.field("fn", event.function_name);
            object.field("interesting", event.interesting);
            object.field("stubbedWinVerifyTrust", event.stubbed_win_verify_trust);
            object.array_field("details", [&](const auto& emit) {
                for (const auto& detail : event.details)
                {
                    emit([&](std::string& out) {
                        json_object_builder detail_object{out};
                        detail_object.field("label", detail.label);
                        detail_object.field("value", detail.value);
                    });
                }
            });
        }

        static void write_fields(json_object_builder& object, const entry_point_execution_event& event)
        {
            object.field("interesting", event.interesting);
        }

        static void write_fields(json_object_builder& object, const foreign_code_transition_event& event)
        {
            object.field("fn", event.function_name);
            object.hex_field("off", event.function_offset);
            object.field("interesting", event.interesting);
        }

        static void write_fields(json_object_builder& object, const section_first_execute_event& event)
        {
            object.field("moduleName", event.module_name);
            object.field("section", event.section_name);
            object.hex_field("fileAddr", event.file_address);
        }

        static void write_fields(json_object_builder&, const rdtsc_event&)
        {
        }

        static void write_fields(json_object_builder&, const rdtscp_event&)
        {
        }

        static void write_fields(json_object_builder& object, const cpuid_event& event)
        {
            object.field("leaf", event.leaf);
        }

        static void write_fields(json_object_builder& object, const syscall_event& event)
        {
            object.field("class", syscall_classification_name(event.classification));
            object.field("id", event.syscall_id);
            object.field("name", event.syscall_name);
            object.optional_hex_field("callerRip", event.caller_rip);
            object.optional_string_field("callerMod", event.caller_module);
        }

        static void write_fields(json_object_builder& object, const foreign_module_read_event& event)
        {
            object.hex_field("addr", event.address);
            object.field("size", static_cast<uint64_t>(event.size));
            object.field("moduleName", event.module_name);
            object.field("region", event.region_name);
        }

        static void write_fields(json_object_builder& object, const executable_read_event& event)
        {
            object.hex_field("addr", event.address);
            object.field("size", static_cast<uint64_t>(event.size));
            object.field("section", event.section_name);
        }

        static void write_fields(json_object_builder& object, const executable_write_event& event)
        {
            object.hex_field("addr", event.address);
            object.field("size", static_cast<uint64_t>(event.size));
            object.hex_field("value", event.value);
            object.field("section", event.section_name);
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

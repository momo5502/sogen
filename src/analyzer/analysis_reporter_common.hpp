#pragma once

#include "analysis_reporter.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace analysis_reporter_detail
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

    inline std::string hex_string(const uint64_t value)
    {
        std::array<char, 32> buffer{};
        snprintf(buffer.data(), buffer.size(), "0x%" PRIx64, value);
        return buffer.data();
    }

    inline std::string escape_json(std::string_view value)
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

    inline std::string_view syscall_classification_name(const syscall_classification classification)
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
}

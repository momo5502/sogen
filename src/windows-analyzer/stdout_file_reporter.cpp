#include "std_include.hpp"

#include "analysis_reporter.hpp"
#include "analysis_reporter_common.hpp"
#include "stdout_file_reporter.hpp"

namespace sogen
{

    namespace
    {
        using analysis_reporter_detail::make_overloaded;

        class stdout_file_analysis_reporter final : public analysis_reporter
        {
          public:
            explicit stdout_file_analysis_reporter(const std::filesystem::path& path)
            {
                this->file_.rdbuf()->pubsetbuf(this->buffer_.data(), static_cast<std::streamsize>(this->buffer_.size()));
                this->file_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);

                if (!this->file_)
                {
                    throw std::runtime_error("Failed to open guest console output file: " + path.string());
                }
            }

            void report(const analysis_event& event) override
            {
                std::visit(make_overloaded(
                               [this](const stdout_chunk_event& e) {
                                   this->file_.write(e.data.data(), static_cast<std::streamsize>(e.data.size()));
                                   if (e.data.find('\n') != std::string::npos)
                                   {
                                       this->file_.flush();
                                   }
                               },
                               [](const auto&) {}),
                           event);
            }

            void flush() override
            {
                this->file_.flush();
            }

          private:
            std::array<char, 64 * 1024> buffer_{};
            std::ofstream file_{};
        };
    }

    std::unique_ptr<analysis_reporter> create_stdout_file_reporter(const std::filesystem::path& path)
    {
        return std::make_unique<stdout_file_analysis_reporter>(path);
    }

} // namespace sogen

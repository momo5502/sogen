#include "symbol_download.hpp"

#include <utils/finally.hpp>

#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>

#ifndef __EMSCRIPTEN__
#ifdef _WIN32
#include <utils/win.hpp>
#endif
#include <curl/curl.h>
#endif

namespace sogen::symbols
{
#ifndef __EMSCRIPTEN__
    namespace
    {
        class curl_runtime
        {
          public:
            curl_runtime()
            {
                const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
                if (code != CURLE_OK)
                {
                    throw std::runtime_error(curl_easy_strerror(code));
                }
            }

            ~curl_runtime()
            {
                curl_global_cleanup();
            }

            curl_runtime(const curl_runtime&) = delete;
            curl_runtime& operator=(const curl_runtime&) = delete;
        };

        size_t write_download(char* data, const size_t size, const size_t count, void* context) noexcept
        {
            if (size != 0 && count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()) / size)
            {
                return 0;
            }
            const auto length = size * count;
            try
            {
                auto& stream = *static_cast<std::ofstream*>(context);
                stream.write(data, static_cast<std::streamsize>(length));
                return stream ? length : 0;
            }
            catch (...)
            {
                return 0;
            }
        }

        void set_curl_options(CURL* curl, const std::string& url, std::ofstream& output, std::array<char, CURL_ERROR_SIZE>& error)
        {
            const auto set_curl_option = [curl]<typename T>(const CURLoption option, const T value) {
                const auto code = curl_easy_setopt(curl, option, value);
                if (code != CURLE_OK)
                {
                    throw std::runtime_error(curl_easy_strerror(code));
                }
            };
            set_curl_option(CURLOPT_ERRORBUFFER, error.data());
            set_curl_option(CURLOPT_URL, url.c_str());
            set_curl_option(CURLOPT_PROTOCOLS_STR, "http,https");
            set_curl_option(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
            set_curl_option(CURLOPT_FOLLOWLOCATION, 1L);
            set_curl_option(CURLOPT_MAXREDIRS, 10L);
            set_curl_option(CURLOPT_FAILONERROR, 1L);
            set_curl_option(CURLOPT_CONNECTTIMEOUT, 10L);
            set_curl_option(CURLOPT_TIMEOUT, 120L);
            set_curl_option(CURLOPT_NOSIGNAL, 1L);
            set_curl_option(CURLOPT_WRITEFUNCTION, &write_download);
            set_curl_option(CURLOPT_WRITEDATA, &output);
        }
    }
#endif

    symbol_download_result download_symbol(const std::string& url, const std::filesystem::path& output_path)
    {
#ifdef __EMSCRIPTEN__
        (void)url;
        (void)output_path;
        return {.reason = "HTTP symbol downloads are unavailable in browser builds"};
#else
        try
        {
            static const curl_runtime runtime{};
            const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
            if (!curl)
            {
                return {.reason = "Failed to initialize HTTP download"};
            }

            std::ofstream output{output_path, std::ios::binary | std::ios::trunc};
            if (!output)
            {
                return {.reason = "Failed to open symbol download file"};
            }

            auto cleanup = utils::finally([&] {
                output.close();
                std::error_code ec{};
                std::filesystem::remove(output_path, ec);
            });

            std::array<char, CURL_ERROR_SIZE> error{};
            set_curl_options(curl.get(), url, output, error);

            const auto code = curl_easy_perform(curl.get());

            long status{}; // NOLINT(google-runtime-int): CURLINFO_RESPONSE_CODE requires long* on every platform.
            (void)curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);

            if (code != CURLE_OK)
            {
                if (status == 404)
                {
                    return {.reason = "symbol not found on server", .not_found = true};
                }
                return {.reason = error[0] ? error.data() : curl_easy_strerror(code)};
            }

            output.close();

            if (!output)
            {
                return {.reason = "Failed to write symbol download"};
            }

            cleanup.cancel();
            return {.success = true};
        }
        catch (const std::exception& e)
        {
            return {.reason = e.what()};
        }
#endif
    }
}

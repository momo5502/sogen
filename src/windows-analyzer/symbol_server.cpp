#include "symbol_server.hpp"
#include "pdb_file.hpp"
#include "symbol_cache.hpp"
#include "symbol_source.hpp"

#include <logger.hpp>
#include <utils/exec.hpp>
#include <utils/finally.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>

#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace sogen
{
    namespace
    {
        using namespace std::literals;

        constexpr auto k_default_symbol_server = "https://msdl.microsoft.com/download/symbols"sv;

        struct download_result
        {
            bool success{};
            std::string reason{};
            bool not_found{};
        };

        struct server_attempt
        {
            std::optional<symbol_server_result> result{};
            bool stop{};
        };

        bool pdb_validation_failed(const pdb_validation_status status)
        {
            return status == pdb_validation_status::invalid_artifact || status == pdb_validation_status::validation_error;
        }

        bool is_http_server(const std::string_view value)
        {
            return utils::string::starts_with_ignore_case(value, "http://"sv) ||
                   utils::string::starts_with_ignore_case(value, "https://"sv);
        }

        bool is_url_unreserved(const unsigned char value)
        {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '-' ||
                   value == '.' || value == '_' || value == '~';
        }

        std::string encode_url_segment(const std::string_view value)
        {
            constexpr auto hex = "0123456789ABCDEF"sv;
            std::string result{};
            result.reserve(value.size());
            for (const auto character : value)
            {
                const auto byte = static_cast<unsigned char>(character);
                if (is_url_unreserved(byte))
                {
                    result.push_back(character);
                    continue;
                }

                result.push_back('%');
                result.push_back(hex[(byte >> 4) & 0x0F]);
                result.push_back(hex[byte & 0x0F]);
            }
            return result;
        }

        std::string make_curl_url(const std::string& server, const std::filesystem::path& relative_path)
        {
            std::string result = server;
            while (!result.empty() && result.back() == '/')
            {
                result.pop_back();
            }

            for (const auto& component : relative_path)
            {
                result.push_back('/');
                result += encode_url_segment(component.string());
            }
            return result;
        }

        const std::optional<std::string>& decompressor_command()
        {
            static const std::optional<std::string> command = []() -> std::optional<std::string> {
#ifdef __EMSCRIPTEN__
                return std::nullopt;
#else
#ifdef _WIN32
                constexpr auto finder = "where";
                constexpr auto candidate = "expand.exe";
#else
                constexpr auto finder = "which";
                constexpr auto candidate = "cabextract";
#endif

                try
                {
                    if (!utils::string::trim(utils::exec::run_command_capture({finder, candidate})).empty())
                    {
                        return candidate;
                    }
                }
                catch (const std::exception&)
                {
                }
                return std::nullopt;
#endif
            }();
            return command;
        }

        download_result download_with_curl(const std::string& server, const std::filesystem::path& relative_path,
                                           const std::filesystem::path& output_path)
        {
            try
            {
                (void)utils::exec::run_command_capture({"curl", "-fsSL", "--connect-timeout", "10", "--max-time", "120", "-o",
                                                        output_path.string(), make_curl_url(server, relative_path)});
            }
            catch (const std::exception& e)
            {
                auto reason = std::string_view{e.what()};
                const auto not_found = reason.find("(22)") != std::string_view::npos && reason.find("404") != std::string_view::npos;
                const auto timeout = reason.find("(28)") != std::string_view::npos || reason.find("timed out") != std::string_view::npos ||
                                     reason.find("Timeout") != std::string_view::npos;
                if (not_found)
                {
                    reason = "symbol not found on server";
                }
                else if (timeout)
                {
                    reason = "download timed out";
                }
                return {.reason = std::string{reason}, .not_found = not_found};
            }

            if (!utils::io::file_exists(output_path))
            {
                return {.reason = "download finished but output file was not created"};
            }

            return {.success = true};
        }

        download_result decompress_pdb(const std::filesystem::path& compressed_path, const std::filesystem::path& output_path,
                                       const std::string& command)
        {
            std::error_code ec{};
            std::filesystem::remove(output_path, ec);
            try
            {
#ifdef _WIN32
                (void)utils::exec::run_command_capture({command, compressed_path.string(), output_path.string()});
#else
                const auto output = utils::exec::run_command_capture({command, "-q", "-p", compressed_path.string()});
                const auto bytes = std::as_bytes(std::span{output.data(), output.size()});
                if (!utils::io::write_file(output_path, bytes))
                {
                    return {.reason = "failed to write decompressed PDB"};
                }
#endif
            }
            catch (const std::exception& e)
            {
                std::filesystem::remove(output_path, ec);
                return {.reason = "failed to decompress symbol-store artifact: " + std::string{e.what()}};
            }

            return {.success = true};
        }

        server_attempt publish_download(const symbol_cache::download_slot& slot, const std::filesystem::path& path,
                                        const pdb_signature& sig, const std::string& server, const logger& log,
                                        const std::string_view module_name)
        {
            const auto validation = pdb_file::validate(path, sig);
            if (validation.status != pdb_validation_status::match)
            {
                if (validation.status == pdb_validation_status::mismatch)
                {
                    log.warn("Downloaded PDB for %.*s from %s did not match expected GUID and age\n", static_cast<int>(module_name.size()),
                             module_name.data(), server.c_str());
                }
                else
                {
                    log.warn("Downloaded symbol artifact for %.*s from %s was not a usable PDB: %s\n", static_cast<int>(module_name.size()),
                             module_name.data(), server.c_str(), validation.error.c_str());
                }
                return {.stop = validation.status == pdb_validation_status::validation_error};
            }

            const auto publication = symbol_cache::publish(slot, path, sig);
            if (!publication.success)
            {
                log.warn("Failed to cache downloaded PDB for %.*s from %s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                         server.c_str(), publication.reason.c_str());
                return {};
            }

            return {.result = symbol_server_result{
                        .path = publication.path,
                        .source = symbol_server_source::download,
                        .temporary = publication.temporary,
                    }};
        }

        server_attempt find_on_http_server(const std::string& server, const pdb_signature& sig,
                                           const std::filesystem::path& download_target, const logger& log,
                                           const std::string_view module_name)
        {
            std::error_code ec{};
            const auto slot = symbol_cache::acquire_download_slot(download_target, ec);
            if (!slot)
            {
                log.warn("Failed to create a PDB download file for %.*s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                         ec.message().c_str());
                return {};
            }

            auto cleanup = utils::finally([&] { symbol_cache::discard(*slot); });
            log.info("Downloading PDB symbols for %.*s from %s\n", static_cast<int>(module_name.size()), module_name.data(),
                     server.c_str());

            if (const auto& decompressor = decompressor_command())
            {
                const auto compressed_path = std::filesystem::path{slot->path.string() + ".compressed"};
                const auto expanded_path =
                    slot->temporary_directory ? slot->path : std::filesystem::path{slot->path.string() + ".expanded"};
                const auto compressed = download_with_curl(server, symbol_cache::store_relative_path(sig, true), compressed_path);
                if (compressed.success)
                {
                    const auto decompressed = decompress_pdb(compressed_path, expanded_path, *decompressor);
                    std::filesystem::remove(compressed_path, ec);
                    if (decompressed.success)
                    {
                        const auto result = publish_download(*slot, expanded_path, sig, server, log, module_name);
                        if (result.result)
                        {
                            if (result.result->temporary)
                            {
                                cleanup.cancel();
                            }
                            return result;
                        }
                        if (result.stop)
                        {
                            std::filesystem::remove(expanded_path, ec);
                            return result;
                        }
                    }
                    else
                    {
                        log.warn("Failed to decompress PDB for %.*s from %s, falling back to the uncompressed artifact: %s\n",
                                 static_cast<int>(module_name.size()), module_name.data(), server.c_str(), decompressed.reason.c_str());
                    }
                }
                std::filesystem::remove(compressed_path, ec);

                if (expanded_path != slot->path)
                {
                    std::filesystem::remove(expanded_path, ec);
                }
                else
                {
                    std::filesystem::remove(slot->path, ec);
                }
            }

            const auto download = download_with_curl(server, symbol_cache::store_relative_path(sig), slot->path);
            if (!download.success)
            {
                if (download.not_found)
                {
                    log.info("PDB symbols for %.*s were not found on %s\n", static_cast<int>(module_name.size()), module_name.data(),
                             server.c_str());
                }
                else
                {
                    log.warn("Failed to download PDB symbols for %.*s from %s: %s\n", static_cast<int>(module_name.size()),
                             module_name.data(), server.c_str(), download.reason.c_str());
                }
                return {};
            }

            const auto result = publish_download(*slot, slot->path, sig, server, log, module_name);
            if (result.result && result.result->temporary)
            {
                cleanup.cancel();
            }
            return result;
        }

        std::optional<symbol_server_result> find_in_symbol_store(const std::string& server, const pdb_signature& sig, const logger& log,
                                                                 const std::string_view module_name)
        {
            const auto path = symbol_cache::store_path(server, sig);
            if (!utils::io::file_exists(path))
            {
                return std::nullopt;
            }

            const auto validation = pdb_file::validate(path, sig);
            if (validation.status == pdb_validation_status::match)
            {
                return symbol_server_result{.path = path, .source = symbol_server_source::symbol_store};
            }
            if (pdb_validation_failed(validation.status))
            {
                log.warn("Failed to validate PDB for %.*s in symbol store %s: %s\n", static_cast<int>(module_name.size()),
                         module_name.data(), server.c_str(), validation.error.c_str());
            }
            return std::nullopt;
        }
    }

    std::optional<symbol_server_result> find_on_symbol_servers(const pdb_signature& sig, const pdb_symbol_options& options,
                                                               const logger& log, const std::string_view module_name)
    {
        const auto download_root = options.symbol_cache.value_or(symbol_cache::default_root());
        const auto target = symbol_cache::prepare_target(download_root, sig, log, module_name);
        if (target.status == symbol_cache::target_status::available)
        {
            return symbol_server_result{.path = target.path, .source = symbol_server_source::cache};
        }
        if (target.status == symbol_cache::target_status::unavailable)
        {
            return std::nullopt;
        }

        std::vector<std::string> servers = options.symbol_servers;
        servers.emplace_back(k_default_symbol_server);

        for (const auto& server : servers)
        {
            if (!is_http_server(server))
            {
                if (const auto result = find_in_symbol_store(server, sig, log, module_name))
                {
                    return result;
                }
                continue;
            }

            const auto attempt = find_on_http_server(server, sig, target.path, log, module_name);
            if (attempt.result)
            {
                return attempt.result;
            }
            if (attempt.stop)
            {
                return std::nullopt;
            }
        }

        return std::nullopt;
    }

} // namespace sogen

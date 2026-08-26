#include "pdb_symbol_source.hpp"
#include "pdb_file.hpp"

#include <windows_emulator.hpp>
#include <utils/exec.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <platform/win_pefile_debug.hpp>

#include <cstdlib>
#include <random>
#include <iomanip>
#include <span>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sogen
{
    namespace
    {
        constexpr auto k_default_symbol_server = "https://msdl.microsoft.com/download/symbols"sv;
        constexpr auto k_sogen_cache_marker = ".sogen-symbol-cache"sv;
        constexpr auto k_cache_max_age = std::chrono::hours{24 * 31};

        enum class pdb_resolution_source
        {
            explicit_path,
            recorded_path,
            sibling_path,
            cache,
            symbol_store,
            download,
        };

        struct resolved_pdb
        {
            std::filesystem::path path{};
            bool explicit_input{};
            pdb_resolution_source source{};
        };

        struct download_result
        {
            bool success{};
            std::filesystem::path path{};
            std::string reason{};
            bool not_found{};
        };

        struct symbol_store_candidate
        {
            bool two_tier{};
            bool compressed{};
        };

        bool pdb_validation_failed(const pdb_validation_status status)
        {
            return status == pdb_validation_status::invalid_artifact || status == pdb_validation_status::validation_error;
        }

        bool is_pdb_path(const std::filesystem::path& path)
        {
            return utils::string::equals_ignore_case(path.extension().string(), ".pdb"s);
        }

        bool is_http_server(const std::string_view value)
        {
            return utils::string::starts_with_ignore_case(value, "http://"sv) ||
                   utils::string::starts_with_ignore_case(value, "https://"sv);
        }

        const char* source_label(const pdb_resolution_source source)
        {
            switch (source)
            {
            case pdb_resolution_source::explicit_path:
                return "disk explicit PDB";
            case pdb_resolution_source::recorded_path:
                return "disk recorded PDB path";
            case pdb_resolution_source::sibling_path:
                return "disk sibling PDB";
            case pdb_resolution_source::cache:
                return "disk symbol cache";
            case pdb_resolution_source::symbol_store:
                return "disk symbol store";
            case pdb_resolution_source::download:
                return "downloaded PDB";
            }

            return "PDB";
        }

        std::string make_signature_key(const pdb_signature& sig)
        {
            std::ostringstream stream{};
            stream << winpe::detail::normalize_guid(sig.guid) << std::nouppercase << std::hex << sig.age;
            return stream.str();
        }

        std::filesystem::path default_sogen_cache_root()
        {
#ifdef _WIN32
            if (const auto* local_app_data = std::getenv("LOCALAPPDATA"))
            {
                return std::filesystem::path(local_app_data) / "sogen" / "symbols";
            }

            if (const auto* user_profile = std::getenv("USERPROFILE"))
            {
                return std::filesystem::path(user_profile) / "AppData" / "Local" / "sogen" / "symbols";
            }
#else
            if (const auto* xdg_cache = std::getenv("XDG_CACHE_HOME"))
            {
                return std::filesystem::path(xdg_cache) / "sogen" / "symbols";
            }

            if (const auto* home = std::getenv("HOME"))
            {
                return std::filesystem::path(home) / ".cache" / "sogen" / "symbols";
            }
#endif
            return std::filesystem::current_path() / ".sogen-symbols";
        }

        std::vector<std::string> split_symbol_path(const std::string_view value)
        {
            std::vector<std::string> entries{};
            size_t start = 0;

            for (size_t i = 0; i <= value.size(); ++i)
            {
                if (i != value.size() && value[i] != ';')
                {
                    continue;
                }

                const auto entry = utils::string::trim(value.substr(start, i - start));
                if (!entry.empty())
                {
                    entries.emplace_back(entry);
                }
                start = i + 1;
            }

            return entries;
        }

        void add_unique_path(std::vector<std::filesystem::path>& paths, std::filesystem::path path)
        {
            if (path.empty())
            {
                return;
            }

            std::error_code ec{};
            if (!std::filesystem::exists(path, ec) || ec)
            {
                return;
            }

            path = std::filesystem::absolute(path, ec);
            if (ec)
            {
                return;
            }

            for (const auto& existing : paths)
            {
                if (utils::string::equals_ignore_case(existing.string(), path.string()))
                {
                    return;
                }
            }

            paths.push_back(std::move(path));
        }

        void collect_nt_symbol_path_caches(std::vector<std::filesystem::path>& paths)
        {
            const auto* env = std::getenv("_NT_SYMBOL_PATH");
            if (!env)
            {
                return;
            }

            for (const auto& entry : split_symbol_path(env))
            {
                if (utils::string::starts_with_ignore_case(std::string_view{entry}, "cache*"sv))
                {
                    add_unique_path(paths, entry.substr(6));
                    continue;
                }

                if (utils::string::starts_with_ignore_case(std::string_view{entry}, "srv*"sv))
                {
                    const auto first = entry.find('*');
                    if (first == std::string::npos)
                    {
                        continue;
                    }

                    const auto second = entry.find('*', first + 1);
                    if (second == std::string::npos)
                    {
                        continue;
                    }

                    const auto cache = entry.substr(first + 1, second - first - 1);
                    if (!cache.empty() && !is_http_server(cache))
                    {
                        add_unique_path(paths, cache);
                    }
                    continue;
                }

                if (!is_http_server(entry))
                {
                    add_unique_path(paths, entry);
                }
            }
        }

        std::vector<std::filesystem::path> existing_cache_roots(const std::optional<std::filesystem::path>& explicit_cache)
        {
            std::vector<std::filesystem::path> paths{};

            if (explicit_cache)
            {
                add_unique_path(paths, *explicit_cache);
            }

            collect_nt_symbol_path_caches(paths);

#ifdef _WIN32
            add_unique_path(paths, "C:\\Symbols");
            add_unique_path(paths, "C:\\SymCache");
#endif

            add_unique_path(paths, default_sogen_cache_root());
            return paths;
        }

        std::filesystem::path symbol_store_relative_path(const pdb_signature& sig, const symbol_store_candidate candidate = {})
        {
            const std::filesystem::path pdb_name{sig.filename()};
            auto stored_name = pdb_name;
            if (candidate.compressed)
            {
                auto name = stored_name.string();
                if (!name.empty())
                {
                    name.back() = '_';
                }
                stored_name = std::move(name);
            }

            auto path = pdb_name / make_signature_key(sig) / stored_name;
            if (candidate.two_tier)
            {
                const auto name = pdb_name.string();
                path = std::filesystem::path{name.substr(0, std::min<size_t>(2, name.size()))} / path;
            }
            return path;
        }

        std::filesystem::path symbol_store_path(const std::filesystem::path& root, const pdb_signature& sig)
        {
            return root / symbol_store_relative_path(sig, {.two_tier = utils::io::file_exists(root / "index2.txt")});
        }

        std::optional<resolved_pdb> find_in_caches(const pdb_signature& sig, const std::optional<std::filesystem::path>& explicit_cache,
                                                   const logger& log, const std::string_view module_name)
        {
            for (const auto& root : existing_cache_roots(explicit_cache))
            {
                const auto path = symbol_store_path(root, sig);
                if (!utils::io::file_exists(path))
                {
                    continue;
                }

                const auto validation = pdb_file::validate(path, sig);
                if (validation.status == pdb_validation_status::match)
                {
                    return resolved_pdb{
                        .path = path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::cache,
                    };
                }
                if (pdb_validation_failed(validation.status))
                {
                    log.warn("Failed to validate cached PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                             path.string().c_str(), validation.error.c_str());
                }
            }

            return std::nullopt;
        }

        std::string join_url(const std::string& server, const std::filesystem::path& path)
        {
            std::string result = server;
            while (!result.empty() && result.back() == '/')
            {
                result.pop_back();
            }

            result += '/';
            result += path.generic_string();
            return result;
        }

        std::optional<std::filesystem::path> create_temporary_download_path(const std::filesystem::path& target, std::error_code& ec)
        {
            std::random_device random{};
            for (size_t attempt = 0; attempt < 32; ++attempt)
            {
                std::ostringstream suffix{};
                suffix << ".tmp." << std::hex << random() << random();
                std::filesystem::path path = target.string() + suffix.str();

#ifdef _WIN32
                const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
                if (handle != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(handle);
                    return path;
                }

                const auto error = GetLastError();
                if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                {
                    ec = std::error_code{static_cast<int>(error), std::system_category()};
                    return std::nullopt;
                }
#else
                const auto descriptor = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
                if (descriptor >= 0)
                {
                    close(descriptor);
                    return path;
                }

                if (errno != EEXIST)
                {
                    ec = std::error_code{errno, std::generic_category()};
                    return std::nullopt;
                }
#endif
            }

            ec = std::make_error_code(std::errc::file_exists);
            return std::nullopt;
        }

        download_result download_with_curl(const std::string& url, const std::filesystem::path& target)
        {
            std::error_code ec{};
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec)
            {
                return {.reason = "failed to create cache directory: " + ec.message()};
            }

            const auto temp = create_temporary_download_path(target, ec);
            if (!temp)
            {
                return {.reason = "failed to create temporary download: " + ec.message()};
            }

            try
            {
                (void)utils::exec::run_command_capture(
                    {"curl", "-fsSL", "--connect-timeout", "10", "--max-time", "120", "-o", temp->string(), url});
            }
            catch (const std::exception& e)
            {
                std::filesystem::remove(*temp, ec);
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

            if (!utils::io::file_exists(*temp))
            {
                return {.reason = "download finished but temporary file was not created"};
            }

            return {.success = true, .path = *temp};
        }

        download_result decompress_pdb(const std::filesystem::path& compressed_path, const std::filesystem::path& target)
        {
            std::error_code ec{};
            std::filesystem::create_directories(target.parent_path(), ec);
            if (ec)
            {
                return {.reason = "failed to create cache directory: " + ec.message()};
            }

            const auto output_path = create_temporary_download_path(target, ec);
            if (!output_path)
            {
                return {.reason = "failed to create temporary decompression output: " + ec.message()};
            }

            try
            {
#ifdef _WIN32
                std::filesystem::remove(*output_path, ec);
                (void)utils::exec::run_command_capture({"expand", compressed_path.string(), output_path->string()});
#else
                const auto output = utils::exec::run_command_capture({"cabextract", "-q", "-p", compressed_path.string()});
                const auto bytes = std::as_bytes(std::span{output.data(), output.size()});
                if (!utils::io::write_file(*output_path, bytes))
                {
                    std::filesystem::remove(*output_path, ec);
                    return {.reason = "failed to write decompressed PDB"};
                }
#endif
            }
            catch (const std::exception& e)
            {
                std::filesystem::remove(*output_path, ec);
                return {.reason = "failed to decompress symbol-store artifact: " + std::string{e.what()}};
            }

            return {.success = true, .path = *output_path};
        }

        download_result promote_download(const std::filesystem::path& temporary_path, const std::filesystem::path& target,
                                         const pdb_signature& expected)
        {
            for (size_t attempt = 0; attempt < 2; ++attempt)
            {
                std::error_code ec{};
                std::filesystem::create_hard_link(temporary_path, target, ec);
                if (!ec)
                {
                    std::filesystem::remove(temporary_path, ec);
                    return {.success = true, .path = target};
                }

                if (!utils::io::file_exists(target))
                {
                    return {.reason = "failed to promote downloaded PDB into cache: " + ec.message()};
                }

                const auto validation = pdb_file::validate(target, expected);
                if (validation.status == pdb_validation_status::match)
                {
                    std::filesystem::remove(temporary_path, ec);
                    return {.success = true, .path = target};
                }
                if (validation.status == pdb_validation_status::validation_error)
                {
                    return {.reason = "failed to validate concurrently cached PDB: " + validation.error};
                }

                std::filesystem::remove(target, ec);
                if (ec)
                {
                    return {.reason = "failed to remove invalid concurrently cached PDB: " + ec.message()};
                }
            }

            return {.reason = "failed to promote downloaded PDB because the cache entry kept changing"};
        }

        std::optional<resolved_pdb> find_on_symbol_servers(const pdb_signature& sig, const pdb_symbol_options& options, const logger& log,
                                                           const std::string_view module_name)
        {
            auto download_root = options.symbol_cache.value_or(default_sogen_cache_root());
            const auto download_target = symbol_store_path(download_root, sig);
            if (utils::io::file_exists(download_target))
            {
                const auto validation = pdb_file::validate(download_target, sig);
                if (validation.status == pdb_validation_status::match)
                {
                    return resolved_pdb{
                        .path = download_target,
                        .explicit_input = false,
                        .source = pdb_resolution_source::cache,
                    };
                }
                if (pdb_validation_failed(validation.status))
                {
                    log.warn("Failed to validate cached PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                             download_target.string().c_str(), validation.error.c_str());
                    if (validation.status == pdb_validation_status::validation_error)
                    {
                        return std::nullopt;
                    }
                }

                log.warn("Removing unusable cached PDB for %.*s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                         download_target.string().c_str());
                std::error_code ec{};
                std::filesystem::remove(download_target, ec);
                if (ec)
                {
                    log.warn("Failed to remove unusable cached PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()),
                             module_name.data(), download_target.string().c_str(), ec.message().c_str());
                    return std::nullopt;
                }
            }

            std::vector<std::string> servers = options.symbol_servers;
            servers.emplace_back(k_default_symbol_server);

            for (const auto& server : servers)
            {
                if (is_http_server(server))
                {
                    log.info("Downloading PDB symbols for %.*s from %s\n", static_cast<int>(module_name.size()), module_name.data(),
                             server.c_str());
                    download_result download{};
                    constexpr std::array candidates{
                        symbol_store_candidate{},
                        symbol_store_candidate{.compressed = true},
                        symbol_store_candidate{.two_tier = true},
                        symbol_store_candidate{.two_tier = true, .compressed = true},
                    };
                    for (const auto candidate : candidates)
                    {
                        download = download_with_curl(join_url(server, symbol_store_relative_path(sig, candidate)), download_target);
                        if (download.success && candidate.compressed)
                        {
                            auto decompressed = decompress_pdb(download.path, download_target);
                            std::error_code ec{};
                            std::filesystem::remove(download.path, ec);
                            download = std::move(decompressed);
                        }

                        if (download.success || !download.not_found)
                        {
                            break;
                        }
                    }
                    if (download.success)
                    {
                        const auto& temporary_path = download.path;
                        const auto validation = pdb_file::validate(temporary_path, sig);
                        if (validation.status == pdb_validation_status::match)
                        {
                            const auto promotion = promote_download(temporary_path, download_target, sig);
                            if (promotion.success)
                            {
                                return resolved_pdb{
                                    .path = download_target,
                                    .explicit_input = false,
                                    .source = pdb_resolution_source::download,
                                };
                            }

                            log.warn("Failed to cache downloaded PDB for %.*s from %s: %s\n", static_cast<int>(module_name.size()),
                                     module_name.data(), server.c_str(), promotion.reason.c_str());
                            std::error_code ec{};
                            std::filesystem::remove(temporary_path, ec);
                            continue;
                        }
                        if (validation.status == pdb_validation_status::validation_error)
                        {
                            log.warn("Failed to validate downloaded PDB for %.*s from %s: %s\n", static_cast<int>(module_name.size()),
                                     module_name.data(), server.c_str(), validation.error.c_str());
                            std::error_code ec{};
                            std::filesystem::remove(temporary_path, ec);
                            return std::nullopt;
                        }

                        if (validation.status == pdb_validation_status::mismatch)
                        {
                            log.warn("Downloaded PDB for %.*s from %s did not match expected GUID and age\n",
                                     static_cast<int>(module_name.size()), module_name.data(), server.c_str());
                        }
                        else
                        {
                            log.warn("Downloaded symbol artifact for %.*s from %s was not a usable PDB: %s\n",
                                     static_cast<int>(module_name.size()), module_name.data(), server.c_str(), validation.error.c_str());
                        }
                        std::error_code ec{};
                        std::filesystem::remove(temporary_path, ec);
                        continue;
                    }

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
                    continue;
                }

                const std::filesystem::path root{server};
                auto path = symbol_store_path(root, sig);
                bool temporary = false;
                if (!utils::io::file_exists(path))
                {
                    const auto compressed_path =
                        root /
                        symbol_store_relative_path(sig, {.two_tier = utils::io::file_exists(root / "index2.txt"), .compressed = true});
                    if (!utils::io::file_exists(compressed_path))
                    {
                        continue;
                    }

                    const auto decompressed = decompress_pdb(compressed_path, download_target);
                    if (!decompressed.success)
                    {
                        log.warn("Failed to decompress PDB for %.*s in symbol store %s: %s\n", static_cast<int>(module_name.size()),
                                 module_name.data(), server.c_str(), decompressed.reason.c_str());
                        continue;
                    }
                    path = decompressed.path;
                    temporary = true;
                }

                const auto validation = pdb_file::validate(path, sig);
                if (validation.status == pdb_validation_status::match)
                {
                    if (temporary)
                    {
                        const auto promotion = promote_download(path, download_target, sig);
                        if (!promotion.success)
                        {
                            std::error_code ec{};
                            std::filesystem::remove(path, ec);
                            log.warn("Failed to cache decompressed PDB for %.*s from %s: %s\n", static_cast<int>(module_name.size()),
                                     module_name.data(), server.c_str(), promotion.reason.c_str());
                            continue;
                        }
                        path = promotion.path;
                    }

                    return resolved_pdb{
                        .path = path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::symbol_store,
                    };
                }

                if (temporary)
                {
                    std::error_code ec{};
                    std::filesystem::remove(path, ec);
                }
                if (pdb_validation_failed(validation.status))
                {
                    log.warn("Failed to validate PDB for %.*s in symbol store %s: %s\n", static_cast<int>(module_name.size()),
                             module_name.data(), server.c_str(), validation.error.c_str());
                }
            }

            return std::nullopt;
        }

        void ensure_default_cache_marker()
        {
            const auto root = default_sogen_cache_root();
            std::error_code ec{};
            std::filesystem::create_directories(root, ec);
            if (ec)
            {
                return;
            }

            const auto marker = root / k_sogen_cache_marker;
            if (!utils::io::file_exists(marker))
            {
                (void)utils::io::write_file(marker, std::span<const std::byte>{});
            }
        }

#ifdef _WIN32
        std::optional<std::chrono::system_clock::time_point> last_access_time(const std::filesystem::path& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &data))
            {
                return std::nullopt;
            }

            ULARGE_INTEGER value{};
            value.LowPart = data.ftLastAccessTime.dwLowDateTime;
            value.HighPart = data.ftLastAccessTime.dwHighDateTime;
            constexpr uint64_t windows_to_unix_epoch_100ns = 116444736000000000ull;
            if (value.QuadPart < windows_to_unix_epoch_100ns)
            {
                return std::nullopt;
            }

            const auto ns = (value.QuadPart - windows_to_unix_epoch_100ns) * 100;
            return std::chrono::system_clock::time_point{
                std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{ns})};
        }
#else
        std::optional<std::chrono::system_clock::time_point> last_access_time(const std::filesystem::path& path)
        {
            struct stat st{};
            if (stat(path.string().c_str(), &st) != 0)
            {
                return std::nullopt;
            }

            return std::chrono::system_clock::from_time_t(st.st_atime);
        }
#endif

        void prune_sogen_cache(const logger& log, const std::optional<std::filesystem::path>& explicit_cache)
        {
            if (explicit_cache)
            {
                return;
            }

            const auto root = default_sogen_cache_root();
            if (!utils::io::file_exists(root / k_sogen_cache_marker))
            {
                return;
            }

            const auto cutoff = std::chrono::system_clock::now() - k_cache_max_age;
            std::error_code ec{};
            if (!std::filesystem::is_directory(root, ec) || ec)
            {
                return;
            }

            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
            {
                if (ec || entry.path().filename() == k_sogen_cache_marker)
                {
                    continue;
                }

                const auto access_time = last_access_time(entry.path());
                if (!access_time || *access_time >= cutoff)
                {
                    continue;
                }

                if (entry.is_regular_file(ec) && !ec)
                {
                    std::filesystem::remove(entry.path(), ec);
                }
            }

            std::vector<std::filesystem::path> directories{};
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
            {
                if (!ec && entry.is_directory(ec) && !ec)
                {
                    directories.push_back(entry.path());
                }
            }

            std::ranges::sort(directories, [](const auto& lhs, const auto& rhs) { return lhs.string().size() > rhs.string().size(); });
            for (const auto& directory : directories)
            {
                if (utils::io::directory_is_empty(directory))
                {
                    std::filesystem::remove(directory, ec);
                }
            }

            if (ec)
            {
                log.warn("PDB cache cleanup encountered an error: %s\n", ec.message().c_str());
            }
        }

        std::optional<resolved_pdb> resolve_from_signature(const pdb_signature& sig, const pdb_symbol_options& options, const logger& log,
                                                           const std::string_view module_name,
                                                           const std::optional<std::filesystem::path>& module_pe_path = std::nullopt)
        {
            if (!sig.valid() || sig.path.empty())
            {
                return std::nullopt;
            }

            const std::filesystem::path recorded_path{sig.path};
            if (recorded_path.is_absolute() && utils::io::file_exists(recorded_path))
            {
                const auto validation = pdb_file::validate(recorded_path, sig);
                if (validation.status == pdb_validation_status::match)
                {
                    return resolved_pdb{
                        .path = recorded_path,
                        .explicit_input = false,
                        .source = pdb_resolution_source::recorded_path,
                    };
                }
                if (pdb_validation_failed(validation.status))
                {
                    log.warn("Failed to validate recorded PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()),
                             module_name.data(), recorded_path.string().c_str(), validation.error.c_str());
                }
            }

            if (module_pe_path && !module_pe_path->empty())
            {
                std::vector<std::filesystem::path> sibling_candidates{};

                auto sibling_pdb = *module_pe_path;
                sibling_pdb.replace_extension(".pdb");
                sibling_candidates.push_back(std::move(sibling_pdb));

                if (!sig.filename().empty())
                {
                    sibling_candidates.push_back(module_pe_path->parent_path() / sig.filename());
                }

                for (const auto& candidate : sibling_candidates)
                {
                    if (!utils::io::file_exists(candidate))
                    {
                        continue;
                    }

                    const auto validation = pdb_file::validate(candidate, sig);
                    if (validation.status == pdb_validation_status::mismatch)
                    {
                        continue;
                    }
                    if (pdb_validation_failed(validation.status))
                    {
                        log.warn("Failed to validate sibling PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()),
                                 module_name.data(), candidate.string().c_str(), validation.error.c_str());
                        continue;
                    }

                    return resolved_pdb{
                        .path = candidate,
                        .explicit_input = false,
                        .source = pdb_resolution_source::sibling_path,
                    };
                }
            }

            if (const auto cached = find_in_caches(sig, options.symbol_cache, log, module_name))
            {
                return cached;
            }

            return find_on_symbol_servers(sig, options, log, module_name);
        }

        std::optional<resolved_pdb> resolve_pe_input(const std::filesystem::path& pe_path, const pdb_symbol_options& options,
                                                     const logger& log)
        {
            auto sig = winpe::read_pdb_signature(pe_path);
            if (!sig)
            {
                throw std::runtime_error("No RSDS PDB reference found in " + pe_path.string());
            }

            std::vector<std::pair<std::filesystem::path, pdb_resolution_source>> direct_candidates{};
            const std::filesystem::path recorded_path{sig->path};
            if (recorded_path.is_absolute())
            {
                direct_candidates.emplace_back(recorded_path, pdb_resolution_source::recorded_path);
            }

            auto sibling_pdb = pe_path;
            sibling_pdb.replace_extension(".pdb");
            direct_candidates.emplace_back(std::move(sibling_pdb), pdb_resolution_source::sibling_path);
            if (!sig->filename().empty())
            {
                direct_candidates.emplace_back(pe_path.parent_path() / sig->filename(), pdb_resolution_source::sibling_path);
            }

            for (const auto& [candidate, source] : direct_candidates)
            {
                if (!utils::io::file_exists(candidate))
                {
                    continue;
                }

                const auto parsed = pdb_file::read(candidate);
                if (!pdb_file::signatures_match(parsed.signature, *sig))
                {
                    throw std::runtime_error("PDB signature mismatch for " + candidate.string());
                }

                return resolved_pdb{
                    .path = candidate,
                    .explicit_input = true,
                    .source = source,
                };
            }

            if (const auto resolved = resolve_from_signature(*sig, options, log, pe_path.filename().string(), pe_path))
            {
                auto pdb = *resolved;
                pdb.explicit_input = true;
                return pdb;
            }

            return std::nullopt;
        }

        void merge_symbols(loaded_symbols& result, const mapped_module& mod, const pdb_file& pdb)
        {
            for (const auto& [address, name] : pdb.symbols)
            {
                if (const auto rva = symbol_address_to_rva(mod, address.first, address.second))
                {
                    result.address_names.try_emplace(*rva, name);
                }
            }
        }

        bool matches_module(const mapped_module& mod, const pdb_file& pdb)
        {
            return mod.pdb && pdb_file::signatures_match(*mod.pdb, pdb.signature);
        }

    }

    pdb_symbol_source::pdb_symbol_source(windows_emulator& win_emu, pdb_symbol_options options)
        : win_emu_(&win_emu),
          options_(std::move(options))
    {
        if (!this->options_.symbol_cache)
        {
            ensure_default_cache_marker();
        }
        prune_sogen_cache(win_emu.log, this->options_.symbol_cache);
    }

    loaded_symbols pdb_symbol_source::load(const mapped_module& mod) const
    {
        loaded_symbols result{};
        if (!this->options_.enabled())
        {
            return result;
        }

        std::vector<resolved_pdb> candidates{};

        for (const auto& input : this->options_.inputs)
        {
            if (is_pdb_path(input))
            {
                candidates.emplace_back(resolved_pdb{
                    .path = input,
                    .explicit_input = true,
                    .source = pdb_resolution_source::explicit_path,
                });
                continue;
            }

            if (auto resolved = resolve_pe_input(input, this->options_, this->win_emu_->log))
            {
                candidates.push_back(std::move(*resolved));
            }
        }

        if (this->options_.auto_lookup && mod.pdb)
        {
            const auto module_pe_path =
                (!mod.path.empty() && utils::io::file_exists(mod.path)) ? std::optional<std::filesystem::path>{mod.path} : std::nullopt;

            if (const auto resolved = resolve_from_signature(*mod.pdb, this->options_, this->win_emu_->log, mod.name, module_pe_path))
            {
                candidates.emplace_back(*resolved);
            }
            else
            {
                this->win_emu_->log.warn("No usable PDB found for %s in symbol caches or symbol servers\n", mod.name.c_str());
            }
        }
        else if (this->options_.auto_lookup)
        {
            this->win_emu_->log.info("Skipping automatic PDB lookup for %s: no RSDS reference\n", mod.name.c_str());
        }

        if (candidates.empty())
        {
            this->win_emu_->log.info("No PDB candidates for %s\n", mod.name.c_str());
        }

        std::set<std::filesystem::path> loaded_candidates{};
        for (const auto& candidate : candidates)
        {
            if (!loaded_candidates.insert(candidate.path).second)
            {
                continue;
            }

            this->win_emu_->log.info("Loading PDB symbols for %s from %s: %s\n", mod.name.c_str(), source_label(candidate.source),
                                     candidate.path.string().c_str());

            pdb_file pdb{};
            try
            {
                pdb = pdb_file::read(candidate.path);
            }
            catch (const std::exception& e)
            {
                if (candidate.explicit_input)
                {
                    throw;
                }

                this->win_emu_->log.warn("Failed to parse PDB %s: %s\n", candidate.path.string().c_str(), e.what());
                continue;
            }

            if (!matches_module(mod, pdb))
            {
                const auto candidate_filename = candidate.path.filename().string();
                const auto same_pdb_name =
                    mod.pdb && utils::string::equals_ignore_case(mod.pdb->filename(), std::string_view{candidate_filename});
                if (candidate.explicit_input && same_pdb_name)
                {
                    throw std::runtime_error("PDB signature mismatch for " + candidate.path.string() + " and module " + mod.name);
                }

                this->win_emu_->log.info("Skipping PDB %s for %s: signature mismatch\n", candidate.path.string().c_str(), mod.name.c_str());
                continue;
            }

            merge_symbols(result, mod, pdb);
            this->win_emu_->log.info("Loaded PDB symbols for %s from %s\n", mod.name.c_str(), candidate.path.string().c_str());
        }

        return result;
    }

} // namespace sogen

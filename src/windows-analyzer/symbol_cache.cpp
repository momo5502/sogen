#include "symbol_cache.hpp"
#include "pdb_file.hpp"

#include <logger.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <platform/platform.hpp>
#include <platform/win_pefile_debug.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <span>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#ifndef __EMSCRIPTEN__
#include <sys/file.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sogen
{
    using namespace std::literals;

    namespace
    {
        constexpr auto k_sogen_cache_marker = ".sogen-symbol-cache"sv;
        constexpr auto k_cache_max_age = std::chrono::hours{24 * 31};

        bool is_http_server(const std::string_view value)
        {
            return utils::string::starts_with_ignore_case(value, "http://"sv) ||
                   utils::string::starts_with_ignore_case(value, "https://"sv);
        }

        std::string make_signature_key(const pdb_signature& sig)
        {
            std::ostringstream stream{};
            stream << winpe::detail::normalize_guid(sig.guid) << std::nouppercase << std::hex << sig.age;
            return stream.str();
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

        std::vector<std::filesystem::path> existing_roots(const std::optional<std::filesystem::path>& explicit_cache)
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

            add_unique_path(paths, symbol_cache::default_root());
            return paths;
        }

#ifdef _WIN32
        std::optional<std::wstring> make_download_mutex_name(const std::filesystem::path& path, std::error_code& ec)
        {
            auto normalized = std::filesystem::absolute(path, ec).lexically_normal().wstring();
            if (ec)
            {
                return std::nullopt;
            }

            CharUpperBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));

            constexpr uint64_t fnv_offset = 14695981039346656037ull;
            constexpr uint64_t fnv_prime = 1099511628211ull;
            auto hash = fnv_offset;
            for (const auto character : normalized)
            {
                hash ^= static_cast<uint16_t>(character);
                hash *= fnv_prime;
            }

            std::wostringstream stream{};
            stream << L"Global\\sogen-pdb-download-" << std::hex << std::setfill(L'0') << std::setw(16) << hash;
            return stream.str();
        }
#endif

        std::optional<symbol_cache::download_lock> acquire_lock(const std::filesystem::path& path, std::error_code& ec)
        {
#ifdef _WIN32
            const auto mutex_name = make_download_mutex_name(path, ec);
            if (!mutex_name)
            {
                return std::nullopt;
            }

            const auto mutex = CreateMutexW(nullptr, FALSE, mutex_name->c_str());
            if (!mutex)
            {
                ec = std::error_code{static_cast<int>(GetLastError()), std::system_category()};
                return std::nullopt;
            }

            const auto wait_result = WaitForSingleObject(mutex, 0);
            if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED)
            {
                ec = wait_result == WAIT_TIMEOUT ? std::make_error_code(std::errc::device_or_resource_busy)
                                                 : std::error_code{static_cast<int>(GetLastError()), std::system_category()};
                CloseHandle(mutex);
                return std::nullopt;
            }

            symbol_cache::download_lock lock{reinterpret_cast<std::intptr_t>(mutex)};
            const auto file =
                CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                ec = std::error_code{static_cast<int>(GetLastError()), std::system_category()};
                return std::nullopt;
            }
            CloseHandle(file);

            return lock;
#elif defined(__EMSCRIPTEN__)
            ec = std::make_error_code(std::errc::operation_not_supported);
            return std::nullopt;
#else
            const auto descriptor = open(path.c_str(), O_CREAT | O_RDWR, 0600);
            if (descriptor < 0)
            {
                ec = std::error_code{errno, std::generic_category()};
                return std::nullopt;
            }

            if (flock(descriptor, LOCK_EX | LOCK_NB) != 0 || ftruncate(descriptor, 0) != 0)
            {
                ec = std::error_code{errno, std::generic_category()};
                close(descriptor);
                return std::nullopt;
            }

            return symbol_cache::download_lock{descriptor};
#endif
        }

        std::optional<std::filesystem::path> create_private_temporary_directory(std::error_code& ec)
        {
            const auto root = std::filesystem::temp_directory_path(ec);
            if (ec)
            {
                return std::nullopt;
            }

#ifdef _WIN32
            static std::atomic_uint64_t next_directory{};
            for (size_t attempt = 0; attempt < 128; ++attempt)
            {
                auto directory = root / ("sogen-symbols-" + std::to_string(GetCurrentProcessId()) + "-" +
                                         std::to_string(next_directory.fetch_add(1, std::memory_order_relaxed)));
                ec.clear();
                if (std::filesystem::create_directory(directory, ec))
                {
                    return directory;
                }
                if (ec && ec != std::errc::file_exists)
                {
                    return std::nullopt;
                }
            }

            ec = std::make_error_code(std::errc::file_exists);
            return std::nullopt;
#else
            auto pattern = (root / "sogen-symbols-XXXXXX").string();
            std::vector<char> buffer(pattern.begin(), pattern.end());
            buffer.push_back('\0');
            const auto* directory = mkdtemp(buffer.data());
            if (!directory)
            {
                ec = std::error_code{errno, std::generic_category()};
                return std::nullopt;
            }
            return std::filesystem::path{directory};
#endif
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

        void ensure_default_marker()
        {
            const auto root = symbol_cache::default_root();
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

        void prune(const logger& log, const std::optional<std::filesystem::path>& explicit_cache)
        {
            if (explicit_cache)
            {
                return;
            }

            const auto root = symbol_cache::default_root();
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
    }

    symbol_cache::download_lock::download_lock(const std::intptr_t native_handle)
        : native_handle_(native_handle)
    {
    }

    symbol_cache::download_lock::~download_lock()
    {
        release();
    }

    symbol_cache::download_lock::download_lock(download_lock&& other) noexcept
        : native_handle_(std::exchange(other.native_handle_, -1))
    {
    }

    symbol_cache::download_lock& symbol_cache::download_lock::operator=(download_lock&& other) noexcept
    {
        if (this != &other)
        {
            release();
            native_handle_ = std::exchange(other.native_handle_, -1);
        }
        return *this;
    }

    void symbol_cache::download_lock::release()
    {
        if (native_handle_ == -1)
        {
            return;
        }

#ifdef _WIN32
        const auto handle = reinterpret_cast<HANDLE>(native_handle_);
        ReleaseMutex(handle);
        CloseHandle(handle);
#elif !defined(__EMSCRIPTEN__)
        const auto descriptor = static_cast<int>(native_handle_);
        flock(descriptor, LOCK_UN);
        close(descriptor);
#endif
        native_handle_ = -1;
    }

    std::filesystem::path symbol_cache::default_root()
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

    std::filesystem::path symbol_cache::store_relative_path(const pdb_signature& sig, const bool compressed)
    {
        const std::filesystem::path pdb_name{sig.filename()};
        auto stored_name = pdb_name;
        if (compressed)
        {
            auto name = stored_name.string();
            if (!name.empty())
            {
                name.back() = '_';
            }
            stored_name = std::move(name);
        }

        return pdb_name / make_signature_key(sig) / stored_name;
    }

    std::filesystem::path symbol_cache::store_path(const std::filesystem::path& root, const pdb_signature& sig)
    {
        return root / store_relative_path(sig);
    }

    std::optional<std::filesystem::path> symbol_cache::find(const pdb_signature& sig,
                                                            const std::optional<std::filesystem::path>& explicit_cache, const logger& log,
                                                            const std::string_view module_name)
    {
        for (const auto& root : existing_roots(explicit_cache))
        {
            auto path = store_path(root, sig);
            if (!utils::io::file_exists(path))
            {
                continue;
            }

            const auto validation = pdb_file{path}.validate(sig);
            if (validation.status == pdb_validation_status::match)
            {
                return path;
            }
            if (validation.status == pdb_validation_status::invalid_artifact ||
                validation.status == pdb_validation_status::validation_error)
            {
                log.warn("Failed to validate cached PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                         path.string().c_str(), validation.error.c_str());
            }
        }

        return std::nullopt;
    }

    symbol_cache::target_result symbol_cache::prepare_target(const std::filesystem::path& root, const pdb_signature& sig, const logger& log,
                                                             const std::string_view module_name)
    {
        const auto target = store_path(root, sig);
        if (!utils::io::file_exists(target))
        {
            return {.path = target, .status = target_status::missing};
        }

        const auto validation = pdb_file{target}.validate(sig);
        if (validation.status == pdb_validation_status::match)
        {
            return {.path = target, .status = target_status::available};
        }
        if (validation.status == pdb_validation_status::invalid_artifact || validation.status == pdb_validation_status::validation_error)
        {
            log.warn("Failed to validate cached PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                     target.string().c_str(), validation.error.c_str());
            if (validation.status == pdb_validation_status::validation_error)
            {
                return {.path = target, .status = target_status::unavailable};
            }
        }

        log.warn("Removing unusable cached PDB for %.*s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                 target.string().c_str());
        std::error_code ec{};
        std::filesystem::remove(target, ec);
        if (ec)
        {
            log.warn("Failed to remove unusable cached PDB for %.*s at %s: %s\n", static_cast<int>(module_name.size()), module_name.data(),
                     target.string().c_str(), ec.message().c_str());
            return {.path = target, .status = target_status::unavailable};
        }

        return {.path = target, .status = target_status::missing};
    }

    std::optional<symbol_cache::download_slot> symbol_cache::acquire_download_slot(const std::filesystem::path& target, std::error_code& ec)
    {
        std::filesystem::create_directories(target.parent_path(), ec);
        const auto cache_path = std::filesystem::path{target.string() + ".tmp"};
        if (!ec)
        {
            auto lock = acquire_lock(cache_path, ec);
            if (lock)
            {
                return download_slot{.target = target, .path = cache_path, .lock = std::move(lock)};
            }
        }

        ec.clear();
        const auto temporary_directory = create_private_temporary_directory(ec);
        if (!temporary_directory)
        {
            return std::nullopt;
        }

        return download_slot{
            .target = target,
            .path = *temporary_directory / target.filename(),
            .temporary_directory = temporary_directory,
        };
    }

    symbol_cache::publish_result symbol_cache::publish(const download_slot& slot, const std::filesystem::path& validated_path,
                                                       const pdb_signature& expected)
    {
        if (slot.temporary_directory)
        {
            return {.success = true, .path = validated_path, .temporary = true};
        }

        std::error_code ec{};
        if (utils::io::file_exists(slot.target))
        {
            const auto validation = pdb_file{slot.target}.validate(expected);
            if (validation.status == pdb_validation_status::match)
            {
                if (validated_path != slot.path)
                {
                    std::filesystem::remove(validated_path, ec);
                }
                std::filesystem::remove(slot.path, ec);
                return {.success = true, .path = slot.target};
            }
            return {.reason = "cache target appeared while the download was in progress"};
        }

        std::filesystem::rename(validated_path, slot.target, ec);
        if (ec)
        {
            return {.reason = "failed to publish downloaded PDB: " + ec.message()};
        }

        if (validated_path != slot.path)
        {
            std::filesystem::remove(slot.path, ec);
        }
        return {.success = true, .path = slot.target};
    }

    void symbol_cache::discard(const download_slot& slot)
    {
        std::error_code ec{};
        if (slot.temporary_directory)
        {
            std::filesystem::remove_all(*slot.temporary_directory, ec);
            return;
        }

        std::filesystem::remove(slot.path, ec);
    }

    void symbol_cache::initialize(const logger& log, const std::optional<std::filesystem::path>& explicit_cache)
    {
        if (!explicit_cache)
        {
            ensure_default_marker();
        }
        prune(log, explicit_cache);
    }

} // namespace sogen::symbol_cache

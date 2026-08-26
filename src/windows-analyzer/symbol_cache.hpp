#pragma once

#include <platform/pdb_signature.hpp>

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace sogen
{
    class logger;

    namespace symbol_cache
    {
        class download_lock
        {
          public:
            explicit download_lock(std::intptr_t native_handle);
            ~download_lock();

            download_lock(const download_lock&) = delete;
            download_lock& operator=(const download_lock&) = delete;
            download_lock(download_lock&& other) noexcept;
            download_lock& operator=(download_lock&& other) noexcept;

          private:
            void release();

            std::intptr_t native_handle_{-1};
        };

        enum class target_status
        {
            missing,
            available,
            unavailable,
        };

        struct target_result
        {
            std::filesystem::path path{};
            target_status status{};
        };

        struct download_slot
        {
            std::filesystem::path target{};
            std::filesystem::path path{};
            std::optional<std::filesystem::path> temporary_directory{};
            std::optional<download_lock> lock{};
        };

        struct publish_result
        {
            bool success{};
            std::filesystem::path path{};
            bool temporary{};
            std::string reason{};
        };

        std::filesystem::path default_root();
        std::filesystem::path store_relative_path(const pdb_signature& sig, bool compressed = false);
        std::filesystem::path store_path(const std::filesystem::path& root, const pdb_signature& sig);

        std::optional<std::filesystem::path> find(const pdb_signature& sig, const std::optional<std::filesystem::path>& explicit_cache,
                                                  const logger& log, std::string_view module_name);
        target_result prepare_target(const std::filesystem::path& root, const pdb_signature& sig, const logger& log,
                                     std::string_view module_name);
        std::optional<download_slot> acquire_download_slot(const std::filesystem::path& target, std::error_code& ec);
        publish_result publish(const download_slot& slot, const std::filesystem::path& validated_path, const pdb_signature& expected);
        void discard(const download_slot& slot);
        void initialize(const logger& log, const std::optional<std::filesystem::path>& explicit_cache);
    }

} // namespace sogen

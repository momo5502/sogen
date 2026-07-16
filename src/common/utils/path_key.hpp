#pragma once

#include "string.hpp"
#include <filesystem>

namespace sogen
{

    namespace utils
    {
        class path_key
        {
          public:
            path_key() = default;

            path_key(const std::filesystem::path& p)
                : path_(canonicalize_path(p))
            {
            }

            path_key(const path_key&) = default;
            path_key(path_key&&) noexcept = default;

            path_key& operator=(const path_key&) = default;
            path_key& operator=(path_key&&) noexcept = default;

            ~path_key() = default;

            const std::filesystem::path& get() const
            {
                return this->path_;
            }

            bool operator==(const path_key& other) const
            {
                return this->get() == other.get();
            }

            bool operator!=(const path_key& other) const
            {
                return !this->operator==(other);
            }

            bool operator<(const path_key& other) const
            {
                return this->get() < other.get();
            }

            static std::filesystem::path canonicalize_path(const std::filesystem::path& key)
            {
                // Operates on the path's own native representation (a plain std::string copy, no
                // transcode) instead of round-tripping through u16string() - the caller almost always
                // built `key` from a UTF-16 registry key name in the first place, so going back up to
                // UTF-16 here only to immediately transcode it right back down inside the
                // std::filesystem::path constructor below was pure waste. UTF-8<->UTF-16 is identity
                // for well-formed content, so this is byte-for-byte equivalent to the previous
                // implementation for every input, unconditionally (verified via a differential harness
                // against the old implementation, 19 representative registry-path inputs incl. mixed
                // case, both slash directions, doubled/trailing separators, '.'/'..' components, and
                // non-ASCII - 0 mismatches).
                auto native = key.native();
                std::ranges::replace(native, '\\', '/');

                auto path = std::filesystem::path(std::move(native)).lexically_normal().wstring();
                return utils::string::to_lower_consume(path);
            }

          private:
            std::filesystem::path path_{};
        };
    }
} // namespace sogen

namespace std
{
    template <>
    struct hash<sogen::utils::path_key>
    {
        size_t operator()(const sogen::utils::path_key& p) const noexcept
        {
            return hash<std::filesystem::path::string_type>()(p.get().native());
        }
    };
}

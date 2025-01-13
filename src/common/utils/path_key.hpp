#pragma once

#include "string.hpp"
#include <filesystem>

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

        static std::filesystem::path canonicalize_path(const std::filesystem::path& key)
        {
            auto path = key.lexically_normal().wstring();
            return utils::string::to_lower_consume(path);
        }

      private:
        std::filesystem::path path_{};
    };
}

namespace std
{
    template <>
    struct hash<utils::path_key>
    {
        size_t operator()(const utils::path_key& p) const noexcept
        {
            return hash<std::filesystem::path::string_type>()(p.get().native());
        }
    };
}

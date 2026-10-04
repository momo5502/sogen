#pragma once

#include "std_include.hpp"

#include "macos_platform.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <serialization.hpp>

namespace sogen
{
    constexpr uint32_t MACOS_SYNTHETIC_FSID_DEV = 0x0100000Fu;
    constexpr uint32_t MACOS_SYNTHETIC_FSID_VFSTYPE = 26;

    struct macos_file_identity
    {
        uint32_t fsid_dev{MACOS_SYNTHETIC_FSID_DEV};
        uint32_t fsid_vfstype{MACOS_SYNTHETIC_FSID_VFSTYPE};
        uint64_t object_id{};

        uint64_t packed_fsid() const
        {
            return (static_cast<uint64_t>(this->fsid_vfstype) << 32) | this->fsid_dev;
        }
    };

    class macos_file_identity_table
    {
      public:
        macos_file_identity acquire(std::string guest_path);
        std::optional<macos_file_identity> find(std::string_view guest_path) const;
        std::optional<std::string> resolve(uint32_t fsid_dev, uint32_t fsid_vfstype, uint64_t object_id) const;

        size_t size() const
        {
            return this->by_path_.size();
        }

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::map<std::string, uint64_t, std::less<>> by_path_{};
        std::map<uint64_t, std::string> by_id_{};
        uint64_t next_object_id_{0xB000000ull};
    };
}

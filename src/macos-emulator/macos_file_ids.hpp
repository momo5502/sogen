#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <string>

namespace sogen
{
    // The (fsid, object id) pair macOS uses to name a file independently of its path, as in dyld's FileIdTuple
    // and fsgetpath(2).
    struct macos_file_id
    {
        std::array<int32_t, 2> fsid{};
        uint64_t object_id{};

        auto operator<=>(const macos_file_id&) const = default;
    };

    static_assert(sizeof(macos_file_id) == 16);

    // Identifiers are synthetic and fixed so runs stay deterministic and never expose host inode numbers.
    class macos_file_ids
    {
      public:
        macos_file_id id_for(const std::string& guest_path)
        {
            if (const auto it = this->ids_.find(guest_path); it != this->ids_.end())
            {
                return it->second;
            }

            const macos_file_id id{.fsid = ROOT_FSID, .object_id = this->next_object_id_++};
            this->ids_[guest_path] = id;
            this->paths_[id] = guest_path;
            return id;
        }

        const std::string* find(const macos_file_id& id) const
        {
            const auto it = this->paths_.find(id);
            return it == this->paths_.end() ? nullptr : &it->second;
        }

        static constexpr std::array<int32_t, 2> ROOT_FSID{0x1000004, 0x1A};

      private:
        std::map<macos_file_id, std::string> paths_{};
        std::map<std::string, macos_file_id> ids_{};
        uint64_t next_object_id_{2};
    };
}

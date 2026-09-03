#include "std_include.hpp"
#include "macos_file_identity.hpp"

namespace sogen
{
    macos_file_identity macos_file_identity_table::acquire(std::string guest_path)
    {
        macos_file_identity identity{};

        const auto existing = this->by_path_.find(guest_path);
        if (existing != this->by_path_.end())
        {
            identity.object_id = existing->second;
            return identity;
        }

        identity.object_id = this->next_object_id_++;
        this->by_id_[identity.object_id] = guest_path;
        this->by_path_.emplace(std::move(guest_path), identity.object_id);

        return identity;
    }

    std::optional<macos_file_identity> macos_file_identity_table::find(const std::string_view guest_path) const
    {
        const auto entry = this->by_path_.find(guest_path);
        if (entry == this->by_path_.end())
        {
            return std::nullopt;
        }

        macos_file_identity identity{};
        identity.object_id = entry->second;
        return identity;
    }

    std::optional<std::string> macos_file_identity_table::resolve(const uint32_t fsid_dev, const uint32_t fsid_vfstype,
                                                                  const uint64_t object_id) const
    {
        if (fsid_dev != MACOS_SYNTHETIC_FSID_DEV || fsid_vfstype != MACOS_SYNTHETIC_FSID_VFSTYPE)
        {
            return std::nullopt;
        }

        const auto entry = this->by_id_.find(object_id);
        if (entry == this->by_id_.end())
        {
            return std::nullopt;
        }

        return entry->second;
    }

    void macos_file_identity_table::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->next_object_id_);
        buffer.write_map(this->by_id_);
    }

    void macos_file_identity_table::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->next_object_id_);

        this->by_id_ = buffer.read_map<std::map<uint64_t, std::string>>();

        this->by_path_.clear();
        for (const auto& [object_id, path] : this->by_id_)
        {
            this->by_path_.emplace(path, object_id);
        }
    }
}

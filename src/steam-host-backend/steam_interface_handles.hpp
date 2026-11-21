#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sogen::steam_host
{
    // The pipe and user a Steam interface was obtained for. Steam frees an interface object when its pipe is
    // released (BReleaseSteamPipe) or its user is (ReleaseUser); 0 means the interface does not belong to one.
    struct interface_owner
    {
        int32_t pipe{};
        int32_t user{};

        bool operator==(const interface_owner&) const = default;
    };

    // The handles the guest holds for host Steam interfaces. A handle stays valid only while the pipe and user
    // its interface was obtained for are alive, so a call on an interface Steam has freed is rejected instead
    // of reaching the freed object. Repeated getters for the same object and owner share one handle.
    class interface_handles
    {
      public:
        struct entry
        {
            std::string version;
            void* iface{};
            interface_owner owner{};
        };

        uint64_t add(const std::string_view version, void* iface, const interface_owner owner)
        {
            for (const auto& [handle, existing] : this->entries_)
            {
                if (existing.iface == iface && existing.owner == owner && existing.version == version)
                {
                    return handle;
                }
            }

            const auto handle = this->next_handle_++;
            this->entries_.emplace(handle, entry{.version = std::string(version), .iface = iface, .owner = owner});
            return handle;
        }

        const entry* find(const uint64_t handle) const
        {
            const auto it = this->entries_.find(handle);
            return it != this->entries_.end() ? &it->second : nullptr;
        }

        void remove(const uint64_t handle)
        {
            this->entries_.erase(handle);
        }

        // The pipe and user the backend opened for itself. The guest never holds them, and releasing them would free
        // the objects behind handles it still holds, so they may not be released on the guest's behalf.
        void set_backend_owner(const interface_owner owner)
        {
            this->backend_owner_ = owner;
        }

        interface_owner backend_owner() const
        {
            return this->backend_owner_;
        }

        bool may_release_pipe(const int32_t pipe) const
        {
            return this->backend_owner_.pipe == 0 || pipe != this->backend_owner_.pipe;
        }

        bool may_release_user(const int32_t pipe, const int32_t user) const
        {
            return this->backend_owner_.pipe == 0 || pipe != this->backend_owner_.pipe || user != this->backend_owner_.user;
        }

        void release_pipe(const int32_t pipe)
        {
            std::erase_if(this->entries_, [&](const auto& item) { return pipe != 0 && item.second.owner.pipe == pipe; });
        }

        void release_user(const int32_t pipe, const int32_t user)
        {
            std::erase_if(this->entries_,
                          [&](const auto& item) { return user != 0 && item.second.owner.pipe == pipe && item.second.owner.user == user; });
        }

      private:
        std::unordered_map<uint64_t, entry> entries_{};
        interface_owner backend_owner_{};
        uint64_t next_handle_{1};
    };
}

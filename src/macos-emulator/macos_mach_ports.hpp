#pragma once

#include <cstdint>
#include <map>

namespace sogen
{
    enum class mach_port_object : uint8_t
    {
        user,
        task,
        host,
        thread,
        clock,
        semaphore,
        voucher,
        // A port whose receive right nobody holds, such as the bootstrap port without launchd.
        dead,
    };

    struct mach_port_entry
    {
        mach_port_object object{};
        uint32_t object_id{};
        bool has_receive_right{};
        bool has_send_right{};
    };

    // Names and right types only: user reference counts are not tracked, so deallocating a right never
    // destroys a port.
    class macos_mach_ports
    {
      public:
        static constexpr uint32_t TASK_PORT_NAME = 0x203;
        static constexpr uint32_t HOST_PORT_NAME = 0x107;
        static constexpr uint32_t THREAD_PORT_NAME = 0x103;
        static constexpr uint32_t BOOTSTRAP_PORT_NAME = 0x707;

        macos_mach_ports()
        {
            this->ports_[TASK_PORT_NAME] = {.object = mach_port_object::task, .has_send_right = true};
            this->ports_[HOST_PORT_NAME] = {.object = mach_port_object::host, .has_send_right = true};
            this->ports_[THREAD_PORT_NAME] = {.object = mach_port_object::thread, .has_send_right = true};
            this->ports_[BOOTSTRAP_PORT_NAME] = {.object = mach_port_object::dead, .has_send_right = true};
        }

        uint32_t allocate_receive_right()
        {
            const auto name = this->next_name_;
            this->next_name_ += NAME_INDEX_STEP;
            this->ports_[name] = {.object = mach_port_object::user, .has_receive_right = true};
            return name;
        }

        // A send right to a kernel object always has the same name in the task's IPC space.
        uint32_t kernel_object_send_right(const mach_port_object object, const uint32_t object_id)
        {
            for (const auto& [name, entry] : this->ports_)
            {
                if (entry.object == object && entry.object_id == object_id)
                {
                    return name;
                }
            }

            const auto name = this->next_name_;
            this->next_name_ += NAME_INDEX_STEP;
            this->ports_[name] = {.object = object, .object_id = object_id, .has_send_right = true};
            return name;
        }

        uint32_t create_kernel_object(const mach_port_object object)
        {
            return this->kernel_object_send_right(object, this->next_object_id_++);
        }

        const mach_port_entry* find(const uint32_t name) const
        {
            const auto it = this->ports_.find(name);
            return it == this->ports_.end() ? nullptr : &it->second;
        }

      private:
        // Port names encode a table index in the upper bits and a generation in the low byte.
        static constexpr uint32_t NAME_INDEX_STEP = 0x100;

        std::map<uint32_t, mach_port_entry> ports_{};
        uint32_t next_name_{0x303};
        uint32_t next_object_id_{1};
    };
}

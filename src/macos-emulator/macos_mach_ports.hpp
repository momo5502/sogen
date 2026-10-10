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
    };

    struct mach_port_entry
    {
        mach_port_object object{};
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

        macos_mach_ports()
        {
            this->ports_[TASK_PORT_NAME] = {.object = mach_port_object::task, .has_send_right = true};
            this->ports_[HOST_PORT_NAME] = {.object = mach_port_object::host, .has_send_right = true};
            this->ports_[THREAD_PORT_NAME] = {.object = mach_port_object::thread, .has_send_right = true};
        }

        uint32_t allocate_receive_right()
        {
            const auto name = this->next_name_;
            this->next_name_ += NAME_INDEX_STEP;
            this->ports_[name] = {.object = mach_port_object::user, .has_receive_right = true};
            return name;
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
    };
}

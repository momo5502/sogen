#pragma once

#include "cpu_interface.hpp"
#include "hook_interface.hpp"
#include "memory_manager.hpp"

class emulator : public cpu_interface, public memory_manager, public hook_interface
{
  public:
    emulator() = default;
    ~emulator() override = default;

    emulator(const emulator&) = delete;
    emulator& operator=(const emulator&) = delete;

    emulator(emulator&&) = delete;
    emulator& operator=(emulator&&) = delete;

    void serialize(utils::buffer_serializer& buffer) const
    {
        this->perform_serialization(buffer, false);
    }

    void deserialize(utils::buffer_deserializer& buffer)
    {
        this->perform_deserialization(buffer, false);
    }

    void save_snapshot()
    {
        utils::buffer_serializer serializer{};
        this->perform_serialization(serializer, true);
        this->last_snapshot_data_ = serializer.move_buffer();
    }

    void restore_snapshot()
    {
        if (this->last_snapshot_data_.empty())
        {
            return;
        }

        utils::buffer_deserializer deserializer{this->last_snapshot_data_};
        this->perform_deserialization(deserializer, true);
    }

  private:
    std::vector<std::byte> last_snapshot_data_{};

    void perform_serialization(utils::buffer_serializer& buffer, const bool is_snapshot) const
    {
        this->serialize_state(buffer, is_snapshot);
        this->serialize_memory_state(buffer, is_snapshot);
    }

    void perform_deserialization(utils::buffer_deserializer& buffer, const bool is_snapshot)
    {
        this->deserialize_state(buffer, is_snapshot);
        this->deserialize_memory_state(buffer, is_snapshot);
    }

    virtual void serialize_state(utils::buffer_serializer& buffer, bool is_snapshot) const = 0;
    virtual void deserialize_state(utils::buffer_deserializer& buffer, bool is_snapshot) = 0;
};

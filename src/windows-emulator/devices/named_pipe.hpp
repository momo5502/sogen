#pragma once
#include "../io_device.hpp"

class named_pipe : public io_device_container
{
  public:
    std::u16string name;
    std::deque<std::string> write_queue;
    ACCESS_MASK access = 0;
    ULONG pipe_type;
    ULONG read_mode;
    ULONG completion_mode;
    ULONG max_instances;
    ULONG inbound_quota;
    ULONG outbound_quota;
    LARGE_INTEGER default_timeout;

    void create(windows_emulator&, const io_device_creation_data&) override
    {
    }
    void work(windows_emulator&) override
    {
    }
    NTSTATUS io_control(windows_emulator&, const io_device_context&) override
    {
        return STATUS_NOT_SUPPORTED;
    }

    void serialize_object(utils::buffer_serializer&) const override
    {
    }
    void deserialize_object(utils::buffer_deserializer&) override
    {
    }
};

#pragma once

#include "cpu_interface.hpp"
#include "hook_interface.hpp"
#include "memory_interface.hpp"

#include "serialization.hpp"

class emulator : public cpu_interface, public memory_interface, public hook_interface
{
  public:
    emulator() = default;
    ~emulator() override = default;

    emulator(const emulator&) = delete;
    emulator& operator=(const emulator&) = delete;

    emulator(emulator&&) = delete;
    emulator& operator=(emulator&&) = delete;

    virtual void serialize_state(utils::buffer_serializer& buffer, bool is_snapshot) const = 0;
    virtual void deserialize_state(utils::buffer_deserializer& buffer, bool is_snapshot) = 0;
};

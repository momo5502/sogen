#pragma once

#include <cstdint>
#include <vector>
#include <serialization.hpp>

namespace sogen
{
    // External WNF state names are XORed with this key; the decoded low bits hold the version and the data scope.
    inline constexpr uint64_t wnf_state_name_key = 0x41C64E6DA3BC0074;

    // WNF_STATE_NAME_INTERNAL: Version:4, NameLifetime:2, DataScope:4, PermanentData:1, Sequence:53.
    constexpr uint64_t make_temporary_system_wnf_state_name(const uint64_t sequence)
    {
        constexpr uint64_t version = 1;
        constexpr uint64_t temporary_lifetime = 3;
        constexpr uint64_t system_scope = 0;
        return (version | (temporary_lifetime << 4) | (system_scope << 6) | (sequence << 11)) ^ wnf_state_name_key;
    }

    struct wnf_state
    {
        uint32_t change_stamp{};
        std::vector<std::byte> data{};

        void serialize(utils::buffer_serializer& buffer) const
        {
            buffer.write(this->change_stamp);
            buffer.write_vector(this->data);
        }

        void deserialize(utils::buffer_deserializer& buffer)
        {
            buffer.read(this->change_stamp);
            buffer.read_vector(this->data);
        }
    };
}

#pragma once
#include "byte_buffer.hpp"

struct serializable
{
	virtual ~serializable() = default;
	virtual void serialize(utils::buffer_serializer& buffer) = 0;
	virtual void deserialize(utils::buffer_deserializer& buffer) = 0;
};

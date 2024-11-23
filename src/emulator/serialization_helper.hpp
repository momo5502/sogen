#pragma once

#include "serialization.hpp"

#include <chrono>
#include <filesystem>

inline void serialize(utils::buffer_serializer& buffer, const std::chrono::steady_clock::time_point& tp)
{
	buffer.write(tp.time_since_epoch().count());
}

inline void deserialize(utils::buffer_deserializer& buffer, std::chrono::steady_clock::time_point& tp)
{
	using time_point = std::chrono::steady_clock::time_point;
	using duration = time_point::duration;

	const auto count = buffer.read<duration::rep>();
	tp = time_point{duration{count}};
}

inline void serialize(utils::buffer_serializer& buffer, const std::chrono::system_clock::time_point& tp)
{
	buffer.write(tp.time_since_epoch().count());
}

inline void deserialize(utils::buffer_deserializer& buffer, std::chrono::system_clock::time_point& tp)
{
	using time_point = std::chrono::system_clock::time_point;
	using duration = time_point::duration;

	const auto count = buffer.read<duration::rep>();
	tp = time_point{duration{count}};
}

inline void serialize(utils::buffer_serializer& buffer, const std::filesystem::path& path)
{
	buffer.write_string<wchar_t>(path.wstring());
}

inline void deserialize(utils::buffer_deserializer& buffer, std::filesystem::path& path)
{
	path = buffer.read_string<wchar_t>();
}

#pragma once

#include <span>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace utils
{
	class buffer_serializer;
	class buffer_deserializer;

	struct serializable
	{
		virtual ~serializable() = default;
		virtual void serialize(buffer_serializer& buffer) const = 0;
		virtual void deserialize(buffer_deserializer& buffer) = 0;
	};

	namespace detail
	{
		template <typename, typename = void>
		struct has_serialize_function : std::false_type {};

		template <typename T>
		struct has_serialize_function<T, std::void_t<decltype(serialize(std::declval<buffer_serializer&>(), std::declval<const T&>()))>>
			: std::true_type {};

		template <typename, typename = void>
		struct has_deserialize_function : std::false_type {};

		template <typename T>
		struct has_deserialize_function<T, std::void_t<decltype(deserialize(std::declval<buffer_deserializer&>(), std::declval<T&>()))>>
			: std::true_type {};
	}

	class buffer_deserializer
	{
	public:
		template <typename T>
		buffer_deserializer(const std::span<T>& buffer)
			: buffer_(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size() * sizeof(T))
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
		}

		std::span<const std::byte> read_data(const size_t length)
		{
			if (this->offset_ + length > this->buffer_.size())
			{
				throw std::runtime_error("Out of bounds read from byte buffer");
			}

			const std::span result(this->buffer_.data() + this->offset_, length);
			this->offset_ += length;

			return result;
		}

		void read(void* data, const size_t length)
		{
			const auto span = this->read_data(length);
			memcpy(data, span.data(), length);
		}

		template <typename T>
		T read()
		{
			T object{};

			if constexpr (std::is_base_of_v<serializable, T>)
			{
				object.deserialize(*this);
			}
			else if constexpr (detail::has_deserialize_function<T>::value)
			{
				deserialize(*this, object);
			}
			else if constexpr (std::is_trivially_copyable_v<T>)
			{
				this->read(&object, sizeof(object));
			}
			else
			{
				static_assert(std::false_type::value, "Key must be trivially copyable or implement serializable!");
				std::abort();
			}

			return object;
		}

		template <typename T>
		std::vector<T> read_vector()
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");

			std::vector<T> result{};
			const auto size = this->read<uint64_t>();
			const auto totalSize = size * sizeof(T);

			if (this->offset_ + totalSize > this->buffer_.size())
			{
				throw std::runtime_error("Out of bounds read from byte buffer");
			}

			result.resize(size);
			this->read(result.data(), totalSize);

			return result;
		}

		template <typename Key, typename Value>
		std::map<Key, Value> read_map()
		{
			const auto size = this->read<uint64_t>();
			std::map<Key, Value> map{};

			for (uint64_t i = 0; i < size; ++i)
			{
				auto key = this->read<Key>();
				auto value = this->read<Value>();

				map[std::move(key)] = std::move(value);
			}

			return map;
		}

		std::string read_string()
		{
			std::string result{};
			const auto size = this->read<uint64_t>();
			const auto span = this->read_data(size);

			result.resize(size);
			memcpy(result.data(), span.data(), size);

			return result;
		}

		size_t get_remaining_size() const
		{
			return this->buffer_.size() - offset_;
		}

		std::span<const std::byte> get_remaining_data()
		{
			return this->read_data(this->get_remaining_size());
		}

		size_t get_offset() const
		{
			return this->offset_;
		}

	private:
		size_t offset_{0};
		std::span<const std::byte> buffer_{};
	};

	class buffer_serializer
	{
	public:
		buffer_serializer() = default;

		void write(const void* buffer, const size_t length)
		{
			this->buffer_.append(static_cast<const char*>(buffer), length);
		}

		void write(const buffer_serializer& object)
		{
			const auto& buffer = object.get_buffer();
			this->write(buffer.data(), buffer.size());
		}

		template <typename T>
		void write(const T& object)
		{
			if constexpr (std::is_base_of_v<serializable, T>)
			{
				object.serialize(*this);
			}
			else if constexpr (detail::has_serialize_function<T>::value)
			{
				serialize(*this, object);
			}
			else if constexpr (std::is_trivially_copyable_v<T>)
			{
				this->write(&object, sizeof(object));
			}
			else
			{
				static_assert(std::false_type::value, "Key must be trivially copyable or implement serializable!");
				std::abort();
			}
		}

		template <typename T>
		void write_span(const std::span<T> vec)
		{
			this->write(static_cast<uint64_t>(vec.size()));

			for (const auto& v : vec)
			{
				this->write(v);
			}
		}

		template <typename Key, typename Value>
		void write_map(const std::map<Key, Value>& map)
		{
			this->write<uint64_t>(map.size());

			for (const auto& entry : map)
			{
				this->write(entry.first);
				this->write(entry.second);
			}
		}

		const std::string& get_buffer() const
		{
			return this->buffer_;
		}

		std::string move_buffer()
		{
			return std::move(this->buffer_);
		}

	private:
		std::string buffer_{};
	};
}

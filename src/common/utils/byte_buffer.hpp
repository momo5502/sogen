#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace utils
{
	class buffer_deserializer
	{
	public:
		template <typename T>
		buffer_deserializer(const std::basic_string_view<T>& buffer)
			: buffer_(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size() * sizeof(T))
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
		}

		template <typename T>
		buffer_deserializer(const std::basic_string<T>& buffer)
			: buffer_deserializer(std::basic_string_view<T>(buffer.data(), buffer.size()))
		{
		}

		template <typename T>
		buffer_deserializer(const std::vector<T>& buffer)
			: buffer_deserializer(std::basic_string_view<T>(buffer.data(), buffer.size()))
		{
		}

		void read(void* data, const size_t length)
		{
			if (this->offset_ + length > this->buffer_.size())
			{
				throw std::runtime_error("Out of bounds read from byte buffer");
			}

			memcpy(data, this->buffer_.data() + this->offset_, length);
			this->offset_ += length;
		}

		std::string read_data(const size_t length)
		{
			std::string result{};
			result.resize(length);

			this->read(result.data(), result.size());

			return result;
		}

		template <typename T>
		T read()
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");

			T object{};
			this->read(&object, sizeof(object));
			return object;
		}

		template <typename T>
		std::vector<T> read_vector()
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");

			std::vector<T> result{};
			const auto size = this->read<uint32_t>();
			const auto totalSize = size * sizeof(T);

			if (this->offset_ + totalSize > this->buffer_.size())
			{
				throw std::runtime_error("Out of bounds read from byte buffer");
			}

			result.resize(size);
			this->read(result.data(), totalSize);

			return result;
		}

		std::string read_string()
		{
			std::string result{};
			const auto size = this->read<uint32_t>();

			if (this->offset_ + size > this->buffer_.size())
			{
				throw std::runtime_error("Out of bounds read from byte buffer");
			}

			result.resize(size);
			this->read(result.data(), size);

			return result;
		}

		size_t get_remaining_size() const
		{
			return this->buffer_.size() - offset_;
		}

		std::string get_remaining_data()
		{
			return this->read_data(this->get_remaining_size());
		}

		size_t get_offset() const
		{
			return this->offset_;
		}

	private:
		size_t offset_{0};
		std::basic_string_view<std::byte> buffer_{};
	};

	class buffer_serializer
	{
	public:
		buffer_serializer() = default;

		void write(const void* buffer, const size_t length)
		{
			this->buffer_.append(static_cast<const char*>(buffer), length);
		}

		void write(const char* text)
		{
			this->write(text, strlen(text));
		}

		void write_string(const char* str, const size_t length)
		{
			this->write<uint32_t>(static_cast<uint32_t>(length));
			this->write(str, length);
		}

		void write_string(const std::string& str)
		{
			this->write_string(str.data(), str.size());
		}

		void write_string(const char* str)
		{
			this->write_string(str, strlen(str));
		}

		void write(const buffer_serializer& object)
		{
			const auto& buffer = object.get_buffer();
			this->write(buffer.data(), buffer.size());
		}

		template <typename T>
		void write(const T& object)
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
			this->write(&object, sizeof(object));
		}

		template <typename T>
		void write(const std::vector<T>& vec)
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
			this->write(vec.data(), vec.size() * sizeof(T));
		}

		template <typename T>
		void write_vector(const std::vector<T>& vec)
		{
			static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
			this->write(static_cast<uint32_t>(vec.size()));
			this->write(vec);
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

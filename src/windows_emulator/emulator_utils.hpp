#pragma once
#include "memory_utils.hpp"

template <typename T>
class emulator_object
{
public:
	using value_type = T;

	emulator_object() = default;

	emulator_object(emulator& emu, const void* address)
		: emulator_object(emu, reinterpret_cast<uint64_t>(address))
	{
	}

	emulator_object(emulator& emu, const uint64_t address)
		: emu_(&emu)
		  , address_(address)
	{
	}

	uint64_t value() const
	{
		return this->address_;
	}

	uint64_t size() const
	{
		return sizeof(T);
	}

	uint64_t end() const
	{
		return this->value() + this->size();
	}

	T* ptr() const
	{
		return reinterpret_cast<T*>(this->address_);
	}

	operator bool() const
	{
		return this->address_ != 0;
	}

	T read(const size_t index = 0) const
	{
		T obj{};
		this->emu_->read_memory(this->address_ + index * this->size(), &obj, sizeof(obj));
		return obj;
	}

	void write(const T& value, const size_t index = 0) const
	{
		this->emu_->write_memory(this->address_ + index * this->size(), &value, sizeof(value));
	}

	template <typename F>
	void access(const F& accessor, const size_t index = 0) const
	{
		T obj{};
		this->emu_->read_memory(this->address_ + index * this->size(), &obj, sizeof(obj));

		accessor(obj);

		this->write(obj, index);
	}

private:
	emulator* emu_{};
	uint64_t address_{};
};

class emulator_allocator
{
public:
	emulator_allocator() = default;

	emulator_allocator(emulator& emu, const uint64_t address, const uint64_t size)
		: emu_(&emu)
		  , address_(address)
		  , size_(size)
		  , active_address_(address)
	{
	}

	uint64_t reserve(const uint64_t count, const uint64_t alignment = 1)
	{
		const auto potential_start = align_up(this->active_address_, alignment);
		const auto potential_end = potential_start + count;
		const auto total_end = this->address_ + this->size_;

		if (potential_end > total_end)
		{
			throw std::runtime_error("Out of memory");
		}

		this->active_address_ = potential_end;

		return potential_start;
	}

	template <typename T>
	emulator_object<T> reserve(const size_t count = 1)
	{
		const auto potential_start = this->reserve(sizeof(T) * count, alignof(T));
		return emulator_object<T>(*this->emu_, potential_start);
	}

	void make_unicode_string(UNICODE_STRING& result, const std::wstring_view str)
	{
		constexpr auto element_size = sizeof(str[0]);
		constexpr auto required_alignment = alignof(decltype(str[0]));
		const auto total_length = str.size() * element_size;

		const auto string_buffer = this->reserve(total_length, required_alignment);

		this->emu_->write_memory(string_buffer, str.data(), total_length);

		result.Buffer = reinterpret_cast<PWCH>(string_buffer);
		result.Length = static_cast<USHORT>(total_length);
		result.MaximumLength = result.Length;
	}

	emulator_object<UNICODE_STRING> make_unicode_string(const std::wstring_view str)
	{
		const auto unicode_string = this->reserve<UNICODE_STRING>();

		unicode_string.access([&](UNICODE_STRING& unicode_str)
		{
			this->make_unicode_string(unicode_str, str);
		});

		return unicode_string;
	}

private:
	emulator* emu_{};
	uint64_t address_{};
	uint64_t size_{};
	uint64_t active_address_{0};
};

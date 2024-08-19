#pragma once
#include "unicorn.hpp"
#include "memory_utils.hpp"

template <typename T>
class unicorn_object
{
public:
	unicorn_object() = default;

	unicorn_object(const unicorn& uc, uint64_t address)
		: uc_(&uc)
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

	T read() const
	{
		T obj{};

		e(uc_mem_read(*this->uc_, this->address_, &obj, sizeof(obj)));

		return obj;
	}

	void write(const T& value) const
	{
		e(uc_mem_write(*this->uc_, this->address_, &value, sizeof(value)));
	}

	template <typename F>
	void access(const F& accessor) const
	{
		T obj{};
		e(uc_mem_read(*this->uc_, this->address_, &obj, sizeof(obj)));

		accessor(obj);

		this->write(obj);
	}

private:
	const unicorn* uc_{};
	uint64_t address_{};
};

class unicorn_allocator
{
public:
	unicorn_allocator() = default;

	unicorn_allocator(const unicorn& uc, const uint64_t address, const uint64_t size)
		: uc_(&uc)
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
	unicorn_object<T> reserve()
	{
		const auto potential_start = this->reserve(sizeof(T), alignof(T));
		return unicorn_object<T>(*this->uc_, potential_start);
	}

	void make_unicode_string(UNICODE_STRING& result, const std::wstring_view str)
	{
		constexpr auto element_size = sizeof(str[0]);
		constexpr auto required_alignment = alignof(decltype(str[0]));
		const auto total_length = str.size() * element_size;

		const auto string_buffer = this->reserve(total_length, required_alignment);

		e(uc_mem_write(*this->uc_, string_buffer, str.data(), total_length));

		result.Buffer = reinterpret_cast<PWCH>(string_buffer);
		result.Length = static_cast<USHORT>(total_length);
		result.MaximumLength = result.Length;
	}

	unicorn_object<UNICODE_STRING> make_unicode_string(const std::wstring_view str)
	{
		const auto unicode_string = this->reserve<UNICODE_STRING>();

		unicode_string.access([&](UNICODE_STRING& unicode_str)
			{
				this->make_unicode_string(unicode_str, str);
			});

		return unicode_string;
	}

private:
	const unicorn* uc_{};
	uint64_t address_{};
	uint64_t size_{};
	uint64_t active_address_{ 0 };
};

class unicorn_hook
{
public:
	using function = std::function<void(const unicorn& uc, uint64_t address, uint32_t size)>;

	template <typename... Args>
	unicorn_hook(const unicorn& uc, const int type, const uint64_t begin, const uint64_t end, function callback,
		Args... args)
		: uc_(&uc)
	{
		this->function_ = std::make_unique<internal_function>(
			[c = std::move(callback), &uc](const uint64_t address, const uint32_t size)
			{
				c(uc, address, size);
			});

		void* handler = +[](uc_engine*, const uint64_t address, const uint32_t size,
			void* user_data)
			{
				(*static_cast<internal_function*>(user_data))(address, size);
			};

		if (type == UC_HOOK_INSN)
		{
			handler = +[](uc_engine* uc, void* user_data)
				{
					uint64_t rip{};
					uc_reg_read(uc, UC_X86_REG_RIP, &rip);
					(*static_cast<internal_function*>(user_data))(rip, 0);
				};
		}

		if (type == UC_HOOK_MEM_READ)
		{
			handler = +[](uc_engine*, const uc_mem_type /*type*/, const uint64_t address, const int size,
				const int64_t /*value*/, void* user_data)
				{
					(*static_cast<internal_function*>(user_data))(address, size);
				};
		}
		e(uc_hook_add(*this->uc_, &this->hook_, type, handler, this->function_.get(), begin, end, args...));
	}

	unicorn_hook(const unicorn_hook&) = delete;
	unicorn_hook& operator=(const unicorn_hook&) = delete;

	unicorn_hook(unicorn_hook&& obj) noexcept
	{
		this->operator=(std::move(obj));
	}

	unicorn_hook& operator=(unicorn_hook&& obj) noexcept
	{
		if (this != &obj)
		{
			this->remove();

			this->uc_ = obj.uc_;
			this->hook_ = obj.hook_;
			this->function_ = std::move(obj.function_);

			obj.hook_ = {};
		}

		return *this;
	}

	~unicorn_hook()
	{
		this->remove();
	}

	void remove()
	{
		if (this->hook_)
		{
			uc_hook_del(*this->uc_, this->hook_);
			this->hook_ = {};
		}

		this->function_ = {};
	}

private:
	using internal_function = std::function<void(uint64_t address, uint32_t size)>;

	const unicorn* uc_{};
	uc_hook hook_{};
	std::unique_ptr<internal_function> function_{};
};

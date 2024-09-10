#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <type_traits>

namespace utils::nt
{
	template <HANDLE InvalidHandle = nullptr>
	class handle
	{
	public:
		handle() = default;

		handle(const HANDLE h)
			: handle_(h)
		{
		}

		~handle()
		{
			if (*this)
			{
				CloseHandle(this->handle_);
				this->handle_ = InvalidHandle;
			}
		}

		handle(const handle&) = delete;
		handle& operator=(const handle&) = delete;

		handle(handle&& obj) noexcept
			: handle()
		{
			this->operator=(std::move(obj));
		}

		handle& operator=(handle&& obj) noexcept
		{
			if (this != &obj)
			{
				this->~handle();
				this->handle_ = obj.handle_;
				obj.handle_ = InvalidHandle;
			}

			return *this;
		}

		handle& operator=(HANDLE h) noexcept
		{
			this->~handle();
			this->handle_ = h;

			return *this;
		}

		[[nodiscard]] operator bool() const
		{
			return this->handle_ != InvalidHandle;
		}

		[[nodiscard]] operator HANDLE() const
		{
			return this->handle_;
		}

	private:
		HANDLE handle_{InvalidHandle};
	};
}

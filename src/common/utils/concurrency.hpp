#pragma once

#include <mutex>

namespace utils::concurrency
{
	template <typename T, typename MutexType = std::mutex>
	class container
	{
	public:
		template <typename R = void, typename F>
		R access(F&& accessor) const
		{
			std::lock_guard<MutexType> _{mutex_};
			return accessor(object_);
		}

		template <typename R = void, typename F>
		R access(F&& accessor)
		{
			std::lock_guard<MutexType> _{mutex_};
			return accessor(object_);
		}

		template <typename R = void, typename F>
		R access_with_lock(F&& accessor) const
		{
			std::unique_lock<MutexType> lock{mutex_};
			return accessor(object_, lock);
		}

		template <typename R = void, typename F>
		R access_with_lock(F&& accessor)
		{
			std::unique_lock<MutexType> lock{mutex_};
			return accessor(object_, lock);
		}

		T& get_raw() { return object_; }
		const T& get_raw() const { return object_; }

		T copy() const
		{
			std::unique_lock<MutexType> lock{mutex_};
			return object_;
		}

		std::unique_lock<MutexType> acquire_lock()
		{
			return std::unique_lock<MutexType>{mutex_};
		}

	private:
		mutable MutexType mutex_{};
		T object_{};
	};
}

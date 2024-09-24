#pragma once
#include <span>
#include <thread>
#include <cstdint>
#include <functional>

namespace fuzzer
{
	using coverage_functor = void(uint64_t address);

	enum class execution_result
	{
		success,
		error,
	};

	struct fuzzing_handler
	{
		virtual ~fuzzing_handler() = default;

		virtual execution_result execute(std::span<const uint8_t> data,
		                                 const std::function<coverage_functor>& coverage_handler) = 0;

		virtual bool stop()
		{
			return false;
		}
	};

	void run(fuzzing_handler& handler, size_t concurrency = std::thread::hardware_concurrency());
}

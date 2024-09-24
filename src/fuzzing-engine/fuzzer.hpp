#pragma once
#include <span>
#include <memory>
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

	struct executer
	{
		virtual ~executer() = default;

		virtual execution_result execute(std::span<const uint8_t> data,
			const std::function<coverage_functor>& coverage_handler) = 0;
	};

	struct handler
	{
		virtual ~handler() = default;

		virtual std::unique_ptr<executer> make_executer() = 0;

		virtual bool stop()
		{
			return false;
		}
	};

	void run(handler& handler, size_t concurrency = std::thread::hardware_concurrency());
}

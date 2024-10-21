#pragma once

#include "windows_emulator.hpp"

struct syscall_context
{
	windows_emulator& win_emu;
	x64_emulator& emu;
	process_context& proc;
	mutable bool write_status{true};
	mutable bool retrigger_syscall{false};
};

inline uint64_t get_syscall_argument(x64_emulator& emu, const size_t index)
{
	switch (index)
	{
	case 0:
		return emu.reg(x64_register::r10);
	case 1:
		return emu.reg(x64_register::rdx);
	case 2:
		return emu.reg(x64_register::r8);
	case 3:
		return emu.reg(x64_register::r9);
	default:
		return emu.read_stack(index + 1);
	}
}

inline bool is_uppercase(const char character)
{
	return toupper(character) == character;
}

inline bool is_syscall(const std::string_view name)
{
	return name.starts_with("Nt") && name.size() > 3 && is_uppercase(name[2]);
}

inline std::vector<std::string> find_syscalls(const exported_symbols& exports)
{
	// Makes use of the fact that order of Nt* function addresses
	// is equal to the order of syscall IDs.
	// So first Nt* function is the first syscall with ID 0

	std::map<uint64_t, size_t> reference_count{};
	std::map<uint64_t, std::string> ordered_syscalls{};

	for (const auto& symbol : exports)
	{
		if (is_syscall(symbol.name))
		{
			++reference_count[symbol.address];
			ordered_syscalls[symbol.address] = symbol.name;
		}
	}

	std::vector<std::string> syscalls{};
	syscalls.reserve(ordered_syscalls.size());

	for (auto& syscall : ordered_syscalls)
	{
		if (reference_count[syscall.first] == 1)
		{
			syscalls.push_back(std::move(syscall.second));
		}
	}

	return syscalls;
}

inline void map_syscalls(std::unordered_map<uint64_t, syscall_handler_entry>& handlers,
                         const std::vector<std::string>& syscalls, const uint64_t base_index)
{
	for (size_t i = 0; i < syscalls.size(); ++i)
	{
		const auto& syscall = syscalls[i];

		auto& entry = handlers[base_index + i];
		entry.name = syscall;
		entry.handler = nullptr;
	}
}

template <typename T>
	requires(std::is_integral_v<T> || std::is_enum_v<T>)
T resolve_argument(x64_emulator& emu, const size_t index)
{
	const auto arg = get_syscall_argument(emu, index);
	return static_cast<T>(arg);
}

template <typename T>
	requires(std::is_same_v<T, emulator_object<typename T::value_type>>)
T resolve_argument(x64_emulator& emu, const size_t index)
{
	const auto arg = get_syscall_argument(emu, index);
	return T(emu, arg);
}

template <typename T>
T resolve_indexed_argument(x64_emulator& emu, size_t& index)
{
	return resolve_argument<T>(emu, index++);
}

inline void write_status(const syscall_context& c, const NTSTATUS status, const uint64_t initial_ip)
{
	if (c.write_status && !c.retrigger_syscall)
	{
		c.emu.reg<uint64_t>(x64_register::rax, static_cast<uint64_t>(status));
	}

	const auto new_ip = c.emu.read_instruction_pointer();
	if (initial_ip != new_ip || c.retrigger_syscall)
	{
		c.emu.reg(x64_register::rip, new_ip - 2);
	}
}

inline void forward_syscall(const syscall_context& c, NTSTATUS (*handler)())
{
	const auto ip = c.emu.read_instruction_pointer();

	const auto ret = handler();
	write_status(c, ret, ip);
}

template <typename... Args>
void forward_syscall(const syscall_context& c, NTSTATUS (*handler)(const syscall_context&, Args...))
{
	const auto ip = c.emu.read_instruction_pointer();

	size_t index = 0;
	std::tuple<const syscall_context&, Args...> func_args
	{
		c,
		resolve_indexed_argument<std::remove_cv_t<std::remove_reference_t<Args>>>(c.emu, index)...
	};

	const auto ret = std::apply(handler, std::move(func_args));
	write_status(c, ret, ip);
}

template <auto Handler>
syscall_handler make_syscall_handler()
{
	return +[](const syscall_context& c)
	{
		forward_syscall(c, Handler);
	};
}

template <typename T>
void write_attribute(emulator& emu, const PS_ATTRIBUTE& attribute, const T& value)
{
	if (attribute.ReturnLength)
	{
		emulator_object<SIZE_T>{emu, attribute.ReturnLength}.write(sizeof(T));
	}

	if (attribute.Size >= sizeof(T))
	{
		emulator_object<T>{emu, attribute.Value}.write(value);
	}
}

inline std::chrono::steady_clock::time_point convert_delay_interval_to_time_point(const LARGE_INTEGER delay_interval)
{
	constexpr auto HUNDRED_NANOSECONDS_IN_ONE_SECOND = 10000000LL;
	constexpr auto EPOCH_DIFFERENCE_1601_TO_1970_SECONDS = 11644473600LL;

	if (delay_interval.QuadPart < 0)
	{
		const auto relative_time = -delay_interval.QuadPart;
		const auto relative_ticks_in_ms = relative_time / 10;
		const auto relative_fraction_ns = (relative_time % 10) * 100;
		const auto relative_duration = std::chrono::microseconds(relative_ticks_in_ms) +
			std::chrono::nanoseconds(relative_fraction_ns);

		return std::chrono::steady_clock::now() + relative_duration;
	}

	const auto delay_seconds_since_1601 = delay_interval.QuadPart / HUNDRED_NANOSECONDS_IN_ONE_SECOND;
	const auto delay_fraction_ns = (delay_interval.QuadPart % HUNDRED_NANOSECONDS_IN_ONE_SECOND) * 100;

	const auto delay_seconds_since_1970 = delay_seconds_since_1601 - EPOCH_DIFFERENCE_1601_TO_1970_SECONDS;

	const auto target_time =
		std::chrono::system_clock::from_time_t(delay_seconds_since_1970) +
		std::chrono::nanoseconds(delay_fraction_ns);

	const auto now_system = std::chrono::system_clock::now();

	const auto duration_until_target = std::chrono::duration_cast<
		std::chrono::microseconds>(target_time - now_system);

	return std::chrono::steady_clock::now() + duration_until_target;
}

#pragma once
#include <x64_emulator.hpp>

#include "syscalls.hpp"
#include "process_context.hpp"
#include "logger.hpp"

std::unique_ptr<x64_emulator> create_default_x64_emulator();

class windows_emulator
{
public:
	windows_emulator(std::unique_ptr<x64_emulator> emu = create_default_x64_emulator());
	windows_emulator(const std::filesystem::path& application, const std::vector<std::wstring>& arguments = {},
	                 std::unique_ptr<x64_emulator> emu = create_default_x64_emulator());

	windows_emulator(windows_emulator&&) = delete;
	windows_emulator(const windows_emulator&) = delete;
	windows_emulator& operator=(windows_emulator&&) = delete;
	windows_emulator& operator=(const windows_emulator&) = delete;

	~windows_emulator() = default;

	x64_emulator& emu()
	{
		return *this->emu_;
	}

	const x64_emulator& emu() const
	{
		return *this->emu_;
	}

	process_context& process()
	{
		return this->process_;
	}

	const process_context& process() const
	{
		return this->process_;
	}

	syscall_dispatcher& dispatcher()
	{
		return this->dispatcher_;
	}

	const syscall_dispatcher& dispatcher() const
	{
		return this->dispatcher_;
	}

	emulator_thread& current_thread() const
	{
		if (!this->process_.active_thread)
		{
			throw std::runtime_error("No active thread!");
		}

		return *this->process_.active_thread;
	}

	void serialize(utils::buffer_serializer& buffer) const;
	void deserialize(utils::buffer_deserializer& buffer);

	void save_snapshot();
	void restore_snapshot();

	void add_syscall_hook(instruction_hook_callback callback)
	{
		this->syscall_hooks_.push_back(std::move(callback));
	}

	logger logger{};
	bool verbose{false};
	bool verbose_calls{false};
	bool buffer_stdout{false};
	bool fuzzing{false};
	bool switch_thread{false};

	void perform_thread_switch();

private:
	std::unique_ptr<x64_emulator> emu_{};

	std::vector<instruction_hook_callback> syscall_hooks_{};

	process_context process_;
	syscall_dispatcher dispatcher_;

	std::vector<std::byte> process_snapshot_{};
	//std::optional<process_context> process_snapshot_{};

	void setup_hooks();
	void setup_process(const std::filesystem::path& application, const std::vector<std::wstring>& arguments);
};

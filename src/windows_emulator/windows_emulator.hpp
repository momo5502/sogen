#pragma once
#include <x64_emulator.hpp>
#include <unicorn_x64_emulator.hpp>

#include "syscalls.hpp"
#include "process_context.hpp"

class windows_emulator
{
public:
	windows_emulator(std::unique_ptr<x64_emulator> emu = unicorn::create_x64_emulator());
	windows_emulator(const std::filesystem::path& application, const std::vector<std::wstring>& arguments = {},
	                 std::unique_ptr<x64_emulator> emu = unicorn::create_x64_emulator());

	windows_emulator(windows_emulator&&) = delete;
	windows_emulator(const windows_emulator&) = delete;
	windows_emulator& operator=(windows_emulator&&) = delete;
	windows_emulator& operator=(const windows_emulator&) = delete;

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

	void serialize(utils::buffer_serializer& buffer) const;
	void deserialize(utils::buffer_deserializer& buffer);

	void save_snapshot();
	void restore_snapshot();

	void set_verbose(const bool verbose)
	{
		this->verbose_ = verbose;
	}

private:
	bool verbose_{false};
	std::unique_ptr<x64_emulator> emu_{};

	process_context process_;
	syscall_dispatcher dispatcher_;

	std::vector<std::byte> process_snapshot_{};
	//std::optional<process_context> process_snapshot_{};

	void setup_hooks();
	void setup_process(const std::filesystem::path& application, const std::vector<std::wstring>& arguments);
};

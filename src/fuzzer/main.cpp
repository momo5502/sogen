#include "std_include.hpp"

#include <windows_emulator.hpp>
#include <fuzzer.hpp>

#include "utils/finally.hpp"

bool use_gdb = false;

namespace
{
	void run_emulation(windows_emulator& win_emu)
	{
		try
		{
			win_emu.logger.disable_output(true);
			win_emu.emu().start_from_ip();
		}
		catch (...)
		{
			win_emu.logger.disable_output(false);
			win_emu.logger.print(color::red, "Emulation failed at: 0x%llX\n", win_emu.emu().read_instruction_pointer());
			throw;
		}

		win_emu.logger.disable_output(false);
	}

	void forward_emulator(windows_emulator& win_emu)
	{
		win_emu.emu().hook_memory_execution(0x140001000, 1, [&](uint64_t, size_t, uint64_t)
		{
			win_emu.emu().stop();
		});

		run_emulation(win_emu);
	}

	struct fuzzer_executer : fuzzer::executer
	{
		windows_emulator emu{};
		const std::function<fuzzer::coverage_functor>* handler{nullptr};


		fuzzer_executer(std::span<const std::byte> data)
		{
			utils::buffer_deserializer deserializer{data};
			emu.deserialize(deserializer);
			//emu.save_snapshot();

			emu.emu().hook_edge_generation([&](const basic_block& current_block,
			                                             const basic_block&)
			{
				if (this->handler)
				{
					(*this->handler)(current_block.address);
				}
			});
		}

		fuzzer::execution_result execute(std::span<const uint8_t> data,
		                                 const std::function<fuzzer::coverage_functor>& coverage_handler) override
		{
			printf("Input size: %zd\n", data.size());
			this->handler = &coverage_handler;

			utils::buffer_serializer serializer{};
			emu.serialize(serializer);

			const auto _ = utils::finally([&]
			{
				utils::buffer_deserializer deserializer{serializer.get_buffer()};
				emu.deserialize(deserializer);
			});

			//emu.restore_snapshot();

			const auto memory = emu.emu().allocate_memory(page_align_up(std::max(data.size(), 1ULL)),
			                                              memory_permission::read_write);
			emu.emu().write_memory(memory, data.data(), data.size());

			emu.emu().reg(x64_register::rcx, memory);
			emu.emu().reg<uint64_t>(x64_register::rdx, data.size());

			try
			{
				run_emulation(emu);
				return fuzzer::execution_result::success;
			}
			catch (...)
			{
				return fuzzer::execution_result::error;
			}
		}
	};

	struct my_fuzzer_handler : fuzzer::handler
	{
		std::vector<std::byte> emulator_state{};
		std::atomic_bool stop_fuzzing{false};

		my_fuzzer_handler(std::vector<std::byte> emulator_state)
			: emulator_state(std::move(emulator_state))
		{
		}

		std::unique_ptr<fuzzer::executer> make_executer() override
		{
			return std::make_unique<fuzzer_executer>(emulator_state);
		}

		bool stop() override
		{
			return stop_fuzzing;
		}
	};

	void run_fuzzer(const windows_emulator& base_emulator)
	{
		const auto concurrency = std::thread::hardware_concurrency();

		utils::buffer_serializer serializer{};
		base_emulator.serialize(serializer);

		my_fuzzer_handler handler{serializer.move_buffer()};

		fuzzer::run(handler, concurrency);
	}

	void run(const std::string_view application)
	{
		windows_emulator win_emu{
			application, {}
		};

		forward_emulator(win_emu);
		run_fuzzer(win_emu);
	}
}

int main(const int argc, char** argv)
{
	if (argc <= 1)
	{
		puts("Application not specified!");
		return 1;
	}

	//setvbuf(stdout, nullptr, _IOFBF, 0x10000);
	if (argc > 2 && argv[1] == "-d"s)
	{
		use_gdb = true;
	}

	try
	{
		do
		{
			run(argv[use_gdb ? 2 : 1]);
		}
		while (use_gdb);

		return 0;
	}
	catch (std::exception& e)
	{
		puts(e.what());

#if defined(_WIN32) && 0
		MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
#endif
	}

	return 1;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
	return main(__argc, __argv);
}
#endif

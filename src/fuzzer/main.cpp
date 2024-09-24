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
		win_emu.logger.print(color::red, "Emulation terminated!\n");
	}

	void forward_emulator(windows_emulator& win_emu)
	{
		win_emu.emu().hook_memory_execution(0x140001000, 1, [&](uint64_t, size_t, uint64_t)
		{
			win_emu.emu().stop();
		});

		run_emulation(win_emu);
	}

	std::vector<std::unique_ptr<windows_emulator>> prepare_emulators(const size_t count,
	                                                                 const windows_emulator& base_emulator)
	{
		std::vector<std::unique_ptr<windows_emulator>> emulators{};

		utils::buffer_serializer serializer{};
		base_emulator.serialize(serializer);

		for (size_t i = 0; i < count; ++i)
		{
			auto emu = std::make_unique<windows_emulator>();
			utils::buffer_deserializer deserializer{serializer.get_buffer()};

			emu->deserialize(deserializer);
			//emu->save_snapshot();

			emulators.push_back(std::move(emu));
		}

		return emulators;
	}

	struct my_fuzzer_handler : fuzzer::fuzzing_handler
	{
		const std::vector<std::unique_ptr<windows_emulator>>* emulators{};
		std::atomic_size_t active_emu{0};
		std::atomic_bool stop_fuzzing{false};

		fuzzer::execution_result execute(std::span<const uint8_t> data,
		                                 const std::function<fuzzer::coverage_functor>& coverage_handler) override
		{
			puts("Running...");
			const auto emu_index = ++active_emu;
			auto& emu = *emulators->at(emu_index % emulators->size());

			utils::buffer_serializer serializer{};
			emu.serialize(serializer);

			const auto _ = utils::finally([&]
			{
				utils::buffer_deserializer deserializer{serializer.get_buffer()};
				emu.deserialize(deserializer);
			});

			//emu.restore_snapshot();

			auto* h = emu.emu().hook_edge_generation([&](const basic_block& current_block,
			                                             const basic_block&)
			{
				coverage_handler(current_block.address);
			});

			const auto __ = utils::finally([&]
			{
				emu.emu().delete_hook(h);
			});

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
				stop_fuzzing = true;
				return fuzzer::execution_result::error;
			}
		}

		bool stop() override
		{
			return stop_fuzzing;
		}
	};

	void run_fuzzer(const windows_emulator& base_emulator)
	{
		const auto concurrency = 1ULL; //std::thread::hardware_concurrency();
		const auto emulators = prepare_emulators(concurrency, base_emulator);

		my_fuzzer_handler handler{};
		handler.emulators = &emulators;

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

#include "std_include.hpp"

#include <windows_emulator.hpp>
#include <debugging/x64_gdb_stub_handler.hpp>

#include "object_watching.hpp"

bool use_gdb = false;

namespace
{
	void watch_system_objects(windows_emulator& win_emu)
	{
		watch_object(win_emu, win_emu.process().teb);
		watch_object(win_emu, win_emu.process().peb);
		watch_object(win_emu, win_emu.process().kusd);
		auto* params_hook = watch_object(win_emu, win_emu.process().process_params);

		win_emu.emu().hook_memory_write(win_emu.process().peb.value() + offsetof(PEB, ProcessParameters), 0x8,
		                                [&](const uint64_t address, size_t, const uint64_t value)
		                                {
			                                const auto target_address = win_emu.process().peb.value() + offsetof(
				                                PEB, ProcessParameters);

			                                if (address == target_address)
			                                {
				                                const emulator_object<RTL_USER_PROCESS_PARAMETERS> obj{
					                                win_emu.emu(), value
				                                };

				                                win_emu.emu().delete_hook(params_hook);
				                                params_hook = watch_object(win_emu, obj);
			                                }
		                                });
	}


	void run()
	{
		windows_emulator win_emu {
			R"(C:\Users\mauri\source\repos\ConsoleApplication6\x64\Release\ConsoleApplication6.exe)",
			{
				L"Hello",
				L"World",
			}
		};

		watch_system_objects(win_emu);

		try
		{
			if (use_gdb)
			{
				puts("Launching gdb stub...");

				x64_gdb_stub_handler handler{win_emu.emu()};
				run_gdb_stub(handler, "i386:x86-64", gdb_registers.size(), "0.0.0.0:28960");
			}
			else
			{
				win_emu.emu().start_from_ip();
			}
		}
		catch (...)
		{
			printf("Emulation failed at: %llX\n", win_emu.emu().read_instruction_pointer());
			throw;
		}

		printf("Emulation done.\n");
	}
}

int main(int /*argc*/, char** /*argv*/)
{
	//setvbuf(stdout, nullptr, _IOFBF, 0x10000);

	try
	{
		do
		{
			run();
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

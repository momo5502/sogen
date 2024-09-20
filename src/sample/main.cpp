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

	void run_emulation(windows_emulator& win_emu)
	{
		try
		{
			if (use_gdb)
			{
				const auto* address = "0.0.0.0:28960";
				win_emu.logger.print(color::pink, "Waiting for GDB connection on %s...\n", address);

				x64_gdb_stub_handler handler{win_emu.emu()};
				run_gdb_stub(handler, "i386:x86-64", gdb_registers.size(), address);
			}
			else
			{
				win_emu.emu().start_from_ip();
			}
		}
		catch (...)
		{
			win_emu.logger.print(color::red, "Emulation failed at: 0x%llX\n", win_emu.emu().read_instruction_pointer());
			throw;
		}

		win_emu.logger.print(color::red, "Emulation terminated!\n");
	}

	void run(const std::string_view application)
	{
		windows_emulator win_emu{
			application, {}
		};

		//watch_system_objects(win_emu);


		const auto& exe = *win_emu.process().executable;

		const auto text_start = exe.image_base + 0x1000;
		const auto text_end = exe.image_base + 0x52000;
		constexpr auto scan_size = 0x100;

		win_emu.emu().hook_memory_read(text_start, scan_size, [&](const uint64_t address, size_t, uint64_t)
		{
			const auto rip = win_emu.emu().read_instruction_pointer();
			if (rip >= text_start && rip < text_end)
			{
				win_emu.logger.print(color::green, "Reading from executable .text: 0x%llX at 0x%llX\n", address, rip);
			}
		});

		win_emu.add_syscall_hook([&]
		{
			const auto rip = win_emu.emu().read_instruction_pointer();
			if (rip >= text_start && rip < text_end)
			{
				const auto syscall_id = win_emu.emu().reg(x64_register::eax);
				const auto syscall_name = win_emu.dispatcher().get_syscall_name(syscall_id);

				win_emu.logger.print(color::blue, "Executing inline syscall: %s (0x%X) at 0x%llX\n",
				                     syscall_name.c_str(),
				                     syscall_id, rip);

				if (syscall_name == "NtQueryInformationProcess")
				{
					const auto info_class = win_emu.emu().reg(x64_register::rdx);
					if (info_class == ProcessImageFileNameWin32)
					{
						const auto data = win_emu.emu().reg(x64_register::r8);

						emulator_allocator data_allocator{win_emu.emu(), data, 0x100};
						data_allocator.make_unicode_string(
							L"C:\\Users\\mauri\\source\\repos\\lul\\x64\\Release\\lul.exe");
						win_emu.emu().reg(x64_register::rax, STATUS_SUCCESS);
						return instruction_hook_continuation::skip_instruction;
					}
				}
			}

			return instruction_hook_continuation::run_instruction;
		});

		run_emulation(win_emu);
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

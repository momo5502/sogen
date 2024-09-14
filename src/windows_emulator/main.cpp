#include <gdbstub.h>

#include "std_include.hpp"

#include "emulator_utils.hpp"
#include "process_context.hpp"
#include "syscalls.hpp"

#include "reflect_extension.hpp"
#include <reflect>

#include "windows_emulator.hpp"

#include "debugging/x64_gdb_stub_handler.hpp"


bool use_gdb = false;

namespace
{
	template <typename T>
	class type_info
	{
	public:
		type_info()
		{
			this->type_name_ = reflect::type_name<T>();

			reflect::for_each<T>([this](auto I)
			{
				const auto member_name = reflect::member_name<I, T>();
				const auto member_offset = reflect::offset_of<I, T>();

				this->members_[member_offset] = member_name;
			});
		}

		std::string get_member_name(const size_t offset) const
		{
			size_t last_offset{};
			std::string_view last_member{};

			for (const auto& member : this->members_)
			{
				if (offset == member.first)
				{
					return member.second;
				}

				if (offset < member.first)
				{
					const auto diff = offset - last_offset;
					return std::string(last_member) + "+" + std::to_string(diff);
				}

				last_offset = member.first;
				last_member = member.second;
			}

			return "<N/A>";
		}

		const std::string& get_type_name() const
		{
			return this->type_name_;
		}

	private:
		std::string type_name_{};
		std::map<size_t, std::string> members_{};
	};

	template <typename T>
	emulator_hook* watch_object(windows_emulator& emu, emulator_object<T> object)
	{
		const type_info<T> info{};

		return emu.emu().hook_memory_read(object.value(), object.size(),
		                                  [i = std::move(info), object, &emu](const uint64_t address, size_t, uint64_t)
		                                  {
			                                  const auto rip = emu.emu().read_instruction_pointer();

			                                  const auto offset = address - object.value();
			                                  printf("%s: %llX (%s) at %llX (%s)\n", i.get_type_name().c_str(), offset,
			                                         i.get_member_name(offset).c_str(), rip,
			                                         emu.process().module_manager.find_name(rip));
		                                  });
	}

	void run()
	{
		const std::filesystem::path application =
			R"(C:\Users\mauri\source\repos\ConsoleApplication6\x64\Release\ConsoleApplication6.exe)";
		//R"(C:\Program Files (x86)\Steam\steamapps\common\Hogwarts Legacy\Phoenix\Binaries\Win64\HogwartsLegacy.exe)";

		windows_emulator win_emu{application, {L"Hello", L"World"}};

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

		win_emu.set_verbose(false);

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

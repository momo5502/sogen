#include "std_include.hpp"

#define X86_CODE32 "\x41\x4a" // INC ecx; DEC edx
#define ADDRESS 0x1000000

#include "unicorn.hpp"

namespace
{
	void run()
	{
		int r_ecx = 0x1234; // ECX register
		int r_edx = 0x7890; // EDX register

		printf("Emulate i386 code\n");

		const unicorn uc{UC_ARCH_X86, UC_MODE_32};

		e(uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL));
		e(uc_mem_write(uc, ADDRESS, X86_CODE32, sizeof(X86_CODE32) - 1));

		e(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
		e(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));

		e(uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(X86_CODE32) - 1, 0, 0));

		printf("Emulation done. Below is the CPU context\n");

		e(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
		e(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

		printf(">>> ECX = 0x%x\n", r_ecx);
		printf(">>> EDX = 0x%x\n", r_edx);
	}
}

int main(int /*argc*/, char** /*argv*/)
{
	try
	{
		run();
		return 0;
	}
	catch (std::exception& e)
	{
		puts(e.what());

#ifdef _WIN32
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

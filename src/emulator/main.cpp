#include "std_include.hpp"

#define X86_CODE32 "\x65\x48\x8B\x04\x25\x60\x00\x00\x00" // INC ecx; DEC edx
#define ADDRESS 0x1000000

#define GS_SEGMENT_ADDR 0x6000000ULL
#define GS_SEGMENT_SIZE (20 << 20)  // 20 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_ADDRESS 0x7ffffffde000
#define STACK_SIZE 0x40000

#define KUSD_ADDRESS 0x7ffe0000

#include "unicorn.hpp"

namespace
{
	uint64_t align_down(const uint64_t value, const uint64_t alignment)
	{
		return value & ~(alignment - 1);
	}

	uint64_t align_up(const uint64_t value, const uint64_t alignment)
	{
		return align_down(value + (alignment - 1), alignment);
	}

	template <typename T>
	class unicorn_object
	{
	public:
		unicorn_object() = default;

		unicorn_object(const unicorn& uc, uint64_t address)
			: uc_(&uc)
			  , address_(address)
		{
		}

		uint64_t value() const
		{
			return this->address_;
		}

		T* ptr() const
		{
			return reinterpret_cast<T*>(this->address_);
		}

		template <typename F>
		void access(const F& accessor) const
		{
			T obj{};

			e(uc_mem_read(*this->uc_, this->address_, &obj, sizeof(obj)));

			accessor(obj);

			e(uc_mem_write(*this->uc_, this->address_, &obj, sizeof(obj)));
		}

	private:
		const unicorn* uc_{};
		uint64_t address_{};
	};

	class unicorn_allocator
	{
	public:
		unicorn_allocator(const unicorn& uc, const uint64_t address, const uint64_t size)
			: uc_(&uc)
			  , address_(address)
			  , size_(size)
			  , active_address_(address)
		{
		}

		template <typename T>
		unicorn_object<T> reserve()
		{
			const auto alignment = alignof(T);
			const auto potential_start = align_up(this->active_address_, alignment);
			const auto potential_end = potential_start + sizeof(T);
			const auto total_end = this->address_ + this->size_;

			if (potential_end > total_end)
			{
				throw std::runtime_error("Out of memory");
			}

			this->active_address_ = potential_end;

			return unicorn_object<T>(*this->uc_, potential_start);
		}

	private:
		const unicorn* uc_{};
		const uint64_t address_{};
		const uint64_t size_{};
		uint64_t active_address_{0};
	};

	void setup_stack(const unicorn& uc, uint64_t stack_base, size_t stack_size)
	{
		e(uc_mem_map(uc, stack_base, stack_size, UC_PROT_READ | UC_PROT_WRITE));

		const uint64_t stack_end = stack_base + stack_size;
		e(uc_reg_write(uc, UC_X86_REG_RSP, &stack_end));
	}

	unicorn_allocator setup_gs_segment(const unicorn& uc, const uint64_t segment_base, const uint64_t size)
	{
		const std::array<uint64_t, 2> value = {
			IA32_GS_BASE_MSR,
			segment_base
		};

		e(uc_reg_write(uc, UC_X86_REG_MSR, value.data()));
		e(uc_mem_map(uc, segment_base, size, UC_PROT_READ | UC_PROT_WRITE));

		return {uc, segment_base, size};
	}

	void setup_kusd(const unicorn& uc)
	{
		/* TODO: Fix
		uc_mem_map(uc, KUSD_ADDRESS, sizeof(KUSER_SHARED_DATA), UC_PROT_READ);

		unicorn_object<KUSER_SHARED_DATA> kusd_object{uc, KUSD_ADDRESS};
		*/

		uc_mem_map_ptr(uc, KUSD_ADDRESS, sizeof(KUSER_SHARED_DATA), UC_PROT_READ,
		               reinterpret_cast<void*>(KUSD_ADDRESS));
	}

	void setup_teb_and_peb(const unicorn& uc)
	{
		setup_stack(uc, STACK_ADDRESS, STACK_SIZE);
		auto gs = setup_gs_segment(uc, GS_SEGMENT_ADDR, GS_SEGMENT_SIZE);

		const auto teb_object = gs.reserve<TEB>();
		const auto peb_object = gs.reserve<PEB>();
		const auto ldr_object = gs.reserve<PEB_LDR_DATA>();

		teb_object.access([&](TEB& teb)
		{
			teb.NtTib.StackLimit = reinterpret_cast<void*>(STACK_ADDRESS);
			teb.NtTib.StackBase = reinterpret_cast<void*>((STACK_ADDRESS + STACK_SIZE));
			teb.NtTib.Self = &teb_object.ptr()->NtTib;
			teb.ProcessEnvironmentBlock = peb_object.ptr();
		});

		peb_object.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = nullptr;
			peb.Ldr = ldr_object.ptr();
		});

		ldr_object.access([&](PEB_LDR_DATA& ldr)
		{
			ldr.InLoadOrderModuleList.Flink = &ldr_object.ptr()->InLoadOrderModuleList;
			ldr.InLoadOrderModuleList.Blink = ldr.InLoadOrderModuleList.Flink;

			ldr.InMemoryOrderModuleList.Flink = &ldr_object.ptr()->InMemoryOrderModuleList;
			ldr.InMemoryOrderModuleList.Blink = ldr.InMemoryOrderModuleList.Flink;

			ldr.InInitializationOrderModuleList.Flink = &ldr_object.ptr()->InInitializationOrderModuleList;
			ldr.InInitializationOrderModuleList.Blink = ldr.InInitializationOrderModuleList.Flink;
		});
	}

	void run()
	{
		const unicorn uc{UC_ARCH_X86, UC_MODE_64};

		e(uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL));
		e(uc_mem_write(uc, ADDRESS, X86_CODE32, sizeof(X86_CODE32) - 1));

		setup_kusd(uc);
		setup_teb_and_peb(uc);

		e(uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(X86_CODE32) - 1, 0, 0));

		printf("Emulation done. Below is the CPU context\n");

		uint64_t rax{};
		e(uc_reg_read(uc, UC_X86_REG_RAX, &rax));

		printf(">>> RAX = 0x%llX\n", rax);
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

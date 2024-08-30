#include <gdbstub.h>

#include "std_include.hpp"

#include "emulator_utils.hpp"
#include "process_context.hpp"
#include "syscalls.hpp"

#include "reflect_extension.hpp"
#include <reflect>

#include <unicorn_x64_emulator.hpp>

#include "gdb_stub.hpp"
#include "module_mapper.hpp"

#define GS_SEGMENT_ADDR 0x6000000ULL
#define GS_SEGMENT_SIZE (20 << 20)  // 20 MB

#define IA32_GS_BASE_MSR 0xC0000101

#define STACK_SIZE 0x40000
#define STACK_ADDRESS (0x800000000000 - STACK_SIZE)

#define KUSD_ADDRESS 0x7ffe0000

bool use_gdb = true;

namespace
{
	void setup_stack(x64_emulator& emu, const uint64_t stack_base, const size_t stack_size)
	{
		emu.allocate_memory(stack_base, stack_size, memory_permission::read_write);

		const uint64_t stack_end = stack_base + stack_size;
		emu.reg(x64_register::rsp, stack_end);
	}

	emulator_allocator setup_gs_segment(x64_emulator& emu, const uint64_t segment_base, const uint64_t size)
	{
		struct msr_value
		{
			uint32_t id;
			uint64_t value;
		};

		const msr_value value{
			IA32_GS_BASE_MSR,
			segment_base
		};

		emu.write_register(x64_register::msr, &value, sizeof(value));
		emu.allocate_memory(segment_base, size, memory_permission::read_write);

		return {emu, segment_base, size};
	}

	emulator_object<KUSER_SHARED_DATA> setup_kusd(x64_emulator& emu)
	{
		emu.allocate_memory(KUSD_ADDRESS, page_align_up(sizeof(KUSER_SHARED_DATA)), memory_permission::read);

		const emulator_object<KUSER_SHARED_DATA> kusd_object{emu, KUSD_ADDRESS};
		kusd_object.access([](KUSER_SHARED_DATA& kusd)
		{
			const auto& real_kusd = *reinterpret_cast<KUSER_SHARED_DATA*>(KUSD_ADDRESS);

			memcpy(&kusd, &real_kusd, sizeof(kusd));

			kusd.ImageNumberLow = IMAGE_FILE_MACHINE_I386;
			kusd.ImageNumberHigh = IMAGE_FILE_MACHINE_AMD64;

			memset(&kusd.ProcessorFeatures, 0, sizeof(kusd.ProcessorFeatures));

			// ...
		});

		return kusd_object;
	}

	process_context setup_context(x64_emulator& emu)
	{
		setup_stack(emu, STACK_ADDRESS, STACK_SIZE);
		process_context context{};

		context.kusd = setup_kusd(emu);

		context.gs_segment = setup_gs_segment(emu, GS_SEGMENT_ADDR, GS_SEGMENT_SIZE);

		auto& gs = context.gs_segment;

		context.teb = gs.reserve<TEB>();
		context.peb = gs.reserve<PEB>();
		//context.ldr = gs.reserve<PEB_LDR_DATA>();
		context.process_params = gs.reserve<RTL_USER_PROCESS_PARAMETERS>();

		context.teb.access([&](TEB& teb)
		{
			teb.ClientId.UniqueProcess = reinterpret_cast<HANDLE>(1);
			teb.ClientId.UniqueThread = reinterpret_cast<HANDLE>(2);
			teb.NtTib.StackLimit = reinterpret_cast<void*>(STACK_ADDRESS);
			teb.NtTib.StackBase = reinterpret_cast<void*>((STACK_ADDRESS + STACK_SIZE));
			teb.NtTib.Self = &context.teb.ptr()->NtTib;
			teb.ProcessEnvironmentBlock = context.peb.ptr();
		});

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = nullptr;
			peb.ProcessHeap = nullptr;
			peb.ProcessHeaps = nullptr;
			peb.ProcessParameters = context.process_params.ptr();
		});

		context.process_params.access([&](RTL_USER_PROCESS_PARAMETERS& proc_params)
		{
			proc_params.Length = sizeof(proc_params);
			proc_params.Flags = 0x6001;
			gs.make_unicode_string(proc_params.CurrentDirectory.DosPath, L"C:\\Users\\mauri\\Desktop");
			gs.make_unicode_string(proc_params.ImagePathName, L"C:\\Users\\mauri\\Desktop\\ConsoleApplication6.exe");
			gs.make_unicode_string(proc_params.CommandLine, L"C:\\Users\\mauri\\Desktop\\ConsoleApplication6.exe");
		});

		return context;
	}

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
	void watch_object(x64_emulator& emu, emulator_object<T> object)
	{
		const type_info<T> info{};

		emu.hook_memory_read(object.value(), object.size(),
		                     [i = std::move(info), object](const uint64_t address, size_t)
		                     {
			                     const auto offset = address - object.value();
			                     printf("%s: %llX (%s)\n", i.get_type_name().c_str(), offset,
			                            i.get_member_name(offset).c_str());
		                     });
	}

	enum class gdb_registers
	{
		rax = 0,
		rbx,
		rcx,
		rdx,
		rsi,
		rdi,
		rbp,
		rsp,
		r8,
		r9,
		r10,
		r11,
		r12,
		r13,
		r14,
		r15,

		rip,
		eflags,

		end,
	};

	std::unordered_map<gdb_registers, x64_register> register_map{
		{gdb_registers::rax, x64_register::rax},
		{gdb_registers::rbx, x64_register::rbx},
		{gdb_registers::rcx, x64_register::rcx},
		{gdb_registers::rdx, x64_register::rdx},
		{gdb_registers::rsi, x64_register::rsi},
		{gdb_registers::rdi, x64_register::rdi},
		{gdb_registers::rbp, x64_register::rbp},
		{gdb_registers::rsp, x64_register::rsp},
		{gdb_registers::r8, x64_register::r8},
		{gdb_registers::r9, x64_register::r9},
		{gdb_registers::r10, x64_register::r10},
		{gdb_registers::r11, x64_register::r11},
		{gdb_registers::r12, x64_register::r12},
		{gdb_registers::r13, x64_register::r13},
		{gdb_registers::r14, x64_register::r14},
		{gdb_registers::r15, x64_register::r15},
		{gdb_registers::rip, x64_register::rip},
		{gdb_registers::eflags, x64_register::rflags},
	};

	class scoped_emulator_hook
	{
	public:
		scoped_emulator_hook() = default;

		scoped_emulator_hook(emulator& emu, emulator_hook* hook)
			: emu_(&emu)
			  , hook_(hook)
		{
		}

		~scoped_emulator_hook()
		{
			this->remove();
		}

		scoped_emulator_hook(const scoped_emulator_hook&) = delete;
		scoped_emulator_hook& operator=(const scoped_emulator_hook&) = delete;

		scoped_emulator_hook(scoped_emulator_hook&& obj) noexcept
		{
			this->operator=(std::move(obj));
		}

		scoped_emulator_hook& operator=(scoped_emulator_hook&& obj) noexcept
		{
			if (this != &obj)
			{
				this->remove();
				this->emu_ = obj.emu_;
				this->hook_ = obj.hook_;

				obj.hook_ = {};
			}
			return *this;
		}

		void remove()
		{
			if (this->hook_)
			{
				this->emu_->delete_hook(this->hook_);
				this->hook_ = {};
			}
		}

	private:
		emulator* emu_{};
		emulator_hook* hook_{};
	};

	class x64_gdb_stub_handler : public gdb_stub_handler
	{
	public:
		x64_gdb_stub_handler(x64_emulator& emu)
			: emu_(&emu)
		{
		}

		~x64_gdb_stub_handler() override = default;

		gdb_action cont() override
		{
			this->emu_->start_from_ip();
			return gdb_action::resume;
		}

		gdb_action stepi() override
		{
			this->emu_->start_from_ip({}, 1);
			return gdb_action::resume;
		}

		bool read_reg(const int regno, size_t* value) override
		{
			try
			{
				const auto mapped_register = register_map.at(static_cast<gdb_registers>(regno));
				this->emu_->read_register(mapped_register, value, sizeof(*value));
				return true;
			}
			catch (...)
			{
				*value = 0;
				return true;
			}
		}

		bool write_reg(const int regno, const size_t value) override
		{
			try
			{
				const auto mapped_register = register_map.at(static_cast<gdb_registers>(regno));
				this->emu_->write_register(mapped_register, &value, sizeof(value));
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool read_mem(const size_t addr, const size_t len, void* val) override
		{
			try
			{
				this->emu_->read_memory(addr, val, len);
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool write_mem(const size_t addr, const size_t len, void* val) override
		{
			try
			{
				this->emu_->write_memory(addr, val, len);
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool set_bp(const size_t addr) override
		{
			try
			{
				this->hooks_[addr] = scoped_emulator_hook(*this->emu_, this->emu_->hook_memory_execution(
					                                          addr, 1, [this](uint64_t, size_t)
					                                          {
						                                          this->on_interrupt();
					                                          }));

				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool del_bp(const size_t addr) override
		{
			try
			{
				const auto entry = this->hooks_.find(addr);
				if (entry == this->hooks_.end())
				{
					return false;
				}

				this->hooks_.erase(entry);

				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		void on_interrupt() override
		{
			this->emu_->stop();
		}

	private:
		x64_emulator* emu_{};
		std::unordered_map<size_t, scoped_emulator_hook> hooks_{};
	};

	uint64_t find_exported_function(const std::vector<exported_symbol>& exports, const std::string_view name)
	{
		for (auto& symbol : exports)
		{
			if (symbol.name == name)
			{
				return symbol.address;
			}
		}

		return 0;
	}

	void run()
	{
		const auto emu = unicorn::create_x64_emulator();

		auto context = setup_context(*emu);

		context.executable = map_file(*emu, R"(C:\Users\mauri\Desktop\ConsoleApplication6.exe)");

		context.peb.access([&](PEB& peb)
		{
			peb.ImageBaseAddress = reinterpret_cast<void*>(context.executable.image_base);
		});

		context.ntdll = map_file(*emu, R"(C:\Windows\System32\ntdll.dll)");

		const auto entry1 = find_exported_function(context.ntdll.exports, "LdrInitializeThunk");
		const auto entry2 = find_exported_function(context.ntdll.exports, "RtlUserThreadStart");

		(void)entry1;
		(void)entry2;

		std::unordered_map<uint64_t, std::string> export_remap{};
		for (const auto& symbol : context.ntdll.exports)
		{
			export_remap.try_emplace(symbol.address, symbol.name);
		}

		for (const auto& exp : export_remap)
		{
			auto name = exp.second;
			emu->hook_memory_execution(exp.first, 0,
			                           [n = std::move(name)](const uint64_t address, const size_t)
			                           {
				                           printf("Executing function: %s (%llX)\n", n.c_str(), address);
			                           });
		}

		syscall_dispatcher dispatcher{context.ntdll.exports};

		emu->hook_instruction(x64_hookable_instructions::syscall, [&]
		{
			dispatcher.dispatch(*emu, context);
		});

		watch_object(*emu, context.teb);
		watch_object(*emu, context.peb);
		watch_object(*emu, context.process_params);
		watch_object(*emu, context.kusd);

		/*emu->hook_memory_execution(0, std::numeric_limits<size_t>::max(), [&](const uint64_t address, const size_t)
		{
			if (address == 0x1800D52F4)
			{
				//emu->stop();
			}

			printf(
				"Inst: %16llX - RAX: %16llX - RBX: %16llX - RCX: %16llX - RDX: %16llX - R8: %16llX - R9: %16llX - RDI: %16llX - RSI: %16llX\n",
				address,
				emu->reg(x64_register::rax), emu->reg(x64_register::rbx), emu->reg(x64_register::rcx),
				emu->reg(x64_register::rdx), emu->reg(x64_register::r8), emu->reg(x64_register::r9),
				emu->reg(x64_register::rdi), emu->reg(x64_register::rsi));
		});*/

		const auto execution_context = context.gs_segment.reserve<CONTEXT>();

		emu->reg(x64_register::rcx, execution_context.value());
		emu->reg(x64_register::rdx, context.ntdll.image_base);

		emu->reg(x64_register::rip, entry1);


		try
		{
			if (use_gdb)
			{
				puts("Launching gdb stub...");

				x64_gdb_stub_handler handler{*emu};
				run_gdb_stub(handler, "i386:x86-64", static_cast<size_t>(gdb_registers::end), "0.0.0.0:28960");
			}
			else
			{
				emu->start_from_ip();
			}
		}
		catch (...)
		{
			printf("Emulation failed at: %llX\n", emu->reg(x64_register::rip));
			throw;
		}

		printf("Emulation done.\n");
	}
}

int main(int /*argc*/, char** /*argv*/)
{
	try
	{
		do
		{
			run();
		}
		while (use_gdb);

		return 0;
	}
	catch (std::exception& uce)
	{
		puts(uce.what());

#ifdef _WIN32
		//MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
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

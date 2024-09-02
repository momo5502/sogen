#define UNICORN_EMULATOR_IMPL
#include "unicorn_x64_emulator.hpp"

#include "unicorn_memory_regions.hpp"
#include "unicorn_hook.hpp"

#include "function_wrapper.hpp"

namespace unicorn
{
	namespace
	{
		static_assert(static_cast<uint32_t>(memory_permission::none) == UC_PROT_NONE);
		static_assert(static_cast<uint32_t>(memory_permission::read) == UC_PROT_READ);
		static_assert(static_cast<uint32_t>(memory_permission::exec) == UC_PROT_EXEC);
		static_assert(static_cast<uint32_t>(memory_permission::all) == UC_PROT_ALL);

		static_assert(static_cast<uint32_t>(x64_register::end) == UC_X86_REG_ENDING);

		uc_x86_insn map_hookable_instruction(const x64_hookable_instructions instruction)
		{
			switch (instruction)
			{
			case x64_hookable_instructions::syscall:
				return UC_X86_INS_SYSCALL;
			case x64_hookable_instructions::cpuid:
				return UC_X86_INS_CPUID;
			case x64_hookable_instructions::rdtsc:
				return UC_X86_INS_RDTSC;
			case x64_hookable_instructions::rdtscp:
				return UC_X86_INS_RDTSCP;
			}

			throw std::runtime_error("Bad instruction for mapping");
		}

		memory_operation map_memory_operation(const uc_mem_type mem_type)
		{
			switch (mem_type)
			{
			case UC_MEM_READ:
				return memory_permission::read;
			case UC_MEM_WRITE:
				return memory_permission::write;
			default:
				return memory_permission::none;
			}
		}

		struct hook_object : object
		{
			emulator_hook* as_opaque_hook()
			{
				return reinterpret_cast<emulator_hook*>(this);
			}
		};

		class hook_container : public hook_object
		{
		public:
			template <typename T>
				requires(std::is_base_of_v<object, T>
					&& std::is_move_constructible_v<T>)
			void add(T data, unicorn_hook hook)
			{
				hook_entry entry{};

				entry.data = std::make_unique<T>(std::move(data));
				entry.hook = std::move(hook);

				this->hooks_.emplace_back(std::move(entry));
			}

		private:
			struct hook_entry
			{
				std::unique_ptr<object> data{};
				unicorn_hook hook{};
			};

			std::vector<hook_entry> hooks_;
		};

		void add_read_hook(uc_engine* uc, const uint64_t address, const size_t size, hook_container& container,
		                   const std::shared_ptr<complex_memory_hook_callback>& callback)
		{
			function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(
				[callback](uc_engine*, const uc_mem_type type, const uint64_t address, const int size,
				           const int64_t)
				{
					const auto operation = map_memory_operation(type);
					if (operation != memory_permission::none)
					{
						(*callback)(address, static_cast<uint64_t>(size), operation);
					}
				});

			unicorn_hook hook{uc};

			uce(uc_hook_add(uc, hook.make_reference(), UC_HOOK_MEM_READ, wrapper.get_function(),
			                wrapper.get_user_data(), address, address + size));

			container.add(std::move(wrapper), std::move(hook));
		}

		void add_write_hook(uc_engine* uc, const uint64_t address, const size_t size, hook_container& container,
		                    const std::shared_ptr<complex_memory_hook_callback>& callback)
		{
			function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(
				[callback](uc_engine*, const uc_mem_type type, const uint64_t address, const int size,
				           const int64_t)
				{
					const auto operation = map_memory_operation(type);
					if (operation != memory_permission::none)
					{
						(*callback)(address, static_cast<uint64_t>(size), operation);
					}
				});

			unicorn_hook hook{uc};

			uce(uc_hook_add(uc, hook.make_reference(), UC_HOOK_MEM_WRITE, wrapper.get_function(),
			                wrapper.get_user_data(), address, address + size));

			container.add(std::move(wrapper), std::move(hook));
		}

		void add_exec_hook(uc_engine* uc, const uint64_t address, const size_t size, hook_container& container,
		                   const std::shared_ptr<complex_memory_hook_callback>& callback)
		{
			function_wrapper<void, uc_engine*, uint64_t, uint32_t> wrapper(
				[callback](uc_engine*, const uint64_t address, const uint32_t size)
				{
					(*callback)(address, size, memory_permission::exec);
				});

			unicorn_hook hook{uc};

			uce(uc_hook_add(uc, hook.make_reference(), UC_HOOK_CODE, wrapper.get_function(),
			                wrapper.get_user_data(), address, address + size));

			container.add(std::move(wrapper), std::move(hook));
		}

		class unicorn_x64_emulator : public x64_emulator
		{
		public:
			unicorn_x64_emulator()
			{
				uce(uc_open(UC_ARCH_X86, UC_MODE_64, &this->uc_));
			}

			~unicorn_x64_emulator() override
			{
				this->hooks_.clear();
				uc_close(this->uc_);
			}

			void start(const uint64_t start, const uint64_t end, std::chrono::microseconds timeout,
			           const size_t count) override
			{
				if (timeout.count() < 0)
				{
					timeout = {};
				}

				uce(uc_emu_start(*this, start, end, static_cast<uint64_t>(timeout.count()), count));
			}

			void stop() override
			{
				uce(uc_emu_stop(*this));
			}

			void write_raw_register(const int reg, const void* value, const size_t size) override
			{
				size_t result_size = size;
				uce(uc_reg_write2(*this, reg, value, &result_size));

				if (size != result_size)
				{
					throw std::runtime_error(
						"Register size mismatch: " + std::to_string(size) + " != " + std::to_string(result_size));
				}
			}

			void read_raw_register(const int reg, void* value, const size_t size) override
			{
				size_t result_size = size;
				uce(uc_reg_read2(*this, reg, value, &result_size));

				if (size != result_size)
				{
					throw std::runtime_error(
						"Register size mismatch: " + std::to_string(size) + " != " + std::to_string(result_size));
				}
			}

			void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
			{
				uce(uc_mem_map(*this, address, size, static_cast<uint32_t>(permissions)));
			}

			void unmap_memory(const uint64_t address, const size_t size) override
			{
				uce(uc_mem_unmap(*this, address, size));
			}

			bool try_read_memory(const uint64_t address, void* data, const size_t size) override
			{
				return uc_mem_read(*this, address, data, size) == UC_ERR_OK;
			}

			void read_memory(const uint64_t address, void* data, const size_t size) override
			{
				uce(uc_mem_read(*this, address, data, size));
			}

			void write_memory(const uint64_t address, const void* data, const size_t size) override
			{
				uce(uc_mem_write(*this, address, data, size));
			}

			void apply_memory_protection(const uint64_t address, const size_t size,
			                             memory_permission permissions) override
			{
				uce(uc_mem_protect(*this, address, size, static_cast<uint32_t>(permissions)));
			}

			/*std::vector<memory_region> get_memory_regions() override
			{
				const unicorn_memory_regions regions{*this};
				const auto region_span = regions.get_span();

				std::vector<memory_region> result{};
				result.reserve(region_span.size());

				for (const auto region : region_span)
				{
					memory_region reg{};
					reg.start = region.begin;
					reg.length = region.end - region.begin;
					reg.committed = true;
					reg.pemissions = static_cast<memory_permission>(region.perms) & memory_permission::all;

					result.push_back(reg);
				}

				return result;
			}*/

			emulator_hook* hook_instruction(int instruction_type,
			                                hook_callback callback)
			{
				const auto uc_instruction = map_hookable_instruction(
					static_cast<x64_hookable_instructions>(instruction_type));

				function_wrapper<int, uc_engine*> wrapper([c = std::move(callback)](uc_engine*)
				{
					return c() ? 1 : 0;
				});

				unicorn_hook hook{*this};

				uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN, wrapper.get_function(),
				                wrapper.get_user_data(), 0, std::numeric_limits<pointer_type>::max(), uc_instruction));

				auto container = std::make_unique<hook_container>();
				container->add(std::move(wrapper), std::move(hook));

				auto* result = container->as_opaque_hook();

				this->hooks_.push_back(std::move(container));

				return result;
			}

			emulator_hook* hook_memory_access(const uint64_t address, const size_t size, const memory_operation filter,
			                                  complex_memory_hook_callback callback) override
			{
				if (filter == memory_permission::none)
				{
					return nullptr;
				}

				const auto shared_callback = std::make_shared<complex_memory_hook_callback>(std::move(callback));

				auto container = std::make_unique<hook_container>();

				if ((filter & memory_operation::read) != memory_operation::none)
				{
					add_read_hook(*this, address, size, *container, shared_callback);
				}

				if ((filter & memory_operation::write) != memory_operation::none)
				{
					add_write_hook(*this, address, size, *container, shared_callback);
				}

				if ((filter & memory_operation::exec) != memory_operation::none)
				{
					add_exec_hook(*this, address, size, *container, shared_callback);
				}

				auto* result = container->as_opaque_hook();

				this->hooks_.push_back(std::move(container));

				return result;
			}

			void delete_hook(emulator_hook* hook) override
			{
				const auto entry = std::ranges::find_if(this->hooks_, [&](const std::unique_ptr<hook_object>& hook_ptr)
				{
					return hook_ptr->as_opaque_hook() == hook;
				});

				if (entry != this->hooks_.end())
				{
					this->hooks_.erase(entry);
				}
			}

			operator uc_engine*() const
			{
				return this->uc_;
			}

		private:
			uc_engine* uc_{};
			std::vector<std::unique_ptr<hook_object>> hooks_{};
		};
	}

	std::unique_ptr<x64_emulator> create_x64_emulator()
	{
		return std::make_unique<unicorn_x64_emulator>();
	}
}

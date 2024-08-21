#define UNICORN_EMULATOR_IMPL
#include "unicorn_x64_emulator.hpp"

#include <span>
#include <unicorn/unicorn.h>

namespace unicorn
{
	namespace
	{
		static_assert(static_cast<uint32_t>(memory_permission::none) == UC_PROT_NONE);
		static_assert(static_cast<uint32_t>(memory_permission::read) == UC_PROT_READ);
		static_assert(static_cast<uint32_t>(memory_permission::exec) == UC_PROT_EXEC);
		static_assert(static_cast<uint32_t>(memory_permission::all) == UC_PROT_ALL);

		static_assert(static_cast<uint32_t>(x64_register::end) == UC_X86_REG_ENDING);

		struct unicorn_error : std::runtime_error
		{
			unicorn_error(const uc_err error_code)
				: std::runtime_error(uc_strerror(error_code))
				  , code(error_code)
			{
			}

			uc_err code{};
		};

		void throw_if_unicorn_error(const uc_err error_code)
		{
			if (error_code != UC_ERR_OK)
			{
				throw unicorn_error(error_code);
			}
		}

		void uce(const uc_err error_code)
		{
			throw_if_unicorn_error(error_code);
		}

		class unicorn_memory_regions
		{
		public:
			unicorn_memory_regions(uc_engine* uc)
			{
				uce(uc_mem_regions(uc, &this->regions_, &this->count_));
			}

			unicorn_memory_regions(unicorn_memory_regions&&) = delete;
			unicorn_memory_regions(const unicorn_memory_regions&) = delete;
			unicorn_memory_regions& operator=(unicorn_memory_regions&&) = delete;
			unicorn_memory_regions& operator=(const unicorn_memory_regions&) = delete;

			~unicorn_memory_regions()
			{
				if (regions_)
				{
					uc_free(regions_);
				}
			}

			std::span<uc_mem_region> get_span() const
			{
				return {this->regions_, this->count_};
			}

		private:
			uint32_t count_{};
			uc_mem_region* regions_{};
		};

		class unicorn_x64_emulator : public x64_emulator
		{
		public:
			unicorn_x64_emulator()
			{
				uce(uc_open(UC_ARCH_X86, UC_MODE_64, &this->uc_));
			}

			~unicorn_x64_emulator() override
			{
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

			void read_memory(const uint64_t address, void* data, const size_t size) override
			{
				uce(uc_mem_read(*this, address, data, size));
			}

			void write_memory(const uint64_t address, const void* data, const size_t size) override
			{
				uce(uc_mem_write(*this, address, data, size));
			}

			void protect_memory(const uint64_t address, const size_t size, memory_permission permissions) override
			{
				uce(uc_mem_protect(*this, address, size, static_cast<uint32_t>(permissions)));
			}

			std::vector<memory_region> get_memory_regions() override
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
					reg.pemissions = static_cast<memory_permission>(region.perms) & memory_permission::all;

					result.push_back(reg);
				}

				return result;
			}

			operator uc_engine*() const
			{
				return this->uc_;
			}

		private:
			uc_engine* uc_{};
		};
	}

	std::unique_ptr<x64_emulator> create_x64_emulator()
	{
		return std::make_unique<unicorn_x64_emulator>();
	}
}

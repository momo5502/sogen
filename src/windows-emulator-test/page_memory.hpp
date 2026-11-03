#pragma once

#include <memory_interface.hpp>

#include <array>
#include <map>
#include <stdexcept>

namespace sogen::test
{
    class page_memory final : public memory_interface
    {
      public:
        void read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            if (!this->try_read_memory(address, data, size))
            {
                throw std::runtime_error{"unmapped guest read"};
            }
        }

        bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            auto* destination = static_cast<std::byte*>(data);

            for (size_t i = 0; i < size; ++i)
            {
                const auto page = this->pages_.find(page_base(address + i));
                if (page == this->pages_.end())
                {
                    return false;
                }

                destination[i] = page->second[page_offset(address + i)];
            }

            return true;
        }

        void write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            if (!this->try_write_memory(address, data, size))
            {
                throw std::runtime_error{"unmapped guest write"};
            }
        }

        bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            const auto* source = static_cast<const std::byte*>(data);

            for (size_t i = 0; i < size; ++i)
            {
                const auto page = this->pages_.find(page_base(address + i));
                if (page == this->pages_.end())
                {
                    return false;
                }

                page->second[page_offset(address + i)] = source[i];
            }

            return true;
        }

      private:
        static constexpr size_t page_size = 0x1000;

        static uint64_t page_base(const uint64_t address)
        {
            return address & ~static_cast<uint64_t>(page_size - 1);
        }

        static size_t page_offset(const uint64_t address)
        {
            return static_cast<size_t>(address & (page_size - 1));
        }

        void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
        {
        }

        void map_memory(const uint64_t address, const size_t size, memory_permission) override
        {
            for (uint64_t offset = 0; offset < size; offset += page_size)
            {
                this->pages_.try_emplace(page_base(address + offset));
            }
        }

        void unmap_memory(const uint64_t address, const size_t size) override
        {
            for (uint64_t offset = 0; offset < size; offset += page_size)
            {
                this->pages_.erase(page_base(address + offset));
            }
        }

        void apply_memory_protection(uint64_t, size_t, memory_permission) override
        {
        }

        std::map<uint64_t, std::array<std::byte, page_size>> pages_{};
    };
}

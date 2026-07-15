#include "emulation_test_utils.hpp"
#include "module/module_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sogen::test
{
    class sparse_memory final : public memory_interface
    {
      public:
        void add(const uint64_t address, const void* data, const size_t size)
        {
            std::vector<std::byte> bytes(size);
            std::memcpy(bytes.data(), data, size);
            this->regions.emplace(address, std::move(bytes));
        }

        size_t read_count() const
        {
            return this->read_count_;
        }

        size_t largest_read() const
        {
            return this->largest_read_;
        }

        void read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            if (!this->try_read_memory(address, data, size))
            {
                throw std::out_of_range{"unreadable sparse guest memory"};
            }
        }

        bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            ++this->read_count_;
            this->largest_read_ = std::max(this->largest_read_, size);

            const auto entry = this->regions.upper_bound(address);
            if (entry == this->regions.begin())
            {
                return false;
            }

            const auto& bytes = std::prev(entry)->second;
            const auto region_address = std::prev(entry)->first;
            const auto offset = address - region_address;
            if (offset > bytes.size() || size > bytes.size() - offset)
            {
                return false;
            }

            std::memcpy(data, bytes.data() + offset, size);
            return true;
        }

        void write_memory(uint64_t, const void*, size_t) override
        {
        }

        bool try_write_memory(uint64_t, const void*, size_t) override
        {
            return false;
        }

      private:
        void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
        {
        }

        void map_memory(uint64_t, size_t, memory_permission) override
        {
        }

        void unmap_memory(uint64_t, size_t) override
        {
        }

        void apply_memory_protection(uint64_t, size_t, memory_permission) override
        {
        }

        std::map<uint64_t, std::vector<std::byte>> regions;
        mutable size_t read_count_{0};
        mutable size_t largest_read_{0};
    };

    TEST(MinidumpMemoryModuleTest, DetectsSparsePe32AndPe64ThroughGuestMemory)
    {
        constexpr uint64_t base_address = 0x700000000000ULL;
        constexpr uint32_t nt_offset = 0x1800;
        constexpr uint64_t image_size = 0x2000;

        for (const auto [magic, expected_arch] : {
                 std::pair{static_cast<uint16_t>(PEOptionalHeader_t<uint32_t>::k_Magic), winpe::pe_arch::pe32},
                 std::pair{static_cast<uint16_t>(PEOptionalHeader_t<uint64_t>::k_Magic), winpe::pe_arch::pe64},
             })
        {
            sparse_memory memory;
            PEDosHeader_t dos_header{};
            dos_header.e_magic = PEDosHeader_t::k_Magic;
            dos_header.e_lfanew = nt_offset;
            memory.add(base_address, &dos_header, sizeof(dos_header));

            const uint32_t signature = PENTHeaders_t<uint32_t>::k_Signature;
            memory.add(base_address + nt_offset, &signature, sizeof(signature));
            PEFileHeader_t file_header{};
            memory.add(base_address + nt_offset + sizeof(signature), &file_header, sizeof(file_header));
            memory.add(base_address + nt_offset + sizeof(signature) + sizeof(file_header), &magic, sizeof(magic));

            const auto result = pe_architecture_detector::detect_from_memory(memory, base_address, image_size);

            ASSERT_TRUE(result.is_valid());
            ASSERT_EQ(result.architecture, expected_arch);
            EXPECT_EQ(result.suggested_mode, pe_architecture_detector::determine_execution_mode(expected_arch));
            EXPECT_EQ(memory.read_count(), 4u);
            EXPECT_LE(memory.largest_read(), sizeof(PEDosHeader_t));
        }
    }

    TEST(MinidumpMemoryModuleTest, RejectsUnreadableAndOutOfRangeHeaders)
    {
        constexpr uint64_t base_address = 0x700000000000ULL;
        constexpr uint32_t nt_offset = 0x1800;

        sparse_memory unreadable_memory;
        PEDosHeader_t dos_header{};
        dos_header.e_magic = PEDosHeader_t::k_Magic;
        dos_header.e_lfanew = nt_offset;
        unreadable_memory.add(base_address, &dos_header, sizeof(dos_header));
        EXPECT_FALSE(pe_architecture_detector::detect_from_memory(unreadable_memory, base_address, 0x2000).is_valid());

        sparse_memory out_of_range_memory;
        const uint32_t signature = PENTHeaders_t<uint64_t>::k_Signature;
        out_of_range_memory.add(base_address, &dos_header, sizeof(dos_header));
        out_of_range_memory.add(base_address + nt_offset, &signature, sizeof(signature));
        EXPECT_FALSE(pe_architecture_detector::detect_from_memory(out_of_range_memory, base_address, 0x1000).is_valid());
    }

    TEST(MinidumpMemoryModuleTest, RejectsAddressOverflow)
    {
        constexpr uint64_t base_address = std::numeric_limits<uint64_t>::max() - 128;
        sparse_memory memory;
        PEDosHeader_t dos_header{};
        dos_header.e_magic = PEDosHeader_t::k_Magic;
        dos_header.e_lfanew = 128;
        memory.add(base_address, &dos_header, sizeof(dos_header));

        const auto result = pe_architecture_detector::detect_from_memory(memory, base_address, 0x1000);

        EXPECT_FALSE(result.is_valid());
        EXPECT_EQ(memory.read_count(), 1u);
    }
} // namespace sogen::test

#include <std_include.hpp>
#include <memory_manager.hpp>

#include <gtest/gtest.h>

namespace sogen::test
{
    namespace
    {
        constexpr size_t k_size = 0x2000;

        class permission_recorder final : public memory_interface
        {
          public:
            void read_memory(uint64_t, void*, size_t) const override
            {
            }

            bool try_read_memory(uint64_t, void*, size_t) const override
            {
                return true;
            }

            void write_memory(uint64_t, const void*, size_t) override
            {
            }

            bool try_write_memory(uint64_t, const void*, size_t) override
            {
                return true;
            }

            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
            }

            void map_memory(const uint64_t address, size_t, const memory_permission permissions) override
            {
                this->applied[address] = permissions;
            }

            void unmap_memory(const uint64_t address, size_t) override
            {
                this->applied.erase(address);
            }

            void apply_memory_protection(const uint64_t address, size_t, const memory_permission permissions) override
            {
                this->applied[address] = permissions;
            }

            std::map<uint64_t, memory_permission> applied{};
        };
    }

    // Windows reports PAGE_EXECUTE for an image section without IMAGE_SCN_MEM_READ, yet x86 code can still read it.
    TEST(ExecuteOnlyMemoryTest, X86ExecuteOnlyPagesStayReadableButReportExecuteOnly)
    {
        permission_recorder backend{};
        memory_manager memory{backend};
        memory.set_execute_implies_read(true);

        const auto address = memory.allocate_memory(k_size, memory_permission::exec);
        ASSERT_NE(0u, address);

        EXPECT_EQ(memory_permission::read | memory_permission::exec, backend.applied.at(address));
        EXPECT_EQ(memory_permission::exec, memory.get_region_info(address).permissions.common);
    }

    TEST(ExecuteOnlyMemoryTest, ExecuteOnlyPagesStayUnreadableWithoutTheX86Rule)
    {
        permission_recorder backend{};
        memory_manager memory{backend};

        const auto address = memory.allocate_memory(k_size, memory_permission::exec);
        ASSERT_NE(0u, address);

        EXPECT_EQ(memory_permission::exec, backend.applied.at(address));
    }
}

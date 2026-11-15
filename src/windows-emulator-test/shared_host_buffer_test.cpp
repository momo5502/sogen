#include <gtest/gtest.h>

#include <utils/shared_host_buffer.hpp>

#include <cstring>
#include <vector>

namespace sogen::test
{
    namespace
    {
        TEST(SharedHostBufferTest, NameStaysOpenableWhileAnyHolderRemains)
        {
            std::optional<utils::shared_host_buffer> creator = utils::shared_host_buffer::create_named(0x1000);
            const auto name = creator->name();
            if (name.empty())
            {
                GTEST_SKIP() << "This host has no named shared memory";
            }
            std::memcpy(creator->data(), "shared", 7);

            auto holder = utils::shared_host_buffer::open_named(name, 0x1000);
            ASSERT_TRUE(holder.has_value());

            creator.reset();

            const auto reopened = utils::shared_host_buffer::open_named(name, 0x1000);
            ASSERT_TRUE(reopened.has_value());
            EXPECT_STREQ("shared", reinterpret_cast<const char*>(reopened->data()));
        }

        TEST(SharedHostBufferTest, NameIsGoneOnceTheLastHolderReleases)
        {
            std::string name{};
            {
                const auto creator = utils::shared_host_buffer::create_named(0x1000);
                name = creator.name();
                if (name.empty())
                {
                    GTEST_SKIP() << "This host has no named shared memory";
                }
                const auto holder = utils::shared_host_buffer::open_named(name, 0x1000);
                ASSERT_TRUE(holder.has_value());
            }

            EXPECT_FALSE(utils::shared_host_buffer::open_named(name, 0x1000).has_value());
        }

        TEST(SharedHostBufferTest, OneProcessCanHoldManyMappingsOfAName)
        {
            const auto creator = utils::shared_host_buffer::create_named(0x1000);
            if (creator.name().empty())
            {
                GTEST_SKIP() << "This host has no named shared memory";
            }

            std::vector<utils::shared_host_buffer> holders{};
            for (size_t i = 0; i < 100; ++i)
            {
                auto holder = utils::shared_host_buffer::open_named(creator.name(), 0x1000);
                ASSERT_TRUE(holder.has_value()) << i;
                holders.push_back(std::move(*holder));
            }
        }
    }
}

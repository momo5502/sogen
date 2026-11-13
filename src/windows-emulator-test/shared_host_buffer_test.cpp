#include <gtest/gtest.h>

#include <utils/shared_host_buffer.hpp>

#include <cstring>

namespace sogen::test
{
    namespace
    {
#ifndef __EMSCRIPTEN__
        TEST(SharedHostBufferTest, NameStaysOpenableWhileAnyHolderRemains)
        {
            std::optional<utils::shared_host_buffer> creator = utils::shared_host_buffer::create_named(0x1000);
            const auto name = creator->name();
            ASSERT_FALSE(name.empty());
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
                const auto holder = utils::shared_host_buffer::open_named(name, 0x1000);
                ASSERT_TRUE(holder.has_value());
            }

            EXPECT_FALSE(utils::shared_host_buffer::open_named(name, 0x1000).has_value());
        }
#endif
    }
}

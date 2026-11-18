#include "std_include.hpp"

#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
        TEST(SharedHostBufferTest, TheNameOfABufferWhoseHoldersAllDiedIsRemoved)
        {
            int name_pipe[2]{};
            ASSERT_EQ(0, ::pipe(name_pipe));

            const auto child = ::fork();
            ASSERT_GE(child, 0);
            if (child == 0)
            {
                // Dies holding the buffer, without the release that would unlink its name.
                static std::optional<utils::shared_host_buffer> buffer{};
                buffer = utils::shared_host_buffer::create_named(0x1000);
                const auto& name = buffer->name();
                (void)!::write(name_pipe[1], name.c_str(), name.size() + 1);
                ::_exit(0);
            }

            char name[256]{};
            const auto name_length = ::read(name_pipe[0], name, sizeof(name) - 1);
            ::close(name_pipe[0]);
            ::close(name_pipe[1]);
            ASSERT_EQ(child, ::waitpid(child, nullptr, 0));
            if (name_length <= 1)
            {
                GTEST_SKIP() << "This host has no named shared memory";
            }

            EXPECT_FALSE(utils::shared_host_buffer::open_named(name, 0x1000).has_value());

            const auto descriptor = ::shm_open(name, O_RDWR, 0600);
            EXPECT_LT(descriptor, 0);
            if (descriptor >= 0)
            {
                ::close(descriptor);
                ::shm_unlink(name);
            }
        }
#endif
    }
}

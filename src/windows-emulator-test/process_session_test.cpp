#ifndef __EMSCRIPTEN__

#include <subprocess_process_manager.hpp>
#include <gtest/gtest.h>

namespace sogen::test
{
    namespace
    {
        auto unused_arguments(uint16_t, const std::string&)
        {
            return std::vector<std::string>{};
        }
    }

    TEST(ProcessSession, GrandchildQueriesAncestorExitStatusThroughAnExistingHandle)
    {
        subprocess_process_manager launcher{"unused", unused_arguments};
        launcher.register_current_process({.process_id = 100, .native_environment = 0x180000, .compatibility_environment = 0x280000});
        subprocess_process_manager child{"unused", unused_arguments, create_subprocess_launcher(), launcher.session_endpoint()};
        child.register_current_process({.process_id = 200, .parent_process_id = 100});
        subprocess_process_manager grandchild{"unused", unused_arguments, create_subprocess_launcher(), child.session_endpoint()};
        grandchild.register_current_process({.process_id = 300, .parent_process_id = 200});

        const auto ancestor = grandchild.open_process(100);
        ASSERT_TRUE(ancestor);
        EXPECT_EQ(100u, ancestor.process_id);
        EXPECT_EQ(0u, ancestor.parent_process_id);
        EXPECT_EQ(0x180000u, ancestor.native_environment);
        EXPECT_EQ(0x280000u, ancestor.compatibility_environment);
        const auto active = grandchild.exit_status(ancestor.process);
        ASSERT_TRUE(active);
        EXPECT_FALSE(active.status.has_value());
        EXPECT_EQ(process_error::unavailable, grandchild.terminate_process(ancestor.process, 5));

        launcher.notify_current_process_exit(37);
        const auto exited = grandchild.exit_status(ancestor.process);
        ASSERT_TRUE(exited);
        ASSERT_TRUE(exited.status.has_value());
        EXPECT_EQ(process_exit_kind::exited, exited.status->kind);
        EXPECT_EQ(37u, exited.status->code);
        EXPECT_EQ(process_error::invalid_process, grandchild.open_process(999).error);
    }

    TEST(ProcessSession, IndependentLaunchSessionsDoNotShareProcessIds)
    {
        subprocess_process_manager first{"unused", unused_arguments};
        first.register_current_process({.process_id = 100});
        subprocess_process_manager second{"unused", unused_arguments};
        second.register_current_process({.process_id = 200});
        EXPECT_EQ(process_error::invalid_process, first.open_process(200).error);
        EXPECT_EQ(process_error::invalid_process, second.open_process(100).error);
    }

    TEST(ProcessSession, FailedProcessIsNotReportedAsStillActive)
    {
        subprocess_process_manager launcher{"unused", unused_arguments};
        launcher.register_current_process({.process_id = 100});
        auto child = std::make_unique<subprocess_process_manager>("unused", unused_arguments, create_subprocess_launcher(),
                                                                  launcher.session_endpoint());
        child->register_current_process({.process_id = 200, .parent_process_id = 100});
        const auto process = launcher.open_process(200);
        ASSERT_TRUE(process);
        child.reset();
        const auto status = launcher.exit_status(process.process);
        ASSERT_TRUE(status);
        ASSERT_TRUE(status.status);
        EXPECT_EQ(process_exit_kind::runtime_failure, status.status->kind);
        EXPECT_EQ(1u, status.status->code);
    }
}

#endif

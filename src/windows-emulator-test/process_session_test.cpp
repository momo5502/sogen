#ifndef __EMSCRIPTEN__

#include <subprocess_process_manager.hpp>
#include <process_session.hpp>

#include <array>
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
        launcher.initialize_current_process({.process_id = 100, .native_environment = 0x180000, .compatibility_environment = 0x280000});
        process_session registry{launcher.session_endpoint()};
        const process_identity child_identity{.process_id = 200, .parent_process_id = 100};
        ASSERT_EQ(process_error::none, registry.register_process(child_identity));
        subprocess_process_manager child{"unused", unused_arguments, create_subprocess_launcher(), launcher.session_endpoint()};
        child.initialize_current_process(child_identity);
        const process_identity grandchild_identity{.process_id = 300, .parent_process_id = 200};
        ASSERT_EQ(process_error::none, registry.register_process(grandchild_identity));
        subprocess_process_manager grandchild{"unused", unused_arguments, create_subprocess_launcher(), child.session_endpoint()};
        grandchild.initialize_current_process(grandchild_identity);

        const auto ancestor = grandchild.open_process(100);
        ASSERT_TRUE(ancestor);
        EXPECT_EQ(100u, ancestor.process.identity.process_id);
        EXPECT_EQ(0u, ancestor.process.identity.parent_process_id);
        EXPECT_EQ(0x180000u, ancestor.process.identity.native_environment);
        EXPECT_EQ(0x280000u, ancestor.process.identity.compatibility_environment);
        const auto active = grandchild.exit_status(ancestor.process.handle);
        ASSERT_TRUE(active);
        EXPECT_FALSE(active.status.has_value());
        EXPECT_EQ(process_error::unavailable, grandchild.terminate_process(ancestor.process.handle, 5));

        launcher.notify_current_process_exit(37);
        const auto exited = grandchild.exit_status(ancestor.process.handle);
        ASSERT_TRUE(exited);
        ASSERT_TRUE(exited.status.has_value());
        EXPECT_EQ(process_exit_kind::exited, exited.status->kind);
        EXPECT_EQ(37u, exited.status->code);
        EXPECT_EQ(process_error::invalid_process, grandchild.open_process(999).error);
    }

    TEST(ProcessSession, IndependentLaunchSessionsDoNotShareProcessIds)
    {
        subprocess_process_manager first{"unused", unused_arguments};
        first.initialize_current_process({.process_id = 100});
        subprocess_process_manager second{"unused", unused_arguments};
        second.initialize_current_process({.process_id = 200});
        EXPECT_EQ(process_error::invalid_process, first.open_process(200).error);
        EXPECT_EQ(process_error::invalid_process, second.open_process(100).error);
    }

    TEST(ProcessSession, FailedProcessIsNotReportedAsStillActive)
    {
        process_session session{};
        ASSERT_EQ(process_error::none, session.register_process({.process_id = 200, .parent_process_id = 100}));
        subprocess_process_manager launcher{"unused", unused_arguments, create_subprocess_launcher(), session.endpoint()};
        auto child =
            std::make_unique<subprocess_process_manager>("unused", unused_arguments, create_subprocess_launcher(), session.endpoint());
        child->initialize_current_process({.process_id = 200, .parent_process_id = 100});
        const auto process = launcher.open_process(200);
        ASSERT_TRUE(process);
        child.reset();
        const auto status = launcher.exit_status(process.process.handle);
        ASSERT_TRUE(status);
        ASSERT_TRUE(status.status);
        EXPECT_EQ(process_exit_kind::runtime_failure, status.status->kind);
        EXPECT_EQ(1u, status.status->code);
    }

    TEST(ProcessSession, FirstTerminalReportRemainsVisibleThroughAnExistingHandle)
    {
        const std::array reports{process_exit{.kind = process_exit_kind::exited, .code = 37},
                                 process_exit{.kind = process_exit_kind::runtime_failure, .code = 1}};
        for (size_t first = 0; first < reports.size(); ++first)
        {
            SCOPED_TRACE(first);
            process_session session{};
            ASSERT_EQ(process_error::none, session.register_process({.process_id = 100}));
            process_session publisher{session.endpoint()};
            subprocess_process_manager observer{"unused", unused_arguments, create_subprocess_launcher(), session.endpoint()};
            const auto opened = observer.open_process(100);
            ASSERT_TRUE(opened);

            ASSERT_EQ(process_error::none, publisher.mark_exit(100, reports[first]));
            ASSERT_EQ(process_error::none, session.mark_exit(100, reports[1 - first]));

            const auto status = observer.exit_status(opened.process.handle);
            ASSERT_TRUE(status);
            ASSERT_TRUE(status.status);
            EXPECT_EQ(reports[first].kind, status.status->kind);
            EXPECT_EQ(reports[first].code, status.status->code);
        }
    }

    TEST(ProcessSession, UnreachableRegistryDoesNotBecomeAnUnknownPid)
    {
        auto session = std::make_unique<process_session>();
        ASSERT_EQ(process_error::none, session->register_process({.process_id = 100}));
        subprocess_process_manager observer{"unused", unused_arguments, create_subprocess_launcher(), session->endpoint()};
        const auto opened = observer.open_process(100);
        ASSERT_TRUE(opened);
        EXPECT_EQ(process_error::invalid_process, observer.open_process(999).error);

        session.reset();

        EXPECT_EQ(process_error::communication_failure, observer.open_process(999).error);
        EXPECT_EQ(process_error::communication_failure, observer.exit_status(opened.process.handle).error);
    }

    TEST(ProcessSession, RejectedSessionDoesNotBecomeAnUnknownPid)
    {
        process_session session{};
        auto endpoint = session.endpoint();
        endpoint.token += "invalid";
        subprocess_process_manager observer{"unused", unused_arguments, create_subprocess_launcher(), std::move(endpoint)};
        EXPECT_EQ(process_error::communication_failure, observer.open_process(999).error);
    }
}

#endif

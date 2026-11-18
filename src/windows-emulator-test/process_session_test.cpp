#include "std_include.hpp"

#ifndef __EMSCRIPTEN__

#include <subprocess_process_manager.hpp>
#include <process_session.hpp>

#undef exception_code
#undef exception_info
#include <process_session_registry.hpp>

#include <process_protocol_utils.hpp>

namespace sogen::test
{
    namespace
    {
        auto unused_arguments(uint16_t, const std::string&)
        {
            return std::vector<std::string>{};
        }

        struct simulated_child
        {
            std::mutex mutex{};
            std::condition_variable changed{};
            std::optional<uint64_t> guest_exit{};
            bool host_exited{};
            uint32_t process_id{};
            uint32_t thread_id{};
        };

        class simulated_host_process final : public host_process
        {
          public:
            simulated_host_process(std::shared_ptr<simulated_child> child, const uint16_t port, std::string token)
                : child_(std::move(child)),
                  worker_([this, port, token = std::move(token)] {
                      network::tcp_client_socket socket{AF_INET};
                      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                      if (!socket.connect(network::address{"127.0.0.1", port}, deadline, nullptr))
                      {
                          this->kill();
                          return;
                      }
                      ProcessProtocol::HelloT hello{};
                      hello.token = token;
                      if (!process_protocol::send_message(socket, std::move(hello), deadline))
                      {
                          this->kill();
                          return;
                      }
                      const auto message = process_protocol::receive_message_until(socket, deadline);
                      if (!message || message->payload.type != ProcessProtocol::MessagePayload_ProcessCreateRequest)
                      {
                          this->kill();
                          return;
                      }
                      {
                          const std::scoped_lock lock(this->child_->mutex);
                          this->child_->process_id = message->payload.AsProcessCreateRequest()->process_id;
                          this->child_->thread_id = message->payload.AsProcessCreateRequest()->thread_id;
                      }
                      ProcessProtocol::ProcessStartedT started{};
                      started.native_environment = 0x180000;
                      if (!process_protocol::send_message(socket, started, deadline))
                      {
                          this->kill();
                          return;
                      }
                      std::unique_lock lock(this->child_->mutex);
                      this->child_->changed.wait(lock, [this] { return this->child_->guest_exit || this->child_->host_exited; });
                      if (this->child_->guest_exit)
                      {
                          ProcessProtocol::ProcessExitedT exited{};
                          exited.exit_code = *this->child_->guest_exit;
                          exited.cycles = 1234;
                          lock.unlock();
                          (void)process_protocol::send_message(socket, exited, std::chrono::steady_clock::now() + std::chrono::seconds(5));
                          lock.lock();
                      }
                      this->child_->changed.wait(lock, [this] { return this->child_->host_exited; });
                  })
            {
            }

            ~simulated_host_process() override
            {
                this->kill();
                this->worker_.join();
            }

            uint32_t id() const override
            {
                return 42;
            }

            void kill() override
            {
                {
                    const std::scoped_lock lock(this->child_->mutex);
                    this->child_->host_exited = true;
                }
                this->child_->changed.notify_all();
            }

            std::optional<int> run() override
            {
                std::unique_lock lock(this->child_->mutex);
                this->child_->changed.wait(lock, [this] { return this->child_->host_exited; });
                return 1;
            }

          private:
            std::shared_ptr<simulated_child> child_;
            std::thread worker_;
        };

        class reusing_pid_launcher final : public host_process_launcher
        {
          public:
            std::vector<std::shared_ptr<simulated_child>> children{};

            host_process_launch_result launch(const std::vector<std::string>& command) override
            {
                auto child = std::make_shared<simulated_child>();
                this->children.push_back(child);
                return {.process = std::make_unique<simulated_host_process>(child, static_cast<uint16_t>(std::stoul(command.at(1))),
                                                                            command.at(2))};
            }
        };

        process_exit_status_result wait_for_exit(subprocess_process_manager& manager, const managed_process process)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            auto status = manager.exit_status(process);
            while (!status.status && status && std::chrono::steady_clock::now() < deadline)
            {
                status = manager.exit_status(process);
            }
            return status;
        }
    }

    TEST(ProcessSession, GrandchildQueriesAncestorExitStatusThroughAnExistingHandle)
    {
        subprocess_process_manager launcher{"unused", unused_arguments};
        launcher.initialize_current_process({.process_id = 100, .native_environment = 0x180000, .compatibility_environment = 0x280000});
        process_session registry{launcher.session_endpoint()};
        const process_identity child_identity{.process_id = 200, .parent_process_id = 100};
        const auto registered_child = registry.register_process(child_identity);
        ASSERT_TRUE(registered_child);
        subprocess_process_manager child{"unused", unused_arguments, create_subprocess_launcher(), launcher.session_endpoint()};
        child.initialize_current_process(child_identity);
        const process_identity grandchild_identity{.process_id = 300, .parent_process_id = 200};
        const auto registered_grandchild = registry.register_process(grandchild_identity);
        ASSERT_TRUE(registered_grandchild);
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

    TEST(ProcessSession, RegistryOwnerCachesRemoteExitReportsForAnExistingHandle)
    {
        process_session owner{};
        const auto registered = owner.register_process({.process_id = 100, .parent_process_id = 200});
        ASSERT_TRUE(registered);
        const auto opened = owner.lookup(100);
        ASSERT_TRUE(opened);
        const auto active = owner.poll_exit_status(opened.process);
        ASSERT_TRUE(active);
        EXPECT_FALSE(active.status);

        process_session reporter{owner.endpoint()};
        ASSERT_EQ(process_error::none,
                  reporter.mark_exit(registered.process->identity().process_id, {.kind = process_exit_kind::exited, .code = 37}));
        const auto cached = opened.process->exit_status();
        ASSERT_TRUE(cached);
        ASSERT_TRUE(cached.status);
        EXPECT_EQ(process_exit_kind::exited, cached.status->kind);
        EXPECT_EQ(37u, cached.status->code);
        const auto ready = owner.poll_exit_status(opened.process);
        ASSERT_TRUE(ready);
        ASSERT_TRUE(ready.status);
        EXPECT_EQ(37u, ready.status->code);
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
        const auto registered = session.register_process({.process_id = 200, .parent_process_id = 100});
        ASSERT_TRUE(registered);
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
            const auto registered = session.register_process({.process_id = 100});
            ASSERT_TRUE(registered);
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

    TEST(ProcessSession, ReusedHostPidPreservesGuestHandlesAndSharesExitStatus)
    {
        process_session registry{};
        ASSERT_TRUE(registry.register_process({.process_id = default_guest_process_id}));
        auto launcher = std::make_unique<reusing_pid_launcher>();
        auto* launched = launcher.get();
        subprocess_process_manager owner{
            "unused", [](const uint16_t port, const std::string& token) { return std::vector{std::to_string(port), token}; },
            std::move(launcher), registry.endpoint()};
        subprocess_process_manager observer{"unused", unused_arguments, create_subprocess_launcher(), registry.endpoint()};
        const auto first = owner.create_process({.parent_process_id = default_guest_process_id});
        ASSERT_TRUE(first);
        const auto first_id = first.process.identity.process_id;
        EXPECT_EQ(0u, first_id % 4);
        EXPECT_NE(first_id, first.thread_id);
        EXPECT_EQ(0u, first.thread_id % 4);
        const auto old_opened = observer.open_process(first_id);
        ASSERT_TRUE(old_opened);
        const auto reopened_owned = owner.open_process(first_id);
        ASSERT_TRUE(reopened_owned);
        EXPECT_EQ(first.process.handle, reopened_owned.process.handle);
        {
            const std::scoped_lock lock(launched->children[0]->mutex);
            EXPECT_EQ(first_id, launched->children[0]->process_id);
            EXPECT_EQ(first.thread_id, launched->children[0]->thread_id);
            launched->children[0]->guest_exit = 37;
        }
        launched->children[0]->changed.notify_all();
        const auto owned_exit = wait_for_exit(owner, first.process.handle);
        const auto opened_exit = wait_for_exit(observer, old_opened.process.handle);
        ASSERT_TRUE(owned_exit.status);
        ASSERT_TRUE(opened_exit.status);
        EXPECT_EQ(process_exit_kind::exited, owned_exit.status->kind);
        EXPECT_EQ(37u, owned_exit.status->code);
        EXPECT_EQ(owned_exit.status->kind, opened_exit.status->kind);
        EXPECT_EQ(owned_exit.status->code, opened_exit.status->code);
        EXPECT_EQ(1234u, owned_exit.status->cycles);
        {
            const std::scoped_lock lock(launched->children[0]->mutex);
            EXPECT_FALSE(launched->children[0]->host_exited);
            launched->children[0]->host_exited = true;
        }
        launched->children[0]->changed.notify_all();

        const auto second = owner.create_process({.parent_process_id = 200});
        ASSERT_TRUE(second);
        const auto second_id = second.process.identity.process_id;
        EXPECT_NE(first_id, second_id);
        const auto new_opened = observer.open_process(second_id);
        ASSERT_TRUE(new_opened);
        EXPECT_EQ(200u, new_opened.process.identity.parent_process_id);
        EXPECT_FALSE(owner.exit_status(second.process.handle).status);
        EXPECT_FALSE(observer.exit_status(new_opened.process.handle).status);
        {
            const std::scoped_lock lock(launched->children[1]->mutex);
            EXPECT_EQ(second_id, launched->children[1]->process_id);
            EXPECT_EQ(second.thread_id, launched->children[1]->thread_id);
            launched->children[1]->host_exited = true;
        }
        launched->children[1]->changed.notify_all();
        const auto owned_failure = wait_for_exit(owner, second.process.handle);
        const auto opened_failure = wait_for_exit(observer, new_opened.process.handle);
        ASSERT_TRUE(owned_failure.status);
        ASSERT_TRUE(opened_failure.status);
        EXPECT_EQ(process_exit_kind::runtime_failure, owned_failure.status->kind);
        EXPECT_EQ(1u, owned_failure.status->code);
        EXPECT_EQ(owned_failure.status->kind, opened_failure.status->kind);
        EXPECT_EQ(owned_failure.status->code, opened_failure.status->code);
        const auto original = owner.exit_status(first.process.handle);
        const auto original_opened = observer.exit_status(old_opened.process.handle);
        ASSERT_TRUE(original.status);
        ASSERT_TRUE(original_opened.status);
        EXPECT_EQ(37u, original.status->code);
        EXPECT_EQ(37u, original_opened.status->code);
        EXPECT_EQ(first_id, old_opened.process.identity.process_id);
        EXPECT_EQ(default_guest_process_id, old_opened.process.identity.parent_process_id);
    }

    TEST(ProcessSession, PidAllocationExcludesTheRootsConfiguredInitialThread)
    {
        subprocess_process_manager root{"unused", unused_arguments};
        root.initialize_current_process({.process_id = default_guest_process_id}, 8);
        process_session session{root.session_endpoint()};
        const auto child = session.reserve_process();
        ASSERT_TRUE(child);
        EXPECT_NE(default_guest_process_id, child.process_id);
        EXPECT_NE(8u, child.process_id);
        EXPECT_NE(8u, child.thread_id);
        EXPECT_EQ(0u, child.process_id % 4);
    }

    TEST(ProcessSession, ExplicitReservationsRespectProcessAndInitialThreadIds)
    {
        process_session session{};
        ASSERT_TRUE(session.reserve_process(100, 104));
        EXPECT_EQ(process_error::unavailable, session.reserve_process(101).error);
        EXPECT_EQ(process_error::unavailable, session.reserve_process(104).error);
        EXPECT_EQ(process_error::unavailable, session.reserve_process(96, 100).error);
        EXPECT_EQ(process_error::unavailable, session.reserve_process(200, 100).error);
        EXPECT_EQ(process_error::unavailable, session.reserve_process(200, 201).error);
        EXPECT_TRUE(session.reserve_process(200, 12));
        for (size_t i = 0; i < 30; ++i)
        {
            const auto reserved = session.reserve_process();
            ASSERT_TRUE(reserved);
            EXPECT_NE(100u, reserved.process_id);
            EXPECT_NE(104u, reserved.process_id);
            EXPECT_NE(100u, reserved.thread_id);
            EXPECT_NE(104u, reserved.thread_id);
            EXPECT_NE(12u, reserved.thread_id);
        }
    }

    TEST(ProcessSession, CurrentProcessInitializationRejectsZeroBeforeAndAfterInitialization)
    {
        subprocess_process_manager manager{"unused", unused_arguments};
        EXPECT_THROW(manager.initialize_current_process({}), std::invalid_argument);
        manager.initialize_current_process({.process_id = default_guest_process_id});
        EXPECT_THROW(manager.initialize_current_process({}), std::invalid_argument);
        manager.notify_current_process_exit(37);
        const auto opened = manager.open_process(default_guest_process_id);
        ASSERT_TRUE(opened);
        const auto status = manager.exit_status(opened.process.handle);
        ASSERT_TRUE(status.status);
        EXPECT_EQ(37u, status.status->code);
    }

    TEST(ProcessSession, AllocatedPidsExcludeTheRootAndRemainUniqueAfterExitAndDiscard)
    {
        process_session registry{};
        ASSERT_TRUE(registry.register_process({.process_id = 8}));
        const auto first = registry.register_process({});
        ASSERT_TRUE(first);
        const auto first_id = first.process->identity().process_id;
        EXPECT_NE(8u, first_id);
        EXPECT_EQ(0u, first_id % 4);
        ASSERT_EQ(process_error::none, registry.mark_exit(first_id, {.kind = process_exit_kind::exited, .code = 37}));
        const auto abandoned = registry.reserve_process();
        ASSERT_TRUE(abandoned);
        ASSERT_EQ(process_error::none, registry.discard_process(abandoned.process_id));
        const auto second = registry.register_process({});
        ASSERT_TRUE(second);
        const auto second_id = second.process->identity().process_id;
        EXPECT_NE(first_id, second_id);
        EXPECT_NE(abandoned.process_id, second_id);
        EXPECT_EQ(0u, second_id % 4);
        EXPECT_FALSE(second.process->exit_status().status);
        ASSERT_TRUE(first.process->exit_status().status);
        EXPECT_EQ(37u, first.process->exit_status().status->code);
    }

    TEST(ProcessSession, ConcurrentProcessAndThreadAllocationSharesOneNamespace)
    {
        process_session registry{};
        ASSERT_TRUE(registry.register_process({.process_id = default_guest_process_id}, 8));
        const auto child = registry.reserve_process();
        ASSERT_TRUE(child);
        ASSERT_TRUE(registry.publish_process({.process_id = child.process_id}));
        process_session root_client{registry.endpoint()};
        process_session child_client{registry.endpoint()};
        std::vector<guest_id_result> root_threads;
        std::vector<guest_id_result> child_threads;
        std::thread root_worker([&] {
            for (size_t i = 0; i < 32; ++i)
            {
                root_threads.push_back(root_client.allocate_thread_id(default_guest_process_id));
            }
        });
        std::thread child_worker([&] {
            for (size_t i = 0; i < 32; ++i)
            {
                child_threads.push_back(child_client.allocate_thread_id(child.process_id));
            }
        });
        std::vector<process_session_reservation_result> children;
        children.reserve(16);
        for (size_t i = 0; i < 16; ++i)
        {
            children.push_back(registry.reserve_process());
        }
        root_worker.join();
        child_worker.join();

        std::set<uint32_t> ids{default_guest_process_id, 8, child.process_id, child.thread_id};
        ASSERT_EQ(4u, ids.size());
        for (const auto& threads : {root_threads, child_threads})
        {
            for (const auto& thread : threads)
            {
                ASSERT_TRUE(thread);
                EXPECT_EQ(0u, thread.id % 4);
                EXPECT_TRUE(ids.insert(thread.id).second);
            }
        }
        for (const auto& process : children)
        {
            ASSERT_TRUE(process);
            EXPECT_EQ(0u, process.process_id % 4);
            EXPECT_EQ(0u, process.thread_id % 4);
            EXPECT_TRUE(ids.insert(process.process_id).second);
            EXPECT_TRUE(ids.insert(process.thread_id).second);
        }
        const auto restored = root_client.allocate_thread_id(default_guest_process_id, root_threads.front().id);
        ASSERT_TRUE(restored);
        EXPECT_EQ(root_threads.front().id, restored.id);
        EXPECT_EQ(process_error::unavailable, child_client.allocate_thread_id(child.process_id, root_threads.front().id).error);
        EXPECT_EQ(process_error::unavailable, root_client.allocate_thread_id(default_guest_process_id, child.process_id).error);
        EXPECT_EQ(process_error::unavailable, root_client.allocate_thread_id(default_guest_process_id, default_guest_process_id).error);
        ASSERT_EQ(process_error::none, registry.discard_process(children.front().process_id));
        const auto replacement = registry.reserve_process();
        ASSERT_TRUE(replacement);
        EXPECT_TRUE(ids.insert(replacement.process_id).second);
        EXPECT_TRUE(ids.insert(replacement.thread_id).second);
    }

    TEST(ProcessSession, CachedExitStatusSurvivesTheRegistryConnection)
    {
        auto registry = std::make_unique<process_session>();
        const auto registered = registry->register_process({.process_id = 100});
        ASSERT_TRUE(registered);
        subprocess_process_manager observer{"unused", unused_arguments, create_subprocess_launcher(), registry->endpoint()};
        const auto opened = observer.open_process(100);
        ASSERT_TRUE(opened);
        ASSERT_EQ(process_error::none,
                  registry->mark_exit(registered.process->identity().process_id, {.kind = process_exit_kind::exited, .code = 37}));
        ASSERT_TRUE(observer.exit_status(opened.process.handle).status);
        registry.reset();

        const auto cached = observer.exit_status(opened.process.handle);
        ASSERT_TRUE(cached);
        ASSERT_TRUE(cached.status);
        EXPECT_EQ(37u, cached.status->code);
    }

    TEST(ProcessSession, UnreachableRegistryDoesNotBecomeAnUnknownPid)
    {
        auto session = std::make_unique<process_session>();
        ASSERT_TRUE(session->register_process({.process_id = 100}));
        subprocess_process_manager observer{"unused", unused_arguments, create_subprocess_launcher(), session->endpoint()};
        const auto opened = observer.open_process(100);
        ASSERT_TRUE(opened);
        EXPECT_EQ(process_error::invalid_process, observer.open_process(999).error);

        session.reset();

        EXPECT_EQ(process_error::communication_failure, observer.open_process(999).error);
        EXPECT_EQ(process_error::communication_failure, observer.exit_status(opened.process.handle).error);
    }

    TEST(ProcessSession, RepeatedOpensShareTheManagedInstance)
    {
        process_session registry{};
        ASSERT_TRUE(registry.register_process({.process_id = 100}));
        subprocess_process_manager observer{"unused", unused_arguments, create_subprocess_launcher(), registry.endpoint()};
        const auto first = observer.open_process(100);
        ASSERT_TRUE(first);
        const auto reopened = observer.open_process(100);
        ASSERT_TRUE(reopened);
        EXPECT_EQ(first.process.handle, reopened.process.handle);
    }

    TEST(ProcessSession, DiscardingAnUnpublishedProcessPreservesOtherProcesses)
    {
        process_session registry{};
        const process_identity identity{.process_id = 100, .parent_process_id = 200, .native_environment = 0x180000};
        const auto original = registry.register_process(identity);
        ASSERT_TRUE(original);
        const auto reserved = registry.reserve_process();
        ASSERT_TRUE(reserved);
        ASSERT_EQ(process_error::none, registry.mark_exit(reserved.process_id, {.kind = process_exit_kind::runtime_failure, .code = 1}));
        ASSERT_EQ(process_error::none, registry.discard_process(reserved.process_id));
        const auto current = registry.lookup(100);
        ASSERT_TRUE(current);
        EXPECT_EQ(original.process->identity().process_id, current.process->identity().process_id);
        EXPECT_EQ(process_error::invalid_process, registry.mark_exit(reserved.process_id, {.kind = process_exit_kind::exited, .code = 42}));
    }

    TEST(ProcessSession, ExitBeforePublicationIsPreservedWhenIdentityBecomesAvailable)
    {
        process_session registry{};
        const auto reserved = registry.reserve_process();
        ASSERT_TRUE(reserved);
        ASSERT_EQ(process_error::none, registry.mark_exit(reserved.process_id, {.kind = process_exit_kind::runtime_failure, .code = 1}));
        EXPECT_EQ(process_error::invalid_process, registry.lookup(reserved.process_id).error);
        const auto published =
            registry.publish_process({.process_id = reserved.process_id, .parent_process_id = 200, .native_environment = 0x180000});
        ASSERT_TRUE(published);
        EXPECT_EQ(200u, published.process->identity().parent_process_id);
        EXPECT_EQ(0x180000u, published.process->identity().native_environment);
        const auto status = published.process->exit_status();
        ASSERT_TRUE(status.status);
        EXPECT_EQ(process_exit_kind::runtime_failure, status.status->kind);
    }

    TEST(ProcessSession, AbandonedReservationsExpireWithoutRemovingPublishedProcesses)
    {
        const auto start = std::chrono::steady_clock::now();
        std::atomic<int64_t> elapsed_seconds{};
        process_session registry{{}, [&] { return start + std::chrono::seconds(elapsed_seconds.load()); }};
        const auto abandoned = registry.reserve_process();
        const auto failed = registry.reserve_process();
        ASSERT_TRUE(abandoned);
        ASSERT_TRUE(failed);
        ASSERT_EQ(process_error::none, registry.mark_exit(failed.process_id, {.kind = process_exit_kind::runtime_failure, .code = 1}));
        const auto published = registry.register_process({.process_id = 200, .parent_process_id = 100});
        ASSERT_TRUE(published);
        elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(process_session_registry::reservation_lifetime).count();
        EXPECT_EQ(process_error::invalid_process, registry.publish_process({.process_id = abandoned.process_id}).error);
        EXPECT_EQ(process_error::invalid_process, registry.publish_process({.process_id = failed.process_id}).error);
        const auto current = registry.lookup(200);
        ASSERT_TRUE(current);
        EXPECT_EQ(published.process->identity().process_id, current.process->identity().process_id);
        const auto replacement = registry.register_process({.process_id = 100});
        ASSERT_TRUE(replacement);
    }

    TEST(ProcessSession, CompletedProcessesDoNotExhaustReservationCapacity)
    {
        process_session registry{};
        const auto first = registry.register_process({.process_id = 100});
        ASSERT_TRUE(first);
        for (uint64_t i = 0; i < 65536; ++i)
        {
            const auto current = registry.register_process({});
            ASSERT_TRUE(current);
            ASSERT_EQ(process_error::none,
                      registry.mark_exit(current.process->identity().process_id, {.kind = process_exit_kind::exited, .code = i}));
        }
        ASSERT_EQ(process_error::none,
                  registry.mark_exit(first.process->identity().process_id, {.kind = process_exit_kind::exited, .code = 123}));
        const auto original = registry.exit_status(first.process);
        ASSERT_TRUE(original.status);
        EXPECT_EQ(123u, original.status->code);
        EXPECT_TRUE(registry.reserve_process());
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

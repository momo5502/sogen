#ifndef __EMSCRIPTEN__

#include "emulation_test_utils.hpp"
#include <subprocess_process_manager.hpp>
#include <network/tcp_server_socket.hpp>

#undef exception_code
#undef exception_info
#include <process_protocol_utils.hpp>
#include <process_session_registry.hpp>
#include <future>
#include <set>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sogen::test
{
    namespace
    {
        class gated_session_registry
        {
          public:
            gated_session_registry()
                : server_(AF_INET)
            {
                if (!this->server_.bind(network::address{"127.0.0.1", 0}))
                {
                    throw std::runtime_error("Cannot bind test registry");
                }
                this->server_.listen();
                this->worker_ = std::thread([this] { this->serve(); });
            }

            ~gated_session_registry()
            {
                {
                    const std::scoped_lock lock(this->mutex_);
                    this->stopping_ = true;
                }
                this->changed_.notify_all();
                this->worker_.join();
            }

            process_session_endpoint endpoint() const
            {
                return {.port = this->server_.get_port(), .token = "test"};
            }

            std::vector<uint32_t> wait_for_batch(const size_t index)
            {
                std::unique_lock lock(this->mutex_);
                if (!this->changed_.wait_for(lock, std::chrono::seconds(5), [&] { return this->batches_.size() > index; }))
                {
                    return {};
                }
                return this->batches_[index];
            }

            bool prepare_barrier(subprocess_process_manager& manager)
            {
                const auto barrier = manager.open_process(barrier_process_id);
                this->barrier_process_ = barrier.process.handle;
                return static_cast<bool>(barrier);
            }

            bool complete_terminal_batch(subprocess_process_manager& manager, const size_t count)
            {
                if (!this->barrier_process_ || !manager.poll_exit_status(this->barrier_process_))
                {
                    return false;
                }
                this->release(count, true);
                return this->wait_for_batch(count) == std::vector<uint32_t>{barrier_process_id};
            }

            void release(const size_t count, const bool exited, const bool drop_response = false, uint32_t missing_process_id = {})
            {
                {
                    const std::scoped_lock lock(this->mutex_);
                    this->released_ = count;
                    this->exited_ = exited;
                    this->drop_response_ = drop_response;
                    this->missing_process_id_ = missing_process_id;
                }
                this->changed_.notify_all();
            }

          private:
            static constexpr uint32_t barrier_process_id = 0xFFFFFFF8;
            managed_process barrier_process_{};
            network::tcp_server_socket server_;
            std::atomic_bool stopping_{};
            std::mutex mutex_{};
            std::condition_variable changed_{};
            std::vector<std::vector<uint32_t>> batches_{};
            size_t released_{};
            bool exited_{};
            bool drop_response_{};
            uint32_t missing_process_id_{};
            std::thread worker_{};

            static std::unique_ptr<ProcessProtocol::SessionProcessT> record(const uint32_t process_id, const bool exited)
            {
                auto result = std::make_unique<ProcessProtocol::SessionProcessT>();
                result->process_id = process_id;
                result->exited = exited;
                result->exit_code = 37;
                result->exit_kind = static_cast<uint8_t>(process_exit_kind::exited);
                return result;
            }

            void serve()
            {
                while (!this->stopping_)
                {
                    if (!this->server_.sleep(std::chrono::milliseconds(10), true))
                    {
                        continue;
                    }
                    auto client = this->server_.accept();
                    if (!client.set_blocking(false))
                    {
                        continue;
                    }
                    auto message =
                        process_protocol::receive_message_until(client, std::chrono::steady_clock::time_point::max(), &this->stopping_);
                    if (!message || message->payload.type != ProcessProtocol::MessagePayload_SessionRequest)
                    {
                        continue;
                    }
                    const auto* request = message->payload.AsSessionRequest();
                    if (!request)
                    {
                        continue;
                    }
                    ProcessProtocol::SessionResponseT response{};
                    if (request->operation == ProcessProtocol::SessionOperation_Refresh)
                    {
                        std::unique_lock lock(this->mutex_);
                        this->batches_.push_back(request->process_ids);
                        this->changed_.notify_all();
                        const auto batch = this->batches_.size();
                        this->changed_.wait(lock, [&] { return this->stopping_ || this->released_ >= batch; });
                        if (this->stopping_)
                        {
                            return;
                        }
                        if (this->drop_response_)
                        {
                            continue;
                        }
                        for (const auto& process_id : request->process_ids)
                        {
                            if (process_id == this->missing_process_id_)
                            {
                                response.missing_process_ids.push_back(process_id);
                            }
                            else
                            {
                                response.processes.push_back(record(process_id, this->exited_ && process_id != barrier_process_id));
                            }
                        }
                    }
                    else
                    {
                        response.process = record(request->process_id, false);
                    }
                    (void)process_protocol::send_message(client, std::move(response),
                                                         std::chrono::steady_clock::now() + std::chrono::seconds(5), &this->stopping_);
                }
            }
        };

        std::optional<ProcessProtocol::SessionResponseT> request_session(const process_session_endpoint& endpoint,
                                                                         ProcessProtocol::SessionRequestT request)
        {
            network::tcp_client_socket client{AF_INET};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (!client.connect(network::address{"127.0.0.1", endpoint.port}, deadline, nullptr))
            {
                return std::nullopt;
            }
            request.token = endpoint.token;
            if (!process_protocol::send_message(client, std::move(request), deadline))
            {
                return std::nullopt;
            }
            auto message = process_protocol::receive_message_until(client, deadline);
            if (!message || message->payload.type != ProcessProtocol::MessagePayload_SessionResponse)
            {
                return std::nullopt;
            }
            return std::move(*message->payload.AsSessionResponse());
        }

        class faulting_session_registry
        {
          public:
            process_session registry{};
            std::atomic_size_t discard_attempts{};
            std::atomic_bool drop_exit_reports{};

            explicit faulting_session_registry(const bool fail_publication_retry, const bool drop_before_publish = false)
                : server_(AF_INET),
                  fail_publication_retry_(fail_publication_retry),
                  drop_before_publish_(drop_before_publish)
            {
                if (!this->server_.bind(network::address{"127.0.0.1", 0}))
                {
                    throw std::runtime_error("Cannot bind test registry proxy");
                }
                this->server_.listen();
                this->worker_ = std::thread([this] { this->serve(); });
            }

            ~faulting_session_registry()
            {
                this->stopping_ = true;
                this->worker_.join();
            }

            process_session_endpoint endpoint() const
            {
                return {.port = this->server_.get_port(), .token = "proxy"};
            }

            std::vector<uint32_t> publication_process_ids()
            {
                const std::scoped_lock lock(this->mutex_);
                return this->publication_process_ids_;
            }

            bool wait_for_exit_delivery()
            {
                std::unique_lock lock(this->mutex_);
                return this->changed_.wait_for(lock, std::chrono::seconds(5), [this] { return this->exit_delivered_; });
            }

            bool wait_for_exit_attempts(const size_t count)
            {
                std::unique_lock lock(this->mutex_);
                return this->changed_.wait_for(lock, std::chrono::seconds(5), [this, count] { return this->exit_attempts_ >= count; });
            }

            bool wait_for_discard()
            {
                std::unique_lock lock(this->mutex_);
                return this->changed_.wait_for(lock, std::chrono::seconds(5), [this] { return this->discard_completed_; });
            }

          private:
            network::tcp_server_socket server_;
            bool fail_publication_retry_{};
            bool publication_dropped_{};
            bool drop_before_publish_{};
            std::atomic_bool stopping_{};
            std::thread worker_{};

            std::mutex mutex_{};
            std::condition_variable changed_{};
            std::vector<uint32_t> publication_process_ids_{};
            bool discard_completed_{};
            size_t exit_attempts_{};
            bool exit_delivered_{};

            void serve()
            {
                while (!this->stopping_)
                {
                    if (!this->server_.sleep(std::chrono::milliseconds(10), true))
                    {
                        continue;
                    }
                    auto client = this->server_.accept();
                    if (!client.set_blocking(false))
                    {
                        continue;
                    }
                    auto message =
                        process_protocol::receive_message_until(client, std::chrono::steady_clock::time_point::max(), &this->stopping_);
                    if (!message || message->payload.type != ProcessProtocol::MessagePayload_SessionRequest)
                    {
                        continue;
                    }
                    auto request = std::move(*message->payload.AsSessionRequest());
                    const auto operation = request.operation;
                    if (operation == ProcessProtocol::SessionOperation_Exit)
                    {
                        {
                            const std::scoped_lock lock(this->mutex_);
                            ++this->exit_attempts_;
                        }
                        this->changed_.notify_all();
                        if (this->drop_exit_reports)
                        {
                            continue;
                        }
                    }
                    if (operation == ProcessProtocol::SessionOperation_Publish)
                    {
                        const std::scoped_lock lock(this->mutex_);
                        this->publication_process_ids_.push_back(request.process_id);
                    }
                    if (operation == ProcessProtocol::SessionOperation_Publish && this->publication_dropped_ &&
                        this->fail_publication_retry_)
                    {
                        continue;
                    }
                    if (operation == ProcessProtocol::SessionOperation_Discard && ++this->discard_attempts == 1 &&
                        this->fail_publication_retry_)
                    {
                        continue;
                    }
                    if (operation == ProcessProtocol::SessionOperation_Publish && !this->publication_dropped_ && this->drop_before_publish_)
                    {
                        this->publication_dropped_ = true;
                        continue;
                    }
                    auto response = request_session(this->registry.endpoint(), std::move(request));
                    if (operation == ProcessProtocol::SessionOperation_Exit && response &&
                        response->error == ProcessProtocol::ProcessError_None)
                    {
                        {
                            const std::scoped_lock lock(this->mutex_);
                            this->exit_delivered_ = true;
                        }
                        this->changed_.notify_all();
                    }
                    if (operation == ProcessProtocol::SessionOperation_Discard && response &&
                        response->error == ProcessProtocol::ProcessError_None)
                    {
                        {
                            const std::scoped_lock lock(this->mutex_);
                            this->discard_completed_ = true;
                        }
                        this->changed_.notify_all();
                    }
                    if (operation == ProcessProtocol::SessionOperation_Publish && !this->publication_dropped_)
                    {
                        this->publication_dropped_ = true;
                        continue;
                    }
                    if (response)
                    {
                        (void)process_protocol::send_message(client, std::move(*response),
                                                             std::chrono::steady_clock::now() + std::chrono::seconds(5), &this->stopping_);
                    }
                }
            }
        };

        auto unused_wait_arguments(uint16_t, const std::string&)
        {
            return std::vector<std::string>{};
        }
    }

    TEST(ProcessSession, CreatedThreadsUseSessionIdsAndAllocationFailureCreatesNoThread)
    {
        auto registry = std::make_unique<process_session>();
        ASSERT_TRUE(registry->register_process({.process_id = default_guest_process_id}, 8));
        subprocess_process_manager manager{"unused", unused_wait_arguments, create_subprocess_launcher(), registry->endpoint()};
        manager.initialize_current_process({.process_id = default_guest_process_id}, 8);
        emulator_interfaces interfaces{.ui = std::make_unique<null_ui_backend>(),
                                       .audio = std::make_unique<null_audio_backend>(),
                                       .processes = &manager,
                                       .console = std::make_unique<null_console_backend>()};
        windows_emulator emu{create_x86_64_emulator_from_environment(),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             emulator_callbacks{}, std::move(interfaces)};
        emu.process.user_handles.setup(false);
        const auto initial = emu.process.create_thread(emu.memory, 0, 0, 0x100000, 0, true);
        ASSERT_NE(initial, 0);
        ASSERT_EQ(8u, emu.process.threads.get(initial)->id);
        std::set<uint32_t> ids{default_guest_process_id, 8};
        for (size_t i = 0; i < 4; ++i)
        {
            const auto child = registry->reserve_process();
            ASSERT_TRUE(child);
            ASSERT_TRUE(ids.insert(child.process_id).second);
            ASSERT_TRUE(ids.insert(child.thread_id).second);
            const auto handle = emu.process.create_thread(emu.memory, 0, 0, 0x100000, 0);
            ASSERT_NE(handle, 0);
            const auto* thread = emu.process.threads.get(handle);
            ASSERT_NE(nullptr, thread);
            EXPECT_TRUE(ids.insert(thread->id).second);
            EXPECT_EQ(0u, thread->id % 4);
            ASSERT_TRUE(thread->teb64);
            const auto teb = thread->teb64->read();
            EXPECT_EQ(thread->id, teb.ClientId.UniqueThread);
            EXPECT_EQ(default_guest_process_id, teb.ClientId.UniqueProcess);
        }
        const auto count = emu.process.spawned_thread_count;
        registry.reset();
        EXPECT_EQ(emu.process.create_thread(emu.memory, 0, 0, 0x100000, 0), 0);
        EXPECT_EQ(count, emu.process.spawned_thread_count);
    }

    TEST(ProcessSession, SchedulerWaitsStayLocalWhileABatchedRefreshIsBlocked)
    {
        gated_session_registry registry{};
        subprocess_process_manager manager{
            "unused", unused_wait_arguments, create_subprocess_launcher(), registry.endpoint(), std::chrono::seconds(5), [] {
                return std::chrono::steady_clock::time_point{};
            }};
        ASSERT_TRUE(registry.prepare_barrier(manager));
        const auto first = manager.open_process(100);
        const auto second = manager.open_process(200);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        emulator_interfaces interfaces{.ui = std::make_unique<null_ui_backend>(),
                                       .audio = std::make_unique<null_audio_backend>(),
                                       .processes = &manager,
                                       .console = std::make_unique<null_console_backend>()};
        windows_emulator emu{create_x86_64_emulator_from_environment(),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             emulator_callbacks{}, std::move(interfaces)};
        const auto handle = emu.process.processes.store(make_emulator_process(first.process));
        const auto reference = emu.process.processes.reference_of(handle);
        ASSERT_TRUE(reference);
        emulator_thread thread{emu.memory};
        thread.id = 8;
        thread.await_objects = {*reference};
        thread.await_any = true;
        EXPECT_FALSE(thread.is_thread_ready(emu));
        ASSERT_EQ(1u, registry.wait_for_batch(0).size());
        EXPECT_FALSE(manager.poll_exit_status(second.process.handle).status);
        const auto pending = manager.poll_exit_status(first.process.handle);
        ASSERT_TRUE(pending);
        EXPECT_FALSE(pending.status);
        EXPECT_FALSE(thread.is_thread_ready(emu));
        registry.release(1, false);
        const auto batch = registry.wait_for_batch(1);
        ASSERT_EQ(2u, batch.size());
        EXPECT_NE(batch[0], batch[1]);
        ASSERT_TRUE(registry.complete_terminal_batch(manager, 2));
        ASSERT_TRUE(manager.poll_exit_status(first.process.handle).status);
        EXPECT_TRUE(thread.is_thread_ready(emu));
        ASSERT_TRUE(manager.poll_exit_status(second.process.handle).status);
    }

    TEST(ProcessSession, AFailedRefreshKeepsTheWaitPendingAndRetries)
    {
        gated_session_registry registry{};
        subprocess_process_manager manager{
            "unused", unused_wait_arguments, create_subprocess_launcher(), registry.endpoint(), std::chrono::seconds(5), [] {
                return std::chrono::steady_clock::time_point{};
            }};
        ASSERT_TRUE(registry.prepare_barrier(manager));
        const auto opened = manager.open_process(100);
        ASSERT_TRUE(opened);
        ASSERT_TRUE(manager.poll_exit_status(opened.process.handle));
        ASSERT_EQ(1u, registry.wait_for_batch(0).size());
        registry.release(1, false, true);
        ASSERT_EQ(1u, registry.wait_for_batch(1).size());
        const auto pending = manager.poll_exit_status(opened.process.handle);
        EXPECT_TRUE(pending);
        EXPECT_FALSE(pending.status);
        ASSERT_TRUE(registry.complete_terminal_batch(manager, 2));
        const auto exited = manager.poll_exit_status(opened.process.handle);
        ASSERT_TRUE(exited);
        ASSERT_TRUE(exited.status);
        EXPECT_EQ(37u, exited.status->code);
    }

    TEST(ProcessSession, RefreshReturnsValidRecordsAlongsideMissingAndUnpublishedProcesses)
    {
        process_session registry{};
        const auto first = registry.register_process({.process_id = 100});
        const auto second = registry.register_process({.process_id = 200});
        const auto unpublished = registry.reserve_process();
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        ASSERT_TRUE(unpublished);
        ASSERT_EQ(process_error::none,
                  registry.mark_exit(second.process->identity().process_id, {.kind = process_exit_kind::exited, .code = 37}));
        ProcessProtocol::SessionRequestT request{};
        request.operation = ProcessProtocol::SessionOperation_Refresh;
        request.process_ids = {999999, first.process->identity().process_id, unpublished.process_id, second.process->identity().process_id};
        const auto response = request_session(registry.endpoint(), std::move(request));
        ASSERT_TRUE(response);
        EXPECT_EQ(ProcessProtocol::ProcessError_None, response->error);
        ASSERT_EQ(2u, response->processes.size());
        const auto find_record = [&](const uint32_t process_id) {
            return std::ranges::find_if(response->processes, [process_id](const auto& record) { return record->process_id == process_id; });
        };
        const auto active = find_record(first.process->identity().process_id);
        const auto exited = find_record(second.process->identity().process_id);
        ASSERT_NE(response->processes.end(), active);
        ASSERT_NE(response->processes.end(), exited);
        EXPECT_FALSE((*active)->exited);
        EXPECT_TRUE((*exited)->exited);
        EXPECT_EQ(37u, (*exited)->exit_code);
        EXPECT_EQ((std::set<uint32_t>{999999, unpublished.process_id}),
                  (std::set<uint32_t>{response->missing_process_ids.begin(), response->missing_process_ids.end()}));
        EXPECT_EQ(2u, response->missing_process_ids.size());
    }

    TEST(ProcessSession, MissingProcessesStopBeingWatchedWhileOtherProcessesKeepRefreshing)
    {
        gated_session_registry registry{};
        subprocess_process_manager manager{
            "unused", unused_wait_arguments, create_subprocess_launcher(), registry.endpoint(), std::chrono::seconds(5), [] {
                return std::chrono::steady_clock::time_point{};
            }};
        ASSERT_TRUE(registry.prepare_barrier(manager));
        const auto missing = manager.open_process(100);
        const auto valid = manager.open_process(200);
        ASSERT_TRUE(missing);
        ASSERT_TRUE(valid);
        ASSERT_TRUE(manager.poll_exit_status(missing.process.handle));
        ASSERT_EQ(1u, registry.wait_for_batch(0).size());
        ASSERT_TRUE(manager.poll_exit_status(valid.process.handle));
        registry.release(1, false, false, 100);
        const auto next_batch = registry.wait_for_batch(1);
        ASSERT_EQ(1u, next_batch.size());
        EXPECT_EQ(200u, next_batch.front());
        EXPECT_EQ(process_error::invalid_process, manager.poll_exit_status(missing.process.handle).error);
        ASSERT_TRUE(registry.complete_terminal_batch(manager, 2));
        ASSERT_TRUE(manager.poll_exit_status(valid.process.handle).status);
        EXPECT_EQ(process_error::invalid_process, manager.poll_exit_status(missing.process.handle).error);
    }

    TEST(ProcessSession, DiscardedPublicationCannotBePublishedAgain)
    {
        process_session registry{};
        const auto reserved = registry.reserve_process();
        ASSERT_TRUE(reserved);
        const process_identity identity{.process_id = reserved.process_id, .parent_process_id = 200};
        ASSERT_TRUE(registry.publish_process(identity));
        ASSERT_EQ(process_error::none, registry.discard_process(reserved.process_id));
        EXPECT_EQ(process_error::invalid_process, registry.lookup(reserved.process_id).error);
        EXPECT_EQ(process_error::unavailable, registry.publish_process(identity).error);
        EXPECT_TRUE(registry.register_process({}));
    }

    TEST(ProcessSession, InactiveWatchesExpireAndCanBeRenewed)
    {
        gated_session_registry registry{};
        std::atomic<int64_t> elapsed_ticks{};
        const auto lifetime = std::chrono::steady_clock::duration(process_session::watch_lifetime).count();
        process_session session{registry.endpoint(), [&] {
                                    return std::chrono::steady_clock::time_point{} +
                                           std::chrono::steady_clock::duration(elapsed_ticks.load());
                                }};
        const auto first = session.lookup(100);
        const auto second = session.lookup(200);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_FALSE(session.poll_exit_status(first.process).status);
        ASSERT_EQ(1u, registry.wait_for_batch(0).size());
        elapsed_ticks = lifetime / 2;
        EXPECT_FALSE(session.poll_exit_status(second.process).status);
        elapsed_ticks = lifetime + lifetime / 4;
        registry.release(1, false);
        EXPECT_EQ(std::vector<uint32_t>{200}, registry.wait_for_batch(1));
        EXPECT_FALSE(session.poll_exit_status(first.process).status);
        registry.release(2, false);
        EXPECT_EQ(2u, registry.wait_for_batch(2).size());
        registry.release(3, true);
    }

    TEST(ProcessSession, LostPublicationIsRetriedWithTheSamePid)
    {
        for (const bool drop_before_publish : {false, true})
        {
            faulting_session_registry registry{false, drop_before_publish};
            process_session publisher{registry.endpoint()};
            SCOPED_TRACE(drop_before_publish);
            const auto reserved = publisher.reserve_process();
            ASSERT_TRUE(reserved);
            const auto created = publisher.publish_process({.process_id = reserved.process_id, .parent_process_id = 200});
            ASSERT_TRUE(created);
            const auto current = registry.registry.lookup(reserved.process_id);
            ASSERT_TRUE(current);
            EXPECT_EQ(reserved.process_id, created.process->identity().process_id);
            EXPECT_EQ(reserved.process_id, current.process->identity().process_id);
            EXPECT_EQ((std::vector<uint32_t>{reserved.process_id, reserved.process_id}), registry.publication_process_ids());
            EXPECT_EQ(200u, current.process->identity().parent_process_id);
            EXPECT_EQ(0u, registry.discard_attempts.load());
        }
    }

    TEST(ProcessSession, FailedPublicationRetryAndDiscardAreRecoveredInTheBackground)
    {
        faulting_session_registry registry{true};
        const auto original = registry.registry.register_process({.process_id = 100, .parent_process_id = 200});
        ASSERT_TRUE(original);
        process_session publisher{registry.endpoint()};
        const auto failed = publisher.register_process({.process_id = 300, .parent_process_id = 100});
        EXPECT_EQ(process_error::communication_failure, failed.error);
        ASSERT_TRUE(registry.wait_for_discard());
        EXPECT_GE(registry.discard_attempts.load(), 2u);
        const auto restored = registry.registry.lookup(100);
        ASSERT_TRUE(restored);
        EXPECT_EQ(original.process->identity().process_id, restored.process->identity().process_id);
    }

    TEST(ProcessSession, FailedExitReportsRetryTheGuestResultAfterTheHostFallback)
    {
        faulting_session_registry registry{false};
        const auto registered = registry.registry.register_process({.process_id = 100});
        ASSERT_TRUE(registered);
        process_session reporter{registry.endpoint()};
        const auto owned = reporter.lookup(100);
        ASSERT_TRUE(owned);
        subprocess_process_manager observer{"unused", unused_wait_arguments, create_subprocess_launcher(), registry.endpoint()};
        const auto opened = observer.open_process(100);
        ASSERT_TRUE(opened);

        registry.drop_exit_reports = true;
        EXPECT_EQ(process_error::communication_failure,
                  reporter.mark_exit(100, {.kind = process_exit_kind::exited, .code = 37, .cycles = 1234}));
        EXPECT_EQ(process_error::communication_failure, reporter.mark_exit(100, {.kind = process_exit_kind::runtime_failure, .code = 1}));
        ASSERT_TRUE(registry.wait_for_exit_attempts(2));
        EXPECT_FALSE(registered.process->exit_status().status);
        registry.drop_exit_reports = false;

        ASSERT_TRUE(registry.wait_for_exit_delivery());
        const auto owner_status = reporter.exit_status(owned.process);
        const auto opened_status = observer.exit_status(opened.process.handle);
        ASSERT_TRUE(owner_status.status);
        ASSERT_TRUE(opened_status.status);
        EXPECT_EQ(process_exit_kind::exited, owner_status.status->kind);
        EXPECT_EQ(37u, owner_status.status->code);
        EXPECT_EQ(1234u, owner_status.status->cycles);
        EXPECT_EQ(owner_status.status->kind, opened_status.status->kind);
        EXPECT_EQ(owner_status.status->code, opened_status.status->code);
        EXPECT_EQ(owner_status.status->cycles, opened_status.status->cycles);
    }

    TEST(ProcessSession, ShutdownCancelsARefreshBeforeTheRegistryReplies)
    {
        gated_session_registry registry{};
        auto manager = std::make_unique<subprocess_process_manager>("unused", unused_wait_arguments, create_subprocess_launcher(),
                                                                    registry.endpoint(), std::chrono::milliseconds::max());
        const auto opened = manager->open_process(100);
        ASSERT_TRUE(opened);
        EXPECT_FALSE(manager->poll_exit_status(opened.process.handle).status);
        ASSERT_EQ(1u, registry.wait_for_batch(0).size());
        std::promise<void> destroyed{};
        auto completed = destroyed.get_future();
        std::thread destroyer([manager = std::move(manager), &destroyed]() mutable {
            manager.reset();
            destroyed.set_value();
        });
        EXPECT_EQ(std::future_status::ready, completed.wait_for(std::chrono::seconds(5)));
        registry.release(1, false);
        destroyer.join();
    }
}
#endif

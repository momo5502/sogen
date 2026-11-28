#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetEvent(const syscall_context& c, uint64_t handle, emulator_object<LONG> previous_state);
    NTSTATUS handle_NtPulseEvent(const syscall_context& c, uint64_t handle, emulator_object<LONG> previous_state);
    NTSTATUS handle_NtReleaseSemaphore(const syscall_context& c, handle semaphore_handle, LONG release_count,
                                       emulator_object<LONG> previous_count);
    NTSTATUS handle_NtReleaseMutant(const syscall_context& c, handle mutant_handle, emulator_object<LONG> previous_count);
    NTSTATUS handle_NtSuspendThread(const syscall_context& c, handle thread_handle, emulator_object<ULONG> previous_suspend_count);
    NTSTATUS handle_NtResumeThread(const syscall_context& c, handle thread_handle, emulator_object<ULONG> previous_suspend_count);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtAlertThread(const syscall_context& c, handle thread_handle);
    NTSTATUS handle_NtQueueApcThread(const syscall_context& c, handle thread_handle, uint64_t apc_routine, uint64_t apc_argument1,
                                     uint64_t apc_argument2, uint64_t apc_argument3);
    DWORD handle_NtUserMsgWaitForMultipleObjectsEx(const syscall_context& c, ULONG count, emulator_object<handle> handles, DWORD timeout,
                                                   DWORD wake_mask, DWORD flags);
}

namespace sogen::test
{
    namespace
    {
        class ObjectWaitListTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));

                this->runner = &this->add_thread(1);
                this->emu->vcpu(0).active_thread = this->runner;
            }

            emulator_thread& add_thread(const uint32_t id)
            {
                auto& threads = this->emu->process.threads;
                return *threads.get(threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, true}));
            }

            emulator_thread& thread(const uint32_t id)
            {
                auto* found = this->emu->process.find_thread_by_id(id);
                if (!found)
                {
                    throw std::runtime_error("No thread " + std::to_string(id));
                }

                return *found;
            }

            handle handle_of(const emulator_thread& t) const
            {
                return this->emu->process.threads.find_handle(t);
            }

            handle store_event(const EVENT_TYPE type, const bool signaled = false)
            {
                event e{};
                e.type = type;
                e.set_signaled(signaled);
                return this->emu->process.events.store(std::move(e));
            }

            handle store_semaphore(const uint32_t count)
            {
                semaphore s{};
                s.current_count = count;
                s.max_count = 10;
                return this->emu->process.semaphores.store(std::move(s));
            }

            handle store_mutant(const std::optional<uint32_t> owner = {})
            {
                mutant m{};
                if (owner)
                {
                    m.try_lock(mutant::make_owner(this->emu->process.process_id, *owner));
                }

                return this->emu->process.mutants.store(std::move(m));
            }

            event& event_of(const handle h)
            {
                return *this->emu->process.events.get(h);
            }

            semaphore& semaphore_of(const handle h)
            {
                return *this->emu->process.semaphores.get(h);
            }

            mutant& mutant_of(const handle h)
            {
                return *this->emu->process.mutants.get(h);
            }

            void wait(emulator_thread& t, std::vector<handle> objects, const bool any = true, const bool alertable = false)
            {
                const auto ready = this->emu->synchronize([&] {
                    t.await_objects.clear();
                    for (const auto object : objects)
                    {
                        t.await_objects.push_back(this->emu->process.reference_of(object));
                    }
                    t.await_any = any;
                    t.apc_alertable = alertable;
                    return t.is_thread_ready(*this->emu);
                });
                ASSERT_FALSE(ready);
            }

            bool check(emulator_thread& t)
            {
                return this->emu->synchronize([&] { return t.is_thread_ready(*this->emu); });
            }

            bool parked(const emulator_thread& t)
            {
                return this->emu->synchronize([&] { return t.is_parked(this->emu->clock().steady_now(), this->emu->process); });
            }

            template <typename Function>
            NTSTATUS call(Function&& function)
            {
                return this->emu->synchronize([&] {
                    const syscall_context c{
                        .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                    return function(c);
                });
            }

            NTSTATUS set_event(const handle h)
            {
                return this->call([&](const syscall_context& c) { return syscalls::handle_NtSetEvent(c, h.bits, {this->emu->memory}); });
            }

            NTSTATUS pulse_event(const handle h)
            {
                return this->call([&](const syscall_context& c) { return syscalls::handle_NtPulseEvent(c, h.bits, {this->emu->memory}); });
            }

            NTSTATUS release_semaphore(const handle h, const LONG count)
            {
                return this->call(
                    [&](const syscall_context& c) { return syscalls::handle_NtReleaseSemaphore(c, h, count, {this->emu->memory}); });
            }

            NTSTATUS release_mutant(const handle h, emulator_thread& owner)
            {
                this->emu->vcpu(0).active_thread = &owner;
                const auto status =
                    this->call([&](const syscall_context& c) { return syscalls::handle_NtReleaseMutant(c, h, {this->emu->memory}); });
                this->emu->vcpu(0).active_thread = this->runner;
                return status;
            }

            NTSTATUS suspend(const emulator_thread& t)
            {
                return this->call(
                    [&](const syscall_context& c) { return syscalls::handle_NtSuspendThread(c, this->handle_of(t), {this->emu->memory}); });
            }

            NTSTATUS resume(const emulator_thread& t)
            {
                return this->call(
                    [&](const syscall_context& c) { return syscalls::handle_NtResumeThread(c, this->handle_of(t), {this->emu->memory}); });
            }

            static std::vector<uint32_t> waiters(const object_wait_list& list)
            {
                std::vector<uint32_t> ids{};
                for (const auto& e : list.entries())
                {
                    ids.push_back(e.thread_id);
                }

                return ids;
            }

            static std::optional<NTSTATUS> wait_status(const NTSTATUS status)
            {
                return status;
            }

            std::unique_ptr<windows_emulator> emu{};
            emulator_thread* runner{};
        };
    }

    TEST_F(ObjectWaitListTest, SynchronizationEventEndsTheWaitsInTheOrderTheyCame)
    {
        const auto e = this->store_event(SynchronizationEvent);
        auto& a = this->add_thread(11);
        auto& b = this->add_thread(12);
        auto& c = this->add_thread(13);
        this->wait(b, {e});
        this->wait(a, {e});
        this->wait(c, {e});
        EXPECT_EQ(this->waiters(this->event_of(e).waiters), (std::vector<uint32_t>{12, 11, 13}));

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), b.pending_status);
        EXPECT_FALSE(a.pending_status.has_value());
        EXPECT_FALSE(c.pending_status.has_value());
        EXPECT_FALSE(this->event_of(e).is_signaled());
        EXPECT_EQ(this->waiters(this->event_of(e).waiters), (std::vector<uint32_t>{11, 13}));

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), a.pending_status);
        EXPECT_FALSE(c.pending_status.has_value());

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), c.pending_status);
        EXPECT_TRUE(this->event_of(e).waiters.empty());
        EXPECT_FALSE(this->event_of(e).is_signaled());

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_TRUE(this->event_of(e).is_signaled());
    }

    TEST_F(ObjectWaitListTest, NotificationEventEndsEveryWaitAndStaysSet)
    {
        const auto e = this->store_event(NotificationEvent);
        auto& a = this->add_thread(11);
        auto& b = this->add_thread(12);
        this->wait(a, {e});
        this->wait(b, {e});

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), a.pending_status);
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), b.pending_status);
        EXPECT_TRUE(this->event_of(e).is_signaled());
        EXPECT_TRUE(this->event_of(e).waiters.empty());
    }

    TEST_F(ObjectWaitListTest, PulseEndsTheWaitsOfThatMomentAndLeavesTheEventClear)
    {
        const auto automatic = this->store_event(SynchronizationEvent);
        const auto manual = this->store_event(NotificationEvent);
        auto& a = this->add_thread(11);
        auto& b = this->add_thread(12);
        auto& c = this->add_thread(13);
        auto& d = this->add_thread(14);
        this->wait(a, {automatic});
        this->wait(b, {automatic});
        this->wait(c, {manual});
        this->wait(d, {manual});

        EXPECT_EQ(STATUS_SUCCESS, this->pulse_event(automatic));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), a.pending_status);
        EXPECT_FALSE(b.pending_status.has_value());
        EXPECT_FALSE(this->event_of(automatic).is_signaled());

        EXPECT_EQ(STATUS_SUCCESS, this->pulse_event(manual));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), c.pending_status);
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), d.pending_status);
        EXPECT_FALSE(this->event_of(manual).is_signaled());

        auto& late = this->add_thread(15);
        this->wait(late, {manual});
        EXPECT_FALSE(late.pending_status.has_value());
    }

    TEST_F(ObjectWaitListTest, SemaphoreUnitsGoToItsWaitersOneEachInOrder)
    {
        const auto s = this->store_semaphore(0);
        auto& a = this->add_thread(11);
        auto& b = this->add_thread(12);
        auto& c = this->add_thread(13);
        this->wait(c, {s});
        this->wait(a, {s});
        this->wait(b, {s});

        EXPECT_EQ(STATUS_SUCCESS, this->release_semaphore(s, 2));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), c.pending_status);
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), a.pending_status);
        EXPECT_FALSE(b.pending_status.has_value());
        EXPECT_EQ(0u, this->semaphore_of(s).current_count);

        EXPECT_EQ(STATUS_SUCCESS, this->release_semaphore(s, 2));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), b.pending_status);
        EXPECT_EQ(1u, this->semaphore_of(s).current_count);
        EXPECT_TRUE(this->semaphore_of(s).waiters.empty());
    }

    TEST_F(ObjectWaitListTest, MutantGoesToItsFirstWaiterAtItsOwnersLastRelease)
    {
        auto& owner = this->add_thread(20);
        const auto m = this->store_mutant(owner.id);
        this->mutant_of(m).try_lock(mutant::make_owner(this->emu->process.process_id, owner.id));
        auto& a = this->add_thread(11);
        auto& b = this->add_thread(12);
        this->wait(a, {m});
        this->wait(b, {m});

        EXPECT_EQ(STATUS_SUCCESS, this->release_mutant(m, owner));
        EXPECT_FALSE(a.pending_status.has_value());
        EXPECT_EQ(mutant::make_owner(this->emu->process.process_id, owner.id), this->mutant_of(m).owner());

        EXPECT_EQ(STATUS_SUCCESS, this->release_mutant(m, owner));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), a.pending_status);
        EXPECT_FALSE(b.pending_status.has_value());
        EXPECT_EQ(mutant::make_owner(this->emu->process.process_id, a.id), this->mutant_of(m).owner());
        EXPECT_EQ(1u, this->mutant_of(m).lock_state().locked_count);

        EXPECT_EQ(STATUS_SUCCESS, this->release_mutant(m, a));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), b.pending_status);
        EXPECT_EQ(mutant::make_owner(this->emu->process.process_id, b.id), this->mutant_of(m).owner());
    }

    TEST_F(ObjectWaitListTest, AbandonedMutantGoesToItsFirstWaiterAsAbandoned)
    {
        auto& owner = this->add_thread(20);
        const auto m = this->store_mutant(owner.id);
        const auto e = this->store_event(SynchronizationEvent);
        auto& a = this->add_thread(11);
        this->wait(a, {e, m});

        this->emu->synchronize([&] { this->emu->process.terminate_thread(owner, STATUS_SUCCESS); });
        EXPECT_EQ(this->wait_status(STATUS_ABANDONED_WAIT_0 + 1), a.pending_status);
        EXPECT_EQ(mutant::make_owner(this->emu->process.process_id, a.id), this->mutant_of(m).owner());
        EXPECT_FALSE(this->mutant_of(m).is_abandoned());
        EXPECT_TRUE(this->event_of(e).waiters.empty());
    }

    TEST_F(ObjectWaitListTest, WaitAnyEndsWithTheIndexOfTheObjectThatSignaled)
    {
        const auto first = this->store_event(SynchronizationEvent);
        const auto second = this->store_event(SynchronizationEvent);
        const auto s = this->store_semaphore(0);
        auto& a = this->add_thread(11);
        auto& b = this->add_thread(12);
        this->wait(a, {first, second});
        this->wait(b, {s, s});
        EXPECT_EQ(2u, this->semaphore_of(s).waiters.size());

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(second));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0 + 1), a.pending_status);
        EXPECT_TRUE(this->event_of(first).waiters.empty());

        EXPECT_EQ(STATUS_SUCCESS, this->release_semaphore(s, 1));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), b.pending_status);
        EXPECT_TRUE(this->semaphore_of(s).waiters.empty());
    }

    TEST_F(ObjectWaitListTest, WaitAllStaysOffTheListsAndEndsInItsOwnCheck)
    {
        const auto first = this->store_event(SynchronizationEvent);
        const auto second = this->store_event(SynchronizationEvent);
        auto& a = this->add_thread(11);
        this->wait(a, {first, second}, false);
        EXPECT_TRUE(this->event_of(first).waiters.empty());
        EXPECT_FALSE(this->parked(a));

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(first));
        EXPECT_FALSE(a.pending_status.has_value());
        EXPECT_TRUE(this->event_of(first).is_signaled());

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(second));
        EXPECT_TRUE(this->check(a));
        EXPECT_EQ(this->wait_status(STATUS_SUCCESS), a.pending_status);
        EXPECT_FALSE(this->event_of(first).is_signaled());
        EXPECT_FALSE(this->event_of(second).is_signaled());
    }

    TEST_F(ObjectWaitListTest, SuspensionTakesAWaiterOffItsListsAndResumptionPutsItBackAtTheTail)
    {
        const auto e = this->store_event(SynchronizationEvent);
        auto& a = this->add_thread(11);
        auto& b = this->add_thread(12);
        auto& c = this->add_thread(13);
        this->wait(a, {e});
        this->wait(b, {e});

        EXPECT_EQ(STATUS_SUCCESS, this->suspend(a));
        EXPECT_EQ(this->waiters(this->event_of(e).waiters), (std::vector<uint32_t>{12}));
        EXPECT_FALSE(this->parked(a));

        this->wait(c, {e});
        EXPECT_EQ(STATUS_SUCCESS, this->resume(a));
        EXPECT_EQ(this->waiters(this->event_of(e).waiters), (std::vector<uint32_t>{12, 13, 11}));
        EXPECT_TRUE(this->parked(a));

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), b.pending_status);
        EXPECT_FALSE(a.pending_status.has_value());
    }

    TEST_F(ObjectWaitListTest, AWaiterResumedWithItsObjectSignaledTakesItInItsNextCheck)
    {
        const auto e = this->store_event(SynchronizationEvent);
        auto& a = this->add_thread(11);
        this->wait(a, {e});

        EXPECT_EQ(STATUS_SUCCESS, this->suspend(a));
        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_FALSE(a.pending_status.has_value());
        EXPECT_TRUE(this->event_of(e).is_signaled());

        EXPECT_EQ(STATUS_SUCCESS, this->resume(a));
        EXPECT_TRUE(this->event_of(e).waiters.empty());
        EXPECT_FALSE(this->parked(a));
        EXPECT_TRUE(this->check(a));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), a.pending_status);
        EXPECT_FALSE(this->event_of(e).is_signaled());
    }

    TEST_F(ObjectWaitListTest, TerminationTakesAWaiterOffItsLists)
    {
        const auto e = this->store_event(SynchronizationEvent);
        auto& a = this->add_thread(11);
        this->wait(a, {e});

        this->emu->synchronize([&] { this->emu->process.terminate_thread(a, STATUS_SUCCESS); });
        EXPECT_TRUE(this->event_of(e).waiters.empty());

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_FALSE(a.pending_status.has_value());
        EXPECT_TRUE(this->event_of(e).is_signaled());
    }

    TEST_F(ObjectWaitListTest, AClosedObjectTakesItsWaitersOffAndANewObjectOnItsHandleIsNotTheirs)
    {
        const auto e = this->store_event(SynchronizationEvent);
        auto& a = this->add_thread(11);
        this->wait(a, {e});
        EXPECT_TRUE(this->parked(a));

        EXPECT_EQ(STATUS_SUCCESS, this->call([&](const syscall_context& c) { return syscalls::handle_NtClose(c, e); }));
        ASSERT_EQ(nullptr, this->emu->process.events.get(e));
        EXPECT_FALSE(this->parked(a));
        EXPECT_FALSE(this->check(a));

        EXPECT_EQ(e, this->store_event(SynchronizationEvent));
        EXPECT_FALSE(this->check(a));
        EXPECT_TRUE(this->event_of(e).waiters.empty());
        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_FALSE(a.pending_status.has_value());
    }

    TEST_F(ObjectWaitListTest, ASnapshotKeepsEachListsOrder)
    {
        const auto e = this->store_event(SynchronizationEvent);
        const auto s = this->store_semaphore(0);
        const auto m = this->store_mutant(20);
        this->add_thread(20);
        this->wait(this->add_thread(13), {e, s});
        this->wait(this->add_thread(11), {m, e});
        this->wait(this->add_thread(12), {e});
        this->wait(this->add_thread(14), {s, m});

        utils::buffer_serializer snapshot{};
        this->emu->serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        this->emu->deserialize(input);
        this->runner = &this->thread(1);
        this->emu->vcpu(0).active_thread = this->runner;

        EXPECT_EQ(this->waiters(this->event_of(e).waiters), (std::vector<uint32_t>{13, 11, 12}));
        EXPECT_EQ(this->waiters(this->semaphore_of(s).waiters), (std::vector<uint32_t>{13, 14}));
        EXPECT_EQ(this->waiters(this->mutant_of(m).waiters), (std::vector<uint32_t>{11, 14}));
        for (const auto id : {11u, 12u, 13u, 14u})
        {
            EXPECT_TRUE(this->parked(this->thread(id))) << id;
        }

        EXPECT_EQ(STATUS_SUCCESS, this->set_event(e));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), this->thread(13).pending_status);
        EXPECT_EQ(this->waiters(this->semaphore_of(s).waiters), (std::vector<uint32_t>{14}));

        EXPECT_EQ(STATUS_SUCCESS, this->release_mutant(m, this->thread(20)));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), this->thread(11).pending_status);
        EXPECT_EQ(this->waiters(this->event_of(e).waiters), (std::vector<uint32_t>{12}));
    }

    TEST_F(ObjectWaitListTest, AMessageWaitWithoutAWakeMaskEndsOnAnObjectAlreadySignaled)
    {
        constexpr DWORD infinite_timeout = 0xFFFFFFFF;
        const auto e = this->store_event(SynchronizationEvent, true);
        const auto s = this->store_semaphore(1);
        const auto handles = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);

        uint32_t id = 11;
        for (const auto object : {e, s})
        {
            auto& t = this->add_thread(id++);
            this->emu->memory.write_memory(handles, &object, sizeof(object));
            this->emu->vcpu(0).active_thread = &t;
            this->call([&](const syscall_context& c) {
                return static_cast<NTSTATUS>(
                    syscalls::handle_NtUserMsgWaitForMultipleObjectsEx(c, 1, {this->emu->memory, handles}, infinite_timeout, 0, 0));
            });
            this->emu->vcpu(0).active_thread = this->runner;

            EXPECT_FALSE(this->parked(t)) << t.id;
            EXPECT_TRUE(this->check(t)) << t.id;
            EXPECT_EQ(this->wait_status(STATUS_WAIT_0), t.pending_status) << t.id;
        }

        EXPECT_FALSE(this->event_of(e).is_signaled());
        EXPECT_EQ(0u, this->semaphore_of(s).current_count);
    }

    TEST_F(ObjectWaitListTest, AWaitAUserApcEndedLeavesItsListsAndAReleaseGoesToTheNextWaiter)
    {
        constexpr uint64_t never_run_apc_routine = 0x10000;
        const auto s = this->store_semaphore(0);
        auto& a = this->add_thread(11);
        auto& b = this->add_thread(12);
        this->wait(a, {s}, true, true);
        this->wait(b, {s});

        EXPECT_EQ(STATUS_SUCCESS, this->call([&](const syscall_context& c) {
            return syscalls::handle_NtQueueApcThread(c, this->handle_of(a), never_run_apc_routine, 0, 0, 0);
        }));
        EXPECT_EQ(this->waiters(this->semaphore_of(s).waiters), (std::vector<uint32_t>{12}));

        EXPECT_EQ(STATUS_SUCCESS, this->release_semaphore(s, 1));
        EXPECT_EQ(STATUS_SUCCESS, this->suspend(a));
        EXPECT_EQ(this->wait_status(STATUS_WAIT_0), b.pending_status);
        EXPECT_FALSE(a.pending_status.has_value());

        EXPECT_EQ(STATUS_SUCCESS, this->resume(a));
        EXPECT_TRUE(this->semaphore_of(s).waiters.empty());
        EXPECT_FALSE(this->parked(a)) << "its APC is to run";
    }

    TEST_F(ObjectWaitListTest, AResumedAlertableWaitWithItsAlertPendingIsNotParked)
    {
        const auto e = this->store_event(SynchronizationEvent);
        auto& a = this->add_thread(11);
        this->wait(a, {e}, true, true);
        EXPECT_TRUE(this->parked(a));

        EXPECT_EQ(STATUS_SUCCESS, this->suspend(a));
        EXPECT_EQ(STATUS_SUCCESS,
                  this->call([&](const syscall_context& c) { return syscalls::handle_NtAlertThread(c, this->handle_of(a)); }));
        EXPECT_TRUE(a.user_alerted);
        EXPECT_EQ(STATUS_SUCCESS, this->resume(a));

        EXPECT_FALSE(this->parked(a));
        EXPECT_TRUE(this->check(a));
        EXPECT_EQ(this->wait_status(STATUS_ALERTED), a.pending_status);
        EXPECT_TRUE(this->event_of(e).waiters.empty());
    }
}

#include "emulation_test_utils.hpp"
#include <devices/named_pipe.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtCreateKey(const syscall_context& c, emulator_object<handle> key_handle, ACCESS_MASK desired_access,
                                emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, ULONG title_index,
                                emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> key_class, ULONG create_options,
                                emulator_object<ULONG> disposition);
    NTSTATUS handle_NtSetValueKey(const syscall_context& c, handle key_handle,
                                  emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name, ULONG title_index, ULONG type,
                                  uint64_t data, ULONG data_size);
    NTSTATUS handle_NtDeleteKey(const syscall_context& c, handle key_handle);
    NTSTATUS handle_NtDeleteValueKey(const syscall_context& c, handle key_handle,
                                     emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name);
    NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, handle process_handle, emulator_object<uint64_t> base_address,
                                           emulator_object<uint32_t> bytes_to_protect, uint32_t protection,
                                           emulator_object<uint32_t> old_protection);
    NTSTATUS handle_NtNotifyChangeKey(const syscall_context& c, handle key_handle, handle event, uint64_t apc_routine, uint64_t apc_context,
                                      uint64_t io_status_block, ULONG completion_filter, BOOLEAN watch_tree, uint64_t buffer,
                                      ULONG buffer_size, BOOLEAN asynchronous);
    NTSTATUS handle_NtNotifyChangeMultipleKeys(const syscall_context& c, handle master_key_handle, ULONG count,
                                               uint64_t subordinate_objects, handle event, uint64_t apc_routine, uint64_t apc_context,
                                               uint64_t io_status_block, ULONG completion_filter, BOOLEAN watch_tree, uint64_t buffer,
                                               ULONG buffer_size, BOOLEAN asynchronous);
    NTSTATUS handle_NtCreateEvent(const syscall_context& c, emulator_object<handle> event_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, EVENT_TYPE event_type,
                                  BOOLEAN initial_state);
    NTSTATUS handle_NtClearEvent(const syscall_context& c, handle event_handle);
    NTSTATUS handle_NtQueryEvent(const syscall_context& c, handle event_handle, uint32_t event_information_class,
                                 emulator_object<EVENT_BASIC_INFORMATION> event_information, uint32_t event_information_length,
                                 emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, emulator_object<handle> semaphore_handle, ACCESS_MASK desired_access,
                                      emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes, LONG initial_count,
                                      LONG maximum_count);
    NTSTATUS handle_NtTerminateThread(const syscall_context& c, handle thread_handle, NTSTATUS exit_status);
    NTSTATUS handle_NtTerminateProcess(const syscall_context& c, handle process_handle, NTSTATUS exit_status);
}

namespace sogen::test
{
    namespace
    {
        constexpr ULONG change_name = 0x1;
        constexpr ULONG change_last_set = 0x4;
        constexpr ULONG thread_agnostic = 0x10000000;
        constexpr uint64_t untouched_status_block = 0xAAAAAAAAAAAAAAAA;
        constexpr uint64_t apc_routine = 0x7FF612340000;
        constexpr uint64_t apc_context = 0x1234;
        constexpr std::u16string_view doomed_key_path = u"\\Registry\\Machine\\Software\\SogenNotify\\Doomed";

        using status_block_object = emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>;

        class RegistryChangeNotificationTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu =
                    std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                       emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root()},
                                                       emulator_callbacks{}, std::move(interfaces));
                this->requester = this->add_thread(8);
                this->other_thread = this->add_thread(12);
                this->activate(this->requester);

                const auto storage = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->allocator.emplace(this->emu->memory, storage, 0x10000);
                this->watched = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify");
                this->child = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify\\Child");
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            handle add_thread(const uint32_t id)
            {
                return this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, true});
            }

            void activate(const handle thread_handle)
            {
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread_handle);
            }

            emulator_thread& thread_object(const handle thread_handle)
            {
                return *this->emu->process.threads.get(thread_handle);
            }

            emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> named_attributes(const std::u16string_view path)
            {
                const auto name = this->allocator->make_unicode_string(path);
                const auto attributes = this->allocator->reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> values{};
                values.Length = sizeof(values);
                values.ObjectName = name.value();
                attributes.write(values);
                return attributes;
            }

            handle create_key(const std::u16string_view path)
            {
                const auto key = this->allocator->reserve<handle>();
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateKey(this->context(), key, 0, this->named_attributes(path), 0,
                                                                       {this->emu->memory}, 0, {this->emu->memory}));
                return key.read();
            }

            void set_value(const handle key, const std::u16string_view name, const std::optional<ULONG> value = std::nullopt)
            {
                const auto value_name = this->allocator->make_unicode_string(name);
                const auto data = this->allocator->reserve<ULONG>();
                data.write(value.value_or(++this->last_value));
                ASSERT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtSetValueKey(this->context(), key, value_name, 0, REG_DWORD, data.value(), sizeof(ULONG)));
            }

            handle create_event(const bool signaled = false)
            {
                const auto event_handle = this->allocator->reserve<handle>();
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateEvent(this->context(), event_handle, 0, {this->emu->memory},
                                                                         NotificationEvent, signaled ? TRUE : FALSE));
                return event_handle.read();
            }

            bool is_signaled(const handle event_handle)
            {
                const auto information = this->allocator->reserve<EVENT_BASIC_INFORMATION>();
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryEvent(this->context(), event_handle, 0, information,
                                                                        sizeof(EVENT_BASIC_INFORMATION), {this->emu->memory}));
                return information.read().EventState != 0;
            }

            status_block_object status_block()
            {
                const auto block = this->allocator->reserve<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>();
                const std::array pattern{untouched_status_block, untouched_status_block};
                this->emu->memory.write_memory(block.value(), pattern.data(), sizeof(pattern));
                return block;
            }

            NTSTATUS notify(const handle key_handle, const handle completion_event, const status_block_object block, const ULONG filter,
                            const BOOLEAN watch_tree = FALSE, const BOOLEAN asynchronous = TRUE, const uint64_t routine = 0)
            {
                return syscalls::handle_NtNotifyChangeKey(this->context(), key_handle, completion_event, routine, routine ? apc_context : 0,
                                                          block.value(), filter, watch_tree, 0, 0, asynchronous);
            }

            NTSTATUS notify_synchronously(const handle key_handle, const status_block_object block)
            {
                return this->emu->synchronize([&] { return this->notify(key_handle, {}, block, change_last_set, FALSE, FALSE); });
            }

            void delete_value(const handle key, const std::u16string_view name)
            {
                ASSERT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtDeleteValueKey(this->context(), key, this->allocator->make_unicode_string(name)));
            }

            static void expect_status(const status_block_object block, const NTSTATUS status)
            {
                const auto value = block.read();
                EXPECT_EQ(static_cast<uint32_t>(value.Status), static_cast<uint32_t>(status));
                EXPECT_EQ(value.Information, 0u);
            }

            static void expect_untouched(const status_block_object block)
            {
                EXPECT_EQ(block.read().Information, untouched_status_block);
                EXPECT_EQ(static_cast<uint32_t>(block.read().Status), static_cast<uint32_t>(untouched_status_block));
            }

            std::unique_ptr<windows_emulator> emu{};
            std::optional<emulator_allocator> allocator{};
            ULONG last_value{};
            handle requester{};
            handle other_thread{};
            handle watched{};
            handle child{};
        };
    }

    TEST_F(RegistryChangeNotificationTest, AsynchronousRequestPendsUntilAValueChanges)
    {
        const auto completion_event = this->create_event(true);
        const auto block = this->status_block();

        EXPECT_EQ(STATUS_PENDING, this->notify(this->watched, completion_event, block, change_last_set));
        expect_status(block, STATUS_PENDING);
        EXPECT_FALSE(this->is_signaled(completion_event));

        const auto writer = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify");
        this->set_value(writer, u"A");
        expect_status(block, STATUS_NOTIFY_ENUM_DIR);
        EXPECT_TRUE(this->is_signaled(completion_event));
        EXPECT_EQ(this->emu->memory.read_memory<uint32_t>(block.value() + 4), static_cast<uint32_t>(untouched_status_block));
    }

    TEST_F(RegistryChangeNotificationTest, RearmedRequestRemembersChangesMadeWhileNothingWasPending)
    {
        const auto completion_event = this->create_event();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, completion_event, this->status_block(), change_last_set));
        this->set_value(this->watched, u"A");

        const auto rearmed = this->status_block();
        EXPECT_EQ(STATUS_PENDING, this->notify(this->watched, completion_event, rearmed, change_last_set));
        expect_status(rearmed, STATUS_PENDING);
        EXPECT_FALSE(this->is_signaled(completion_event));
        this->set_value(this->watched, u"A");
        expect_status(rearmed, STATUS_NOTIFY_ENUM_DIR);

        this->set_value(this->watched, u"A");
        this->set_value(this->watched, u"B");
        const auto after_changes = this->status_block();
        EXPECT_EQ(STATUS_SUCCESS, this->notify(this->watched, completion_event, after_changes, change_last_set));
        expect_status(after_changes, STATUS_NOTIFY_ENUM_DIR);
        EXPECT_TRUE(this->is_signaled(completion_event));
    }

    TEST_F(RegistryChangeNotificationTest, LaterRequestsKeepTheFilterAndSubtreeFlagOfTheFirst)
    {
        const auto first = this->status_block();
        const auto second = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, this->create_event(), first, change_name));
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, this->create_event(), second, change_last_set, TRUE));

        this->set_value(this->watched, u"A");
        this->set_value(this->child, u"C");
        expect_status(first, STATUS_PENDING);
        expect_status(second, STATUS_PENDING);

        this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify\\Child2");
        expect_status(first, STATUS_NOTIFY_ENUM_DIR);
        expect_status(second, STATUS_NOTIFY_ENUM_DIR);

        const auto shallow_first = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify");
        ASSERT_EQ(STATUS_PENDING, this->notify(shallow_first, {}, this->status_block(), change_last_set, FALSE));
        this->set_value(this->watched, u"A");
        const auto tree_rearm = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(shallow_first, {}, tree_rearm, change_last_set, TRUE));
        this->set_value(this->child, u"C");
        expect_status(tree_rearm, STATUS_PENDING);
    }

    TEST_F(RegistryChangeNotificationTest, OnlyCreatingASubKeyCompletesANameRequest)
    {
        const auto name_request = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, this->create_event(), name_request, change_name));
        const auto value_watcher = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify");
        const auto value_request = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(value_watcher, this->create_event(), value_request, change_last_set));

        this->set_value(this->watched, u"A");
        this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify\\Child");
        expect_status(name_request, STATUS_PENDING);
        expect_status(value_request, STATUS_NOTIFY_ENUM_DIR);

        const auto second_value_request = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(value_watcher, {}, second_value_request, change_last_set));
        this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify\\Child2");
        expect_status(name_request, STATUS_NOTIFY_ENUM_DIR);
        expect_status(second_value_request, STATUS_PENDING);
    }

    TEST_F(RegistryChangeNotificationTest, ChangesBelowTheKeyNeedWatchTree)
    {
        const auto shallow = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, this->create_event(), shallow, change_last_set | change_name, FALSE));
        const auto tree_watcher = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify");
        const auto deep = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(tree_watcher, this->create_event(), deep, change_name, TRUE));
        const auto value_tree_watcher = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify");
        const auto deep_value = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(value_tree_watcher, this->create_event(), deep_value, change_last_set, TRUE));

        this->set_value(this->child, u"C");
        this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify\\Child\\Grandchild");
        expect_status(shallow, STATUS_PENDING);
        expect_status(deep, STATUS_NOTIFY_ENUM_DIR);
        expect_status(deep_value, STATUS_NOTIFY_ENUM_DIR);
    }

    TEST_F(RegistryChangeNotificationTest, WritingIdenticalDataIsNoChange)
    {
        this->set_value(this->watched, u"A", 7);
        const auto block = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, {}, block, change_last_set));

        this->set_value(this->watched, u"A", 7);
        expect_status(block, STATUS_PENDING);
        this->set_value(this->watched, u"A", 8);
        expect_status(block, STATUS_NOTIFY_ENUM_DIR);
    }

    TEST_F(RegistryChangeNotificationTest, ApcIsQueuedOnTheRequestingThread)
    {
        const auto block = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, {}, block, change_last_set, FALSE, TRUE, apc_routine));

        this->activate(this->other_thread);
        this->set_value(this->watched, u"A");

        EXPECT_TRUE(this->thread_object(this->other_thread).pending_apcs.empty());
        const auto& apcs = this->thread_object(this->requester).pending_apcs;
        ASSERT_EQ(apcs.size(), 1u);
        EXPECT_EQ(apcs[0].apc_routine, apc_routine);
        EXPECT_EQ(apcs[0].apc_argument1, apc_context);
        EXPECT_EQ(apcs[0].apc_argument2, block.value());
        EXPECT_EQ(apcs[0].apc_argument3, 0u);
    }

    TEST_F(RegistryChangeNotificationTest, ClosingTheLastHandleCompletesEveryPendingRequestWithCleanup)
    {
        const auto first_event = this->create_event();
        const auto second_event = this->create_event();
        const auto first = this->status_block();
        const auto second = this->status_block();
        const auto without_event = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, first_event, first, change_last_set));
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, second_event, second, change_name, TRUE));
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, {}, without_event, change_last_set));
        const auto duplicate = this->emu->process.registry_keys.duplicate(this->watched);
        ASSERT_TRUE(duplicate.has_value());

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), this->watched));
        expect_status(first, STATUS_PENDING);
        EXPECT_FALSE(this->is_signaled(first_event));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), *duplicate));
        expect_status(first, STATUS_NOTIFY_CLEANUP);
        expect_status(second, STATUS_NOTIFY_CLEANUP);
        expect_status(without_event, STATUS_NOTIFY_CLEANUP);
        EXPECT_TRUE(this->is_signaled(first_event));
        EXPECT_TRUE(this->is_signaled(second_event));
    }

    TEST_F(RegistryChangeNotificationTest, DeletingAWatchedKeyCompletesItsRequestsWithCleanup)
    {
        const auto doomed = this->create_key(doomed_key_path);
        const auto waiting = this->create_key(doomed_key_path);
        const auto deleter = this->create_key(doomed_key_path);
        const auto completion_event = this->create_event();
        const auto asynchronous = this->status_block();
        const auto synchronous = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(doomed, completion_event, asynchronous, change_last_set));
        ASSERT_EQ(STATUS_PENDING, this->notify_synchronously(waiting, synchronous));

        this->activate(this->other_thread);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), deleter));
        expect_status(asynchronous, STATUS_NOTIFY_CLEANUP);
        EXPECT_TRUE(this->is_signaled(completion_event));
        EXPECT_TRUE(this->thread_object(this->requester).is_thread_ready(*this->emu));
        EXPECT_EQ(this->thread_object(this->requester).pending_status, std::optional{STATUS_NOTIFY_CLEANUP});
        expect_status(synchronous, STATUS_NOTIFY_CLEANUP);

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClearEvent(this->context(), completion_event));
        const auto rearmed = this->status_block();
        EXPECT_EQ(STATUS_KEY_DELETED, this->notify(doomed, completion_event, rearmed, change_last_set));
        expect_status(rearmed, STATUS_PENDING);
        EXPECT_FALSE(this->is_signaled(completion_event));
    }

    TEST_F(RegistryChangeNotificationTest, RequestOnADeletedKeyFailsAfterWritingTheStatusBlock)
    {
        const auto doomed = this->create_key(doomed_key_path);
        const auto stale = this->create_key(doomed_key_path);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), doomed));

        const auto completion_event = this->create_event();
        const auto block = this->status_block();
        EXPECT_EQ(STATUS_KEY_DELETED, this->notify(stale, completion_event, block, change_last_set));
        expect_status(block, STATUS_PENDING);
        EXPECT_FALSE(this->is_signaled(completion_event));
    }

    TEST_F(RegistryChangeNotificationTest, AnEventClosedWhileARequestPendsIsNotTakenForANewEvent)
    {
        const auto first_event = this->create_event();
        const auto block = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, first_event, block, change_last_set));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), first_event));
        const auto second_event = this->create_event();

        this->set_value(this->watched, u"A");
        expect_status(block, STATUS_NOTIFY_ENUM_DIR);
        EXPECT_FALSE(this->is_signaled(second_event));
    }

    TEST_F(RegistryChangeNotificationTest, ThreadExitEndsItsRequestsUnlessTheyAreThreadAgnostic)
    {
        const auto bound_event = this->create_event();
        const auto agnostic_event = this->create_event();
        const auto bound = this->status_block();
        const auto agnostic = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, bound_event, bound, change_last_set));
        ASSERT_EQ(STATUS_PENDING, this->notify(this->child, agnostic_event, agnostic, thread_agnostic | change_last_set));

        this->activate(this->other_thread);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtTerminateThread(this->context(), this->requester, STATUS_SUCCESS));
        expect_status(bound, STATUS_NOTIFY_CLEANUP);
        EXPECT_TRUE(this->is_signaled(bound_event));
        EXPECT_FALSE(this->is_signaled(agnostic_event));

        this->set_value(this->child, u"C");
        EXPECT_TRUE(this->is_signaled(agnostic_event));
        expect_untouched(agnostic);
    }

    TEST_F(RegistryChangeNotificationTest, ThreadsEndedByNtTerminateProcessEndTheirRequests)
    {
        const auto completion_event = this->create_event();
        const auto block = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, completion_event, block, change_last_set));

        this->activate(this->other_thread);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtTerminateProcess(this->context(), {}, STATUS_SUCCESS));
        expect_status(block, STATUS_NOTIFY_CLEANUP);
        EXPECT_TRUE(this->is_signaled(completion_event));
    }

    TEST_F(RegistryChangeNotificationTest, SynchronousRequestWaitsAlertablyUntilTheKeyChanges)
    {
        const auto block = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify_synchronously(this->watched, block));
        EXPECT_TRUE(this->thread_object(this->requester).apc_alertable);
        EXPECT_FALSE(this->thread_object(this->requester).is_thread_ready(*this->emu));

        this->activate(this->other_thread);
        this->set_value(this->watched, u"A");

        EXPECT_TRUE(this->thread_object(this->requester).is_thread_ready(*this->emu));
        EXPECT_EQ(this->thread_object(this->requester).pending_status, std::optional{STATUS_NOTIFY_ENUM_DIR});
        expect_status(block, STATUS_NOTIFY_ENUM_DIR);

        this->set_value(this->watched, u"A");
        this->activate(this->requester);
        const auto remembered = this->status_block();
        EXPECT_EQ(STATUS_NOTIFY_ENUM_DIR, this->notify_synchronously(this->watched, remembered));
        expect_status(remembered, STATUS_NOTIFY_ENUM_DIR);
    }

    TEST_F(RegistryChangeNotificationTest, SynchronousRequestEndedByAnApcIsDropped)
    {
        const auto block = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify_synchronously(this->watched, block));
        this->thread_object(this->requester).mark_as_ready(STATUS_USER_APC);

        this->activate(this->other_thread);
        this->set_value(this->watched, u"A");
        expect_status(block, STATUS_PENDING);
    }

    TEST_F(RegistryChangeNotificationTest, SynchronousRequestWithAnInterruptedWaitLeavesALaterWaitOnItsStatusBlockAlone)
    {
        const auto block = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify_synchronously(this->watched, block));
        this->thread_object(this->requester).mark_as_ready(STATUS_USER_APC);

        auto pipes = utils::host_pipe::create_pair(false, 4096);
        io_device_container container{u"NamedPipe", *this->emu, {}};
        auto* reader = container.get_internal_device<named_pipe>();
        reader->endpoint = std::move(pipes.first);
        reader->create_options = FILE_SYNCHRONOUS_IO_NONALERT;
        container.granted_access = FILE_READ_DATA;
        const auto reader_handle = this->emu->process.devices.store(std::move(container));

        io_device_context pipe_read{this->emu->memory};
        pipe_read.source_handle = reader_handle;
        pipe_read.io_status_block.set_address(block.value());
        pipe_read.output_buffer = this->allocator->reserve(16, 1);
        pipe_read.output_buffer_length = 16;
        pipe_read.vcpu = &this->emu->vcpu(0);
        auto prepared_pipe_read = this->emu->process.devices.get(reader_handle)->prepare_io_context(pipe_read);
        ASSERT_EQ(STATUS_PENDING, this->emu->synchronize([&] { return reader->start_io(*this->emu, prepared_pipe_read, true); }));
        ASSERT_EQ(this->thread_object(this->requester).await_file_io, block.value());

        this->activate(this->other_thread);
        this->set_value(this->watched, u"A");
        EXPECT_EQ(static_cast<uint32_t>(block.read().Status), static_cast<uint32_t>(STATUS_PENDING));
        EXPECT_FALSE(this->thread_object(this->requester).is_thread_ready(*this->emu));
    }

    TEST_F(RegistryChangeNotificationTest, SynchronousRequestIgnoresItsEventAndApc)
    {
        const auto unsignalled = this->create_event();
        const auto block = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->emu->synchronize([&] {
            return this->notify(this->watched, unsignalled, block, change_last_set, FALSE, FALSE, apc_routine);
        }));

        this->activate(this->other_thread);
        this->set_value(this->watched, u"A");
        EXPECT_TRUE(this->thread_object(this->requester).is_thread_ready(*this->emu));
        EXPECT_EQ(this->thread_object(this->requester).pending_status, std::optional{STATUS_NOTIFY_ENUM_DIR});
        expect_status(block, STATUS_NOTIFY_ENUM_DIR);
        EXPECT_FALSE(this->is_signaled(unsignalled));
        EXPECT_TRUE(this->thread_object(this->requester).pending_apcs.empty());

        this->activate(this->requester);
        const auto signalled = this->create_event(true);
        ASSERT_EQ(STATUS_PENDING, this->emu->synchronize([&] {
            return this->notify(this->child, signalled, this->status_block(), change_last_set, FALSE, FALSE);
        }));
        EXPECT_TRUE(this->is_signaled(signalled));
    }

    TEST_F(RegistryChangeNotificationTest, DeletingASubKeyCompletesNameRequestsOnItsParentAndOnAnAncestorWatchingTheTree)
    {
        const auto leaf = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify\\Child\\Leaf");
        const auto parent_event = this->create_event();
        const auto ancestor_event = this->create_event();
        const auto parent = this->status_block();
        const auto ancestor = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->child, parent_event, parent, change_name));
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, ancestor_event, ancestor, change_name, TRUE));

        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtDeleteKey(this->context(), leaf));
        expect_status(parent, STATUS_NOTIFY_ENUM_DIR);
        expect_status(ancestor, STATUS_NOTIFY_ENUM_DIR);
        EXPECT_TRUE(this->is_signaled(parent_event));
        EXPECT_TRUE(this->is_signaled(ancestor_event));
    }

    TEST_F(RegistryChangeNotificationTest, DeletingAValueCompletesLastSetRequestsOnItsKeyAndOnAnAncestorWatchingTheTree)
    {
        this->set_value(this->watched, u"W");
        this->set_value(this->child, u"V");
        const auto key_event = this->create_event();
        const auto key_request = this->status_block();
        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, key_event, key_request, change_last_set));
        this->delete_value(this->watched, u"W");
        expect_status(key_request, STATUS_NOTIFY_ENUM_DIR);
        EXPECT_TRUE(this->is_signaled(key_event));

        const auto tree_event = this->create_event();
        const auto tree_request = this->status_block();
        const auto tree_watcher = this->create_key(u"\\Registry\\Machine\\Software\\SogenNotify");
        ASSERT_EQ(STATUS_PENDING, this->notify(tree_watcher, tree_event, tree_request, change_last_set, TRUE));
        this->delete_value(this->child, u"V");
        expect_status(tree_request, STATUS_NOTIFY_ENUM_DIR);
        EXPECT_TRUE(this->is_signaled(tree_event));
    }

    TEST_F(RegistryChangeNotificationTest, StatusBlockMaySpanTwoWritableRegions)
    {
        const auto pages = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
        const auto base_address = this->allocator->reserve<uint64_t>();
        const auto bytes_to_protect = this->allocator->reserve<uint32_t>();
        base_address.write(pages + 0x1000);
        bytes_to_protect.write(0x1000);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtProtectVirtualMemory(this->context(), CURRENT_PROCESS, base_address, bytes_to_protect,
                                                                          PAGE_EXECUTE_READWRITE, this->allocator->reserve<uint32_t>()));

        const status_block_object block{this->emu->memory, pages + 0x1000 - 8};
        EXPECT_EQ(STATUS_PENDING, this->notify(this->watched, {}, block, change_last_set));
        expect_status(block, STATUS_PENDING);
    }

    TEST_F(RegistryChangeNotificationTest, ThreadAgnosticRequestNeedsAnAsynchronousEventAndWritesNoStatusBlock)
    {
        const auto completion_event = this->create_event();
        const auto block = this->status_block();
        EXPECT_EQ(STATUS_PENDING, this->notify(this->watched, completion_event, block, thread_agnostic | change_last_set));
        expect_untouched(block);
        this->set_value(this->watched, u"A");
        EXPECT_TRUE(this->is_signaled(completion_event));
        expect_untouched(block);

        const auto rejected = this->status_block();
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->notify(this->watched, {}, rejected, thread_agnostic | change_last_set));
        EXPECT_EQ(STATUS_INVALID_PARAMETER,
                  this->notify(this->watched, {}, rejected, thread_agnostic | change_last_set, FALSE, TRUE, apc_routine));
        EXPECT_EQ(STATUS_INVALID_PARAMETER,
                  this->notify(this->watched, completion_event, rejected, thread_agnostic | change_last_set, FALSE, TRUE, apc_routine));
        EXPECT_EQ(STATUS_INVALID_PARAMETER,
                  this->notify(this->watched, completion_event, rejected, thread_agnostic | change_last_set, FALSE, FALSE));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->notify({}, {}, rejected, thread_agnostic | change_last_set));
        expect_untouched(rejected);
    }

    TEST_F(RegistryChangeNotificationTest, RejectsInvalidArgumentsBeforeOrAfterWritingTheStatusBlock)
    {
        const auto expect_rejected_after_pending = [&](const NTSTATUS expected, const handle key_argument, const handle event_argument,
                                                       const ULONG filter) {
            const auto block = this->status_block();
            EXPECT_EQ(expected, this->notify(key_argument, event_argument, block, filter));
            expect_status(block, STATUS_PENDING);
        };

        const auto semaphore = this->allocator->reserve<handle>();
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateSemaphore(this->context(), semaphore, 0, {this->emu->memory}, 0, 1));
        const auto event_handle = this->create_event();
        handle garbage{};
        garbage.bits = 0x1234;

        expect_rejected_after_pending(STATUS_INVALID_HANDLE, {}, {}, change_last_set);
        expect_rejected_after_pending(STATUS_INVALID_PARAMETER, this->watched, {}, 0x100);
        expect_rejected_after_pending(STATUS_INVALID_PARAMETER, {}, {}, 0x100);
        expect_rejected_after_pending(STATUS_OBJECT_TYPE_MISMATCH, event_handle, {}, change_last_set);
        expect_rejected_after_pending(STATUS_OBJECT_TYPE_MISMATCH, this->watched, this->watched, change_last_set);
        expect_rejected_after_pending(STATUS_OBJECT_TYPE_MISMATCH, this->watched, semaphore.read(), change_last_set);
        expect_rejected_after_pending(STATUS_INVALID_HANDLE, this->watched, garbage, change_last_set);
        expect_rejected_after_pending(STATUS_INVALID_PARAMETER, this->watched, garbage, 0x100);

        const auto null_buffer = this->status_block();
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtNotifyChangeKey(this->context(), {}, {}, 0, 0, null_buffer.value(),
                                                                              change_last_set, FALSE, 0, 16, TRUE));
        expect_untouched(null_buffer);

        const auto read_only = this->emu->memory.allocate_memory(0x1000, memory_permission::read);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION,
                  syscalls::handle_NtNotifyChangeKey(this->context(), {}, {}, 0, 0, read_only, change_last_set, FALSE, 0, 0, TRUE));
        EXPECT_EQ(this->emu->memory.read_memory<uint32_t>(read_only), 0u);
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtNotifyChangeKey(this->context(), {}, {}, 0, 0, 0xFFFFFFFFFFFFFFF8,
                                                                              change_last_set, FALSE, 0, 0, TRUE));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, syscalls::handle_NtNotifyChangeKey(this->context(), {}, {}, 0, 0, 0, 0x100, FALSE, 0, 0, TRUE));

        EXPECT_EQ(STATUS_PENDING, syscalls::handle_NtNotifyChangeKey(this->context(), this->watched, {}, 0, 0, this->status_block().value(),
                                                                     change_last_set, FALSE, this->status_block().value(), 16, TRUE));
        EXPECT_EQ(STATUS_PENDING, this->notify(this->watched, {}, this->status_block(), 0));
    }

    TEST_F(RegistryChangeNotificationTest, MultipleKeysWithoutSubordinatesWatchTheMasterKey)
    {
        const auto completion_event = this->create_event();
        const auto block = this->status_block();
        EXPECT_EQ(STATUS_PENDING, syscalls::handle_NtNotifyChangeMultipleKeys(this->context(), this->watched, 0, 0, completion_event, 0, 0,
                                                                              block.value(), change_last_set, FALSE, 0, 0, TRUE));
        this->set_value(this->watched, u"A");
        expect_status(block, STATUS_NOTIFY_ENUM_DIR);
        EXPECT_TRUE(this->is_signaled(completion_event));

        const auto null_subordinates = this->status_block();
        EXPECT_EQ(STATUS_INVALID_PARAMETER,
                  syscalls::handle_NtNotifyChangeMultipleKeys(this->context(), this->watched, 1, 0, {}, 0, 0, null_subordinates.value(),
                                                              change_last_set, FALSE, 0, 0, TRUE));
        expect_status(null_subordinates, STATUS_PENDING);

        const auto two_subordinates = this->status_block();
        EXPECT_EQ(STATUS_INVALID_PARAMETER,
                  syscalls::handle_NtNotifyChangeMultipleKeys(this->context(), this->watched, 2, 0, {}, 0, 0, two_subordinates.value(),
                                                              change_last_set, FALSE, 0, 0, TRUE));
        expect_untouched(two_subordinates);
    }

    TEST_F(RegistryChangeNotificationTest, Wow64RequestWritesTheCallersThirtyTwoBitStatusBlock)
    {
        this->emu->process.is_wow64_process = true;
        const auto block = this->status_block();
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu32>>> block32{this->emu->memory, block.value()};

        ASSERT_EQ(STATUS_PENDING, this->notify(this->watched, {}, block, change_last_set));
        EXPECT_EQ(static_cast<uint32_t>(block32.read().Status), static_cast<uint32_t>(STATUS_PENDING));
        EXPECT_EQ(block32.read().Information, 0u);
        EXPECT_EQ(block.read().Information, untouched_status_block);

        this->set_value(this->watched, u"A");
        EXPECT_EQ(static_cast<uint32_t>(block32.read().Status), static_cast<uint32_t>(STATUS_NOTIFY_ENUM_DIR));
        EXPECT_EQ(block.read().Information, untouched_status_block);
    }
} // namespace sogen::test

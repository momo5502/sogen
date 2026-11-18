#include "std_include.hpp"

#include <debug_object.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtOpenProcess(const syscall_context& c, emulator_object<handle> process_handle, ACCESS_MASK desired_access,
                                  emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                  emulator_object<CLIENT_ID64> client_id);
    NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, handle process_handle, uint32_t info_class, uint64_t information,
                                              uint32_t information_length, emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        class handle_test_process_manager final : public process_manager
        {
          public:
            process_identity identity{.process_id = 100, .parent_process_id = 200, .native_environment = 0x180000};
            uint64_t next_handle{1};
            std::optional<managed_process> reopened_handle{};
            process_error open_error{process_error::none};
            mutable size_t explicit_queries{};
            bool ready{};
            std::optional<process_debug_event> event{};

            process_open_result open_process(const uint32_t process_id) override
            {
                if (this->open_error != process_error::none)
                {
                    return {.error = this->open_error};
                }
                if (process_id != this->identity.process_id)
                {
                    return {.error = process_error::invalid_process};
                }
                return {.process = {.handle = this->reopened_handle.value_or(managed_process{this->next_handle++}),
                                    .identity = this->identity}};
            }

            process_debug_event_result next_debug_event(managed_process) override
            {
                return {.event = std::exchange(this->event, std::nullopt)};
            }

            process_create_result create_process(process_create_request) override
            {
                return {.error = process_error::unavailable};
            }

            process_error resume_process(managed_process) override
            {
                return process_error::unavailable;
            }

            process_error terminate_process(managed_process, uint64_t) override
            {
                return process_error::unavailable;
            }

            process_exit_status_result exit_status(managed_process) const override
            {
                ++this->explicit_queries;
                return {};
            }

            process_exit_status_result poll_exit_status(managed_process) const override
            {
                return {.status = this->ready ? std::optional{process_exit{.kind = process_exit_kind::exited, .code = 37}} : std::nullopt};
            }

            process_memory_result allocate_memory(managed_process, uint64_t, uint64_t, memory_permission, bool, bool) override
            {
                return {.error = process_error::unavailable};
            }

            process_error free_memory(managed_process, uint64_t, uint64_t, bool) override
            {
                return process_error::unavailable;
            }

            process_memory_result protect_memory(managed_process, uint64_t, uint64_t, memory_permission) override
            {
                return {.error = process_error::unavailable};
            }

            process_memory_read_result read_memory(managed_process, uint64_t, uint64_t) const override
            {
                return {.error = process_error::unavailable};
            }

            process_memory_result write_memory(managed_process, uint64_t, std::span<const uint8_t>) override
            {
                return {.error = process_error::unavailable};
            }
        };

        class ProcessHandleTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{.ui = std::make_unique<null_ui_backend>(),
                                               .audio = std::make_unique<null_audio_backend>(),
                                               .processes = &this->manager,
                                               .console = std::make_unique<null_console_backend>()};
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            handle open(const ULONG attributes = 0)
            {
                const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes{this->emu->memory, this->storage};
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> attribute_values{};
                attribute_values.Length = sizeof(attribute_values);
                attribute_values.Attributes = attributes;
                object_attributes.write(attribute_values);
                const emulator_object<CLIENT_ID64> client{this->emu->memory, this->storage + 0x100};
                CLIENT_ID64 client_id{};
                client_id.UniqueProcess = this->manager.identity.process_id;
                client.write(client_id);
                const emulator_object<handle> output{this->emu->memory, this->storage + 0x200};
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtOpenProcess(this->context(), output, 0, object_attributes, client));
                return output.read();
            }

            handle_test_process_manager manager{};
            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(ProcessHandleTest, FirstOpenPreservesInheritance)
    {
        const auto opened = this->open(OBJ_INHERIT);
        const auto attributes = this->emu->process.processes.attributes_of(opened);
        ASSERT_NE(nullptr, this->emu->process.processes.get(opened));
        EXPECT_TRUE(attributes.inherit);
        EXPECT_FALSE(attributes.protect_from_close);
    }

    TEST_F(ProcessHandleTest, ReopeningAnInstanceSharesItsObjectAndUsesNewAttributes)
    {
        const auto first = this->open();
        const auto* original = this->emu->process.processes.get(first);
        ASSERT_NE(nullptr, original);
        this->manager.reopened_handle = original->process;
        const auto second = this->open(OBJ_INHERIT);
        ASSERT_NE(first.bits, second.bits);
        EXPECT_EQ(this->emu->process.processes.get(first), this->emu->process.processes.get(second));
        EXPECT_FALSE(this->emu->process.processes.attributes_of(first).inherit);
        EXPECT_TRUE(this->emu->process.processes.attributes_of(second).inherit);
    }

    TEST_F(ProcessHandleTest, ReusedPidIsResolvedAgainInsteadOfDuplicatingAnOldInstance)
    {
        const auto first = this->open();
        this->manager.identity.parent_process_id = 300;
        this->manager.identity.native_environment = 0x280000;
        const auto second = this->open();
        const auto* old_process = this->emu->process.processes.get(first);
        const auto* new_process = this->emu->process.processes.get(second);
        ASSERT_NE(nullptr, old_process);
        ASSERT_NE(nullptr, new_process);
        EXPECT_NE(old_process->process, new_process->process);
        EXPECT_EQ(200u, old_process->parent_id);
        EXPECT_EQ(300u, new_process->parent_id);
        EXPECT_EQ(0x280000u, new_process->native_environment);
    }

    TEST_F(ProcessHandleTest, SchedulerObservationAndConsumptionUseReadiness)
    {
        const auto opened = this->open();
        const auto reference = this->emu->process.processes.reference_of(opened);
        ASSERT_TRUE(reference);
        emulator_thread thread{this->emu->memory};
        thread.id = 8;
        thread.await_objects = {*reference};
        thread.await_any = true;
        EXPECT_FALSE(thread.is_thread_ready(*this->emu));
        EXPECT_EQ(0u, this->manager.explicit_queries);
        this->manager.ready = true;
        EXPECT_TRUE(thread.is_thread_ready(*this->emu));
        EXPECT_EQ(0u, this->manager.explicit_queries);
    }

    TEST_F(ProcessHandleTest, AnUnknownPidDoesNotReopenAStaleGuestHandle)
    {
        const auto original = this->open();
        this->manager.open_error = process_error::invalid_process;
        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> attributes{this->emu->memory, this->storage};
        const emulator_object<CLIENT_ID64> client{this->emu->memory, this->storage + 0x100};
        const emulator_object<handle> output{this->emu->memory, this->storage + 0x200};
        EXPECT_EQ(STATUS_INVALID_CID, syscalls::handle_NtOpenProcess(this->context(), output, 0, attributes, client));
        EXPECT_NE(nullptr, this->emu->process.processes.get(original));
    }

    TEST_F(ProcessHandleTest, DebugEventRetainsIdentityAfterTheOriginalHandleIsClosed)
    {
        const auto original = this->open();
        const auto* process = this->emu->process.processes.get(original);
        ASSERT_NE(nullptr, process);
        debug_object object{};
        object.debuggees.push_back({.process = process->process.value, .identity = this->manager.identity, .thread_ids = {8}});
        const auto debug_handle = this->emu->process.debug_objects.store(std::move(object));
        const auto reference = this->emu->process.reference_of(debug_handle);
        ASSERT_TRUE(reference);
        ASSERT_TRUE(this->emu->process.processes.erase(original));
        this->manager.event = process_debug_event{.kind = process_debug_event_kind::create_process, .thread_id = 8};
        ASSERT_TRUE(deliver_debug_event(*this->emu, reference, this->storage + 0x400));
        const auto change = this->emu->memory.read_memory<DBGUI_WAIT_STATE_CHANGE64>(this->storage + 0x400);
        const auto debug_process = make_handle(change.StateInfo.CreateProcessInfo.HandleToProcess);
        ASSERT_EQ(STATUS_SUCCESS,
                  syscalls::handle_NtQueryInformationProcess(this->context(), debug_process, ProcessBasicInformation, this->storage + 0x800,
                                                             sizeof(PROCESS_BASIC_INFORMATION64), {this->emu->memory}));
        const auto information = this->emu->memory.read_memory<PROCESS_BASIC_INFORMATION64>(this->storage + 0x800);
        EXPECT_EQ(this->manager.identity.parent_process_id, information.InheritedFromUniqueProcessId);
        EXPECT_EQ(this->manager.identity.native_environment, information.PebBaseAddress);
        EXPECT_EQ(this->manager.identity.process_id, information.UniqueProcessId);
    }
}

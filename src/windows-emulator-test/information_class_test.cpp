#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, handle process_handle, uint32_t info_class,
                                              uint64_t process_information, uint32_t process_information_length,
                                              emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtSetInformationProcess(const syscall_context& c, handle process_handle, uint32_t info_class,
                                            uint64_t process_information, uint32_t process_information_length);
    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, uint32_t info_class, uint64_t system_information,
                                             uint32_t system_information_length, emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, handle requested_token,
                                            TOKEN_INFORMATION_CLASS token_information_class, uint64_t token_information,
                                            ULONG token_information_length, emulator_object<ULONG> return_length);
    NTSTATUS handle_NtQueryVirtualMemory(const syscall_context& c, handle process_handle, uint64_t base_address, uint32_t info_class,
                                         uint64_t memory_information, uint64_t memory_information_length,
                                         emulator_object<uint64_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t untouched = 0xAAAAAAAA;
        constexpr uint64_t untouched64 = 0xAAAAAAAAAAAAAAAA;
        constexpr uint64_t bad_pointer = 0x10;
        constexpr handle garbage_handle = make_handle(uint64_t{0x1234});

        struct info_buffer
        {
            uint64_t address;
            uint32_t length;
        };

        class InformationClassTest : public testing::Test
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
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true}));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->event_handle = this->emu->process.events.store(event{});
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            info_buffer data() const
            {
                return {.address = this->storage + 0x100, .length = 0x100};
            }

            template <typename Length = uint32_t>
            emulator_object<Length> poisoned_length() const
            {
                const emulator_object<Length> returned{this->emu->memory, this->storage};
                returned.write(untouched);
                return returned;
            }

            uint32_t returned_length() const
            {
                return emulator_object<uint32_t>{this->emu->memory, this->storage}.read();
            }

            NTSTATUS query_process(const handle process, const uint32_t info_class, const info_buffer target)
            {
                return syscalls::handle_NtQueryInformationProcess(this->context(), process, info_class, target.address, target.length,
                                                                  this->poisoned_length());
            }

            NTSTATUS query_token(const handle token, const uint32_t info_class, const info_buffer target)
            {
                return syscalls::handle_NtQueryInformationToken(this->context(), token, static_cast<TOKEN_INFORMATION_CLASS>(info_class),
                                                                target.address, target.length, this->poisoned_length<ULONG>());
            }

            NTSTATUS query_memory(const handle process, const uint64_t address, const uint32_t info_class, const info_buffer target)
            {
                const emulator_object<uint64_t> returned{this->emu->memory, this->storage + 0x10};
                returned.write(untouched64);
                const auto status = syscalls::handle_NtQueryVirtualMemory(this->context(), process, address, info_class, target.address,
                                                                          target.length, returned);
                this->returned_memory_length = returned.read();
                return status;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            handle event_handle{};
            uint64_t returned_memory_length{};
        };
    }

    TEST_F(InformationClassTest, QueryProcessRejectsClassesPastTheLastOneBeforeLookingAtTheHandle)
    {
        for (const uint32_t info_class : {0x74u, 0x75u, 0x7FFFu, 0xFFFFFFFFu})
        {
            for (const auto process : {CURRENT_PROCESS, handle{}})
            {
                EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_process(process, info_class, this->data())) << info_class;
                EXPECT_EQ(untouched, this->returned_length()) << info_class;
            }
        }

        for (const auto process : {garbage_handle, this->event_handle})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_process(process, 0x7FFF, this->data()));
            EXPECT_EQ(untouched, this->returned_length());
        }

        for (const auto target : {info_buffer{0, 0}, info_buffer{0, 0x100}, info_buffer{bad_pointer, 0x100}})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_process(CURRENT_PROCESS, 0x7FFF, target)) << target.address;
            EXPECT_EQ(untouched, this->returned_length()) << target.address;
        }
    }

    TEST_F(InformationClassTest, SetProcessRejectsClassesPastTheLastOneBeforeLookingAtTheHandle)
    {
        for (const uint32_t info_class : {0x74u, 0x7FFFu, 0xFFFFFFFFu})
        {
            for (const auto process : {CURRENT_PROCESS, handle{}, this->event_handle})
            {
                EXPECT_EQ(STATUS_INVALID_INFO_CLASS, syscalls::handle_NtSetInformationProcess(this->context(), process, info_class, 0, 0))
                    << info_class;
            }
        }

        for (const uint32_t info_class : {0x7FFFu, 0xFFFFFFFFu})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS,
                      syscalls::handle_NtSetInformationProcess(this->context(), CURRENT_PROCESS, info_class, this->data().address, 8))
                << info_class;
        }
    }

    TEST_F(InformationClassTest, QuerySystemRejectsClassesPastTheLastOneWindowsAnswers)
    {
        for (const uint32_t info_class : {0x101u, 0x7FFFu, 0xFFFFu, 0x10000u, 0xFFFFFFFFu})
        {
            for (const auto target : {this->data(), info_buffer{0, 0}})
            {
                EXPECT_EQ(STATUS_INVALID_INFO_CLASS, syscalls::handle_NtQuerySystemInformation(this->context(), info_class, target.address,
                                                                                               target.length, this->poisoned_length()))
                    << info_class;
                EXPECT_EQ(untouched, this->returned_length()) << info_class;
            }
        }
    }

    TEST_F(InformationClassTest, QueryTokenRejectsClassesOutsideTheRangeBeforeLookingAtTheHandle)
    {
        for (const uint32_t info_class : {0u, 0x7FFFu, 0xFFFFFFFFu})
        {
            for (const auto token : {CURRENT_PROCESS_TOKEN, handle{}})
            {
                EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_token(token, info_class, this->data())) << info_class;
                EXPECT_EQ(untouched, this->returned_length()) << info_class;
            }
        }

        for (const auto token : {garbage_handle, this->event_handle})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_token(token, 0x7FFF, this->data()));
            EXPECT_EQ(untouched, this->returned_length());
        }

        for (const uint32_t info_class : {0x3Bu, 0x42u, 0x7FFFu})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_token(CURRENT_PROCESS_TOKEN, info_class, info_buffer{0, 0})) << info_class;
            EXPECT_EQ(untouched, this->returned_length()) << info_class;
        }
    }

    TEST_F(InformationClassTest, QueryVirtualMemoryRejectsClassesPastTheLastOneBeforeAnyOtherArgument)
    {
        const auto address = this->storage;
        for (const uint32_t info_class : {0xFu, 0x10u, 0x7FFFu, 0xFFFFFFFFu})
        {
            for (const auto process : {CURRENT_PROCESS, handle{}})
            {
                EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_memory(process, address, info_class, this->data())) << info_class;
                EXPECT_EQ(untouched64, this->returned_memory_length) << info_class;
            }
        }

        for (const auto target : {info_buffer{0, 0}, info_buffer{this->data().address + 1, 0x100}, info_buffer{bad_pointer, 0x100}})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_memory(CURRENT_PROCESS, address, 0x7FFF, target)) << target.address;
            EXPECT_EQ(untouched64, this->returned_memory_length) << target.address;
        }

        for (const uint64_t base : {uint64_t{0}, ~uint64_t{0} << 20})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_memory(CURRENT_PROCESS, base, 0x7FFF, this->data())) << base;
            EXPECT_EQ(untouched64, this->returned_memory_length) << base;
        }

        for (const auto process : {garbage_handle, this->event_handle})
        {
            EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_memory(process, address, 0x7FFF, this->data()));
            EXPECT_EQ(untouched64, this->returned_memory_length);
        }
    }
}

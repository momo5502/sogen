#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <token.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, handle token_handle, TOKEN_INFORMATION_CLASS information_class,
                                            uint64_t information, ULONG information_length, emulator_object<ULONG> return_length);
    NTSTATUS handle_NtAccessCheck(const syscall_context& c, uint64_t security_descriptor, handle requested_token,
                                  ACCESS_MASK desired_access, emulator_object<EMU_GENERIC_MAPPING> generic_mapping, uint64_t privilege_set,
                                  emulator_object<ULONG> privilege_set_length, emulator_object<ACCESS_MASK> granted_access,
                                  emulator_object<NTSTATUS> access_status);
    NTSTATUS handle_NtImpersonateAnonymousToken(const syscall_context& c, handle thread_handle);
}

namespace sogen::test
{
    namespace
    {
        // S-1-5-4, NT AUTHORITY\INTERACTIVE
        constexpr std::array<uint8_t, 12> interactive_sid{0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00};

        constexpr uint8_t object_inherit_ace = 0x1;
        constexpr ACCESS_MASK generic_read = 0x80000000;
        constexpr ACCESS_MASK generic_all = 0x10000000;
        constexpr ACCESS_MASK file_generic_read = 0x00120089;
        constexpr EMU_GENERIC_MAPPING file_generic_mapping{
            .GenericRead = file_generic_read,
            .GenericWrite = 0x00120116,
            .GenericExecute = 0x001200A0,
            .GenericAll = 0x001F01FF,
        };

        struct test_ace
        {
            uint8_t type{ACCESS_ALLOWED_ACE_TYPE};
            uint8_t flags{};
            ACCESS_MASK mask{};
            std::span<const uint8_t> sid{};
        };

        class ElevatedTokenTest : public testing::TestWithParam<bool>
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                               emulator_settings{.disable_logging = true,
                                                                                 .emulation_root = get_emulator_root(),
                                                                                 .load_registry = false,
                                                                                 .elevated = this->elevated()},
                                                               emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
            }

            bool elevated() const
            {
                return GetParam();
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS query(const handle token, const TOKEN_INFORMATION_CLASS information_class, const ULONG size)
            {
                return syscalls::handle_NtQueryInformationToken(this->context(), token, information_class, this->storage + 0x100, size,
                                                                {this->emu->memory, this->storage});
            }

            ULONG required_size(const handle token, const TOKEN_INFORMATION_CLASS information_class)
            {
                EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, this->query(token, information_class, 0));
                return emulator_object<ULONG>{this->emu->memory, this->storage}.read();
            }

            // A self-relative descriptor owned by LocalSystem whose DACL holds the ACEs in order.
            uint64_t build_descriptor(const std::initializer_list<test_ace> aces)
            {
                const auto descriptor_address = this->storage + 0x1000;
                const auto owner_offset = static_cast<uint32_t>(sizeof(SECURITY_DESCRIPTOR_RELATIVE));
                const auto group_offset = owner_offset + static_cast<uint32_t>(local_system_sid.size());
                const auto dacl_offset = group_offset + static_cast<uint32_t>(local_system_sid.size());

                SECURITY_DESCRIPTOR_RELATIVE descriptor{};
                descriptor.Revision = 1;
                descriptor.Control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
                descriptor.Owner = owner_offset;
                descriptor.Group = group_offset;
                descriptor.Dacl = dacl_offset;
                emulator_object<SECURITY_DESCRIPTOR_RELATIVE>{this->emu->memory, descriptor_address}.write(descriptor);
                this->emu->memory.write_memory(descriptor_address + owner_offset, local_system_sid.data(), local_system_sid.size());
                this->emu->memory.write_memory(descriptor_address + group_offset, local_system_sid.data(), local_system_sid.size());

                auto ace_address = descriptor_address + dacl_offset + sizeof(ACL);
                for (const auto& entry : aces)
                {
                    ACCESS_ALLOWED_ACE ace{};
                    ace.Header.AceType = entry.type;
                    ace.Header.AceFlags = entry.flags;
                    ace.Header.AceSize = static_cast<uint16_t>(offsetof(ACCESS_ALLOWED_ACE, SidStart) + entry.sid.size());
                    ace.Mask = entry.mask;
                    emulator_object<ACCESS_ALLOWED_ACE>{this->emu->memory, ace_address}.write(ace);
                    this->emu->memory.write_memory(ace_address + offsetof(ACCESS_ALLOWED_ACE, SidStart), entry.sid.data(),
                                                   entry.sid.size());
                    ace_address += ace.Header.AceSize;
                }

                ACL acl{};
                acl.AclRevision = 2;
                acl.AclSize = static_cast<uint16_t>(ace_address - (descriptor_address + dacl_offset));
                acl.AceCount = static_cast<uint16_t>(aces.size());
                emulator_object<ACL>{this->emu->memory, descriptor_address + dacl_offset}.write(acl);

                return descriptor_address;
            }

            NTSTATUS check_access(const handle token, const std::initializer_list<test_ace> aces, const ACCESS_MASK desired_access)
            {
                const auto descriptor = this->build_descriptor(aces);

                const emulator_object<EMU_GENERIC_MAPPING> mapping{this->emu->memory, this->storage + 0x800};
                mapping.write(file_generic_mapping);

                const emulator_object<ULONG> privilege_set_length{this->emu->memory, this->storage + 0x820};
                const emulator_object<ACCESS_MASK> granted{this->emu->memory, this->storage + 0x830};
                const emulator_object<NTSTATUS> status{this->emu->memory, this->storage + 0x840};

                const auto result = syscalls::handle_NtAccessCheck(this->context(), descriptor, token, desired_access, mapping, 0,
                                                                   privilege_set_length, granted, status);
                EXPECT_EQ(STATUS_SUCCESS, result);
                return status.read();
            }

            NTSTATUS check_access(const handle token, const std::span<const uint8_t> sid, const ACCESS_MASK mask)
            {
                return this->check_access(token, {test_ace{.mask = mask, .sid = sid}}, mask);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_P(ElevatedTokenTest, GroupsIncludeAdministratorsOnlyWhenElevated)
    {
        const auto size = this->required_size(CURRENT_PROCESS_TOKEN, TokenGroups);
        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_PROCESS_TOKEN, TokenGroups, size));

        const auto groups = emulator_object<TOKEN_GROUPS64>{this->emu->memory, this->storage + 0x100}.read();
        ASSERT_EQ(this->elevated() ? 3u : 2u, groups.GroupCount);

        if (!this->elevated())
        {
            return;
        }

        const auto administrators =
            emulator_object<SID_AND_ATTRIBUTES64>{this->emu->memory,
                                                  this->storage + 0x100 + sizeof(TOKEN_GROUPS64) + sizeof(SID_AND_ATTRIBUTES64)}
                .read();

        // SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED | SE_GROUP_OWNER
        EXPECT_EQ(0xFu, administrators.Attributes);

        std::array<uint8_t, administrators_sid.size()> read_sid{};
        this->emu->memory.read_memory(administrators.Sid, read_sid.data(), read_sid.size());
        EXPECT_EQ(administrators_sid, read_sid);
    }

    TEST_P(ElevatedTokenTest, StatisticsAgreeWithTheGroupList)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_PROCESS_TOKEN, TokenStatistics, sizeof(TOKEN_STATISTICS)));
        const auto stats = emulator_object<TOKEN_STATISTICS>{this->emu->memory, this->storage + 0x100}.read();
        EXPECT_EQ(this->elevated() ? 3u : 2u, stats.GroupCount);
    }

    TEST_P(ElevatedTokenTest, AdministratorsMembershipFollowsTheElevatedFlag)
    {
        const auto expected = this->elevated() ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        EXPECT_EQ(expected, this->check_access(CURRENT_PROCESS_TOKEN, administrators_sid, 1));
    }

    TEST_P(ElevatedTokenTest, AnonymousImpersonationIsNeverElevated)
    {
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtImpersonateAnonymousToken(this->context(), CURRENT_THREAD));

        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenElevation, sizeof(TOKEN_ELEVATION)));
        EXPECT_EQ(0u, (emulator_object<TOKEN_ELEVATION>{this->emu->memory, this->storage + 0x100}.read().TokenIsElevated));

        const auto size = this->required_size(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenIntegrityLevel);
        ASSERT_EQ(STATUS_SUCCESS, this->query(CURRENT_THREAD_EFFECTIVE_TOKEN, TokenIntegrityLevel, size));
        const auto label = emulator_object<TOKEN_MANDATORY_LABEL64>{this->emu->memory, this->storage + 0x100}.read();

        // S-1-16-8192, medium integrity: the anonymous token must not pick up the process elevation.
        EXPECT_EQ(0x20u, this->emu->memory.read_memory<uint8_t>(label.Label.Sid + 9));

        EXPECT_EQ(STATUS_ACCESS_DENIED, this->check_access(CURRENT_THREAD_EFFECTIVE_TOKEN, administrators_sid, 1));
    }

    TEST_P(ElevatedTokenTest, AceMasksAreComparedWithoutGenericMapping)
    {
        EXPECT_EQ(STATUS_ACCESS_DENIED,
                  this->check_access(CURRENT_PROCESS_TOKEN, {{.mask = generic_read, .sid = interactive_sid}}, file_generic_read));
        EXPECT_EQ(STATUS_SUCCESS, this->check_access(CURRENT_PROCESS_TOKEN,
                                                     {{.type = ACCESS_DENIED_ACE_TYPE, .mask = generic_all, .sid = interactive_sid},
                                                      {.mask = file_generic_read, .sid = interactive_sid}},
                                                     file_generic_read));
    }

    TEST_P(ElevatedTokenTest, InheritOnlyAcesAreSkipped)
    {
        constexpr uint8_t inherit_only = object_inherit_ace | INHERIT_ONLY_ACE;
        EXPECT_EQ(STATUS_ACCESS_DENIED,
                  this->check_access(CURRENT_PROCESS_TOKEN, {{.flags = inherit_only, .mask = file_generic_read, .sid = interactive_sid}},
                                     file_generic_read));
        EXPECT_EQ(
            STATUS_SUCCESS,
            this->check_access(CURRENT_PROCESS_TOKEN,
                               {{.type = ACCESS_DENIED_ACE_TYPE, .flags = inherit_only, .mask = file_generic_read, .sid = interactive_sid},
                                {.mask = file_generic_read, .sid = interactive_sid}},
                               file_generic_read));
    }

    INSTANTIATE_TEST_SUITE_P(Elevation, ElevatedTokenTest, testing::Values(false, true),
                             [](const testing::TestParamInfo<bool>& info) { return info.param ? "Elevated" : "Default"; });
}
